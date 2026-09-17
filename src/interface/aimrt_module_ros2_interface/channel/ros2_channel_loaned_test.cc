// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "aimrt_module_ros2_interface/channel/ros2_channel.h"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <std_msgs/msg/u_int64.hpp>

namespace aimrt::channel {
namespace {

struct FakeChannelState {
  std_msgs::msg::UInt64 msg;
  std::atomic_int prepare_count = 0;
  std::atomic_int release_count = 0;
  int publish_count = 0;
  uint64_t published_value = 0;
};

aimrt_channel_publisher_base_t MakePublisherBase(FakeChannelState& state) {
  return aimrt_channel_publisher_base_t{
      .register_publish_type = [](void*, const aimrt_type_support_base_t*) { return true; },
      .publish = [](void*, aimrt_string_view_t,
                    const aimrt_channel_context_base_t*, const void*) {},
      .get_topic = [](void*) { return aimrt_string_view_t{.str = "topic", .len = 5}; },
      .merge_subscribe_context_to_publish_context =
          [](void*, const aimrt_channel_context_base_t*,
             const aimrt_channel_context_base_t*) {},
      .impl = &state,
      .prepare_loaned_publisher =
          [](void* impl, aimrt_string_view_t,
             aimrt_channel_loaned_publisher_base_t* output) {
            ++static_cast<FakeChannelState*>(impl)->prepare_count;
            *output = aimrt_channel_loaned_publisher_base_t{
                .impl = impl,
                .borrow_loaned_message =
                    [](void* route_impl,
                       aimrt_channel_loaned_message_base_t* loaned_msg) {
                      auto* state_ptr =
                          static_cast<FakeChannelState*>(route_impl);
                      *loaned_msg = aimrt_channel_loaned_message_base_t{
                          .msg_ptr = &state_ptr->msg,
                          .impl = state_ptr,
                          .release = [](void* release_impl, void*) {
                            ++static_cast<FakeChannelState*>(release_impl)
                                  ->release_count;
                            return AIMRT_CHANNEL_LOAN_STATUS_OK;
                          }};
                      return AIMRT_CHANNEL_LOAN_STATUS_OK;
                    },
                .publish_loaned_message =
                    [](void* route_impl,
                       const aimrt_channel_context_base_t*,
                       aimrt_channel_loaned_message_base_t* loaned_msg) {
                      auto* state_ptr =
                          static_cast<FakeChannelState*>(route_impl);
                      ++state_ptr->publish_count;
                      state_ptr->published_value =
                          static_cast<std_msgs::msg::UInt64*>(
                              loaned_msg->msg_ptr)
                              ->data;
                      *loaned_msg = {};
                      return AIMRT_CHANNEL_LOAN_STATUS_OK;
                    }};
            return AIMRT_CHANNEL_LOAN_STATUS_OK;
          }};
}

aimrt_channel_subscriber_base_t MakeSubscriberBase(FakeChannelState& state) {
  return aimrt_channel_subscriber_base_t{
      .subscribe = [](void*, const aimrt_type_support_base_t*,
                      aimrt_function_base_t*) { return true; },
      .get_topic = [](void*) { return aimrt_string_view_t{.str = "topic", .len = 5}; },
      .impl = &state,
      .subscribe_loaned =
          [](void* impl, const aimrt_type_support_base_t*,
             aimrt_function_base_t* callback_base) {
            auto* state_ptr = static_cast<FakeChannelState*>(impl);
            Context ctx(aimrt_channel_context_type_t::AIMRT_CHANNEL_SUBSCRIBER_CONTEXT);
            SubscriberLoanedCallback callback(callback_base);
            callback(ctx.NativeHandle(), &state_ptr->msg);
            return AIMRT_CHANNEL_LOAN_STATUS_OK;
          }};
}

TEST(Ros2ChannelLoanedTest, FreeAndProxyPublisherApisConsumeLoans) {
  FakeChannelState state;
  auto publisher_base = MakePublisherBase(state);
  PublisherRef publisher(&publisher_base);

  {
    auto loaned_publisher =
        PrepareLoanedPublisher<std_msgs::msg::UInt64>(publisher);
    auto abandoned = loaned_publisher.BorrowLoanedMessage();
    ASSERT_TRUE(abandoned);
    abandoned->data = 1;
  }
  EXPECT_EQ(state.release_count.load(), 1);

  auto loaned_publisher =
      PrepareLoanedPublisher<std_msgs::msg::UInt64>(publisher);
  auto loaned_msg = loaned_publisher.BorrowLoanedMessage();
  ASSERT_TRUE(loaned_msg);
  loaned_msg->data = 42;
  EXPECT_EQ(loaned_publisher.Publish(std::move(loaned_msg)),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(state.publish_count, 1);
  EXPECT_EQ(state.published_value, 42u);

  PublisherProxy<std_msgs::msg::UInt64> proxy(publisher);
  auto proxy_loan = proxy.BorrowLoanedMessage();
  ASSERT_TRUE(proxy_loan);
  proxy_loan->data = 73;
  EXPECT_EQ(proxy.Publish(std::move(proxy_loan)),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(state.publish_count, 2);
  EXPECT_EQ(state.published_value, 73u);
}

TEST(Ros2ChannelLoanedTest, ProxyPreparesRouteOnceUnderConcurrency) {
  FakeChannelState state;
  auto publisher_base = MakePublisherBase(state);
  PublisherProxy<std_msgs::msg::UInt64> proxy{PublisherRef(&publisher_base)};

  std::vector<std::thread> threads;
  for (size_t ii = 0; ii < 8; ++ii) {
    threads.emplace_back([&proxy]() {
      auto loaned_msg = proxy.BorrowLoanedMessage();
      EXPECT_TRUE(loaned_msg);
    });
  }
  for (auto& thread : threads) thread.join();

  EXPECT_EQ(state.prepare_count.load(), 1);
  EXPECT_EQ(state.release_count.load(), 8);
}

TEST(Ros2ChannelLoanedTest, SubscriberApiProvidesReadOnlyScopedView) {
  FakeChannelState state;
  state.msg.data = 91;
  auto subscriber_base = MakeSubscriberBase(state);
  SubscriberRef subscriber(&subscriber_base);
  const void* observed_ptr = nullptr;
  uint64_t observed_value = 0;
  EXPECT_EQ(SubscribeLoaned<std_msgs::msg::UInt64>(
                subscriber,
                [&](ContextRef, const LoanedMessageView<const std_msgs::msg::UInt64>& view) {
                  observed_ptr = &*view;
                  observed_value = view->data;
                }),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(observed_ptr, &state.msg);
  EXPECT_EQ(observed_value, 91u);
}

TEST(Ros2ChannelLoanedTest, SubscriberCallbackExceptionDoesNotEscapeCTrampoline) {
  FakeChannelState state;
  auto subscriber_base = MakeSubscriberBase(state);
  SubscriberRef subscriber(&subscriber_base);

  testing::internal::CaptureStderr();
  EXPECT_EQ(
      SubscribeLoaned<std_msgs::msg::UInt64>(
          subscriber,
          [](ContextRef, const LoanedMessageView<const std_msgs::msg::UInt64>&) {
            throw std::runtime_error("callback failure");
          }),
      AIMRT_CHANNEL_LOAN_STATUS_OK);
  const auto error_output = testing::internal::GetCapturedStderr();
  EXPECT_NE(
      error_output.find(
          "AimRT loaned subscriber callback threw an exception: callback failure"),
      std::string::npos);
}

}  // namespace
}  // namespace aimrt::channel
