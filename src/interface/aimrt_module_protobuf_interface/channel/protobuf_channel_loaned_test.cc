// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "aimrt_module_protobuf_interface/channel/protobuf_channel.h"

#include <google/protobuf/any.pb.h>
#include <gtest/gtest.h>

namespace aimrt::channel {
namespace {

struct FakeChannelState {
  int ordinary_publish_count = 0;
  int ordinary_subscribe_count = 0;
  int borrow_count = 0;
  int loaned_subscribe_count = 0;
};

aimrt_channel_publisher_base_t MakePublisherBase(FakeChannelState& state) {
  return aimrt_channel_publisher_base_t{
      .register_publish_type = [](void*, const aimrt_type_support_base_t*) { return true; },
      .publish = [](void* impl, aimrt_string_view_t,
                    const aimrt_channel_context_base_t*, const void*) { ++static_cast<FakeChannelState*>(impl)->ordinary_publish_count; },
      .get_topic = [](void*) { return aimrt_string_view_t{.str = "topic", .len = 5}; },
      .merge_subscribe_context_to_publish_context =
          [](void*, const aimrt_channel_context_base_t*,
             const aimrt_channel_context_base_t*) {},
      .impl = &state,
      .prepare_loaned_publisher =
          [](void* impl, aimrt_string_view_t,
             aimrt_channel_loaned_publisher_base_t*) {
            ++static_cast<FakeChannelState*>(impl)->borrow_count;
            return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE;
          }};
}

aimrt_channel_subscriber_base_t MakeSubscriberBase(FakeChannelState& state) {
  return aimrt_channel_subscriber_base_t{
      .subscribe = [](void* impl, const aimrt_type_support_base_t*,
                      aimrt_function_base_t*) {
        ++static_cast<FakeChannelState*>(impl)->ordinary_subscribe_count;
        return true; },
      .get_topic = [](void*) { return aimrt_string_view_t{.str = "topic", .len = 5}; },
      .impl = &state,
      .subscribe_loaned =
          [](void* impl, const aimrt_type_support_base_t*,
             aimrt_function_base_t*) {
            ++static_cast<FakeChannelState*>(impl)->loaned_subscribe_count;
            return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE;
          }};
}

TEST(ProtobufChannelLoanedTest, FreeAndProxyPublisherApisPropagateRejection) {
  FakeChannelState state;
  auto publisher_base = MakePublisherBase(state);
  PublisherRef publisher(&publisher_base);

  auto free_publisher = PrepareLoanedPublisher<google::protobuf::Any>(publisher);
  EXPECT_FALSE(free_publisher);
  EXPECT_EQ(free_publisher.Status(),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);

  PublisherProxy<google::protobuf::Any> proxy(publisher);
  auto proxy_loan = proxy.BorrowLoanedMessage();
  EXPECT_FALSE(proxy_loan);
  EXPECT_EQ(proxy_loan.Status(),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);

  EXPECT_EQ(state.borrow_count, 2);
  EXPECT_EQ(state.ordinary_publish_count, 0);
}

TEST(ProtobufChannelLoanedTest, FreeAndProxySubscriberApisDoNotFallback) {
  FakeChannelState state;
  auto subscriber_base = MakeSubscriberBase(state);
  SubscriberRef subscriber(&subscriber_base);

  auto callback = [](ContextRef,
                     const LoanedMessageView<const google::protobuf::Any>&) {};
  EXPECT_EQ(SubscribeLoaned<google::protobuf::Any>(subscriber, callback),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);

  SubscriberProxy<google::protobuf::Any> proxy(subscriber);
  EXPECT_EQ(proxy.SubscribeLoaned(callback),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);

  EXPECT_EQ(state.loaned_subscribe_count, 2);
  EXPECT_EQ(state.ordinary_subscribe_count, 0);
}

}  // namespace
}  // namespace aimrt::channel
