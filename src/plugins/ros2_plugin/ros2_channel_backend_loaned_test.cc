// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "ros2_plugin/ros2_channel_backend.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <gtest/gtest.h>
#include <rclcpp/version.h>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int64.hpp>

#include "aimrt_module_ros2_interface/util/ros2_type_support.h"

namespace aimrt::plugins::ros2_plugin {
namespace {

template <typename MsgType>
runtime::core::channel::TopicInfo MakeTopicInfo(
    std::string topic_name,
    std::string module_name = "test_module") {
  return runtime::core::channel::TopicInfo{
      .msg_type = std::string("ros2:") + rosidl_generator_traits::name<MsgType>(),
      .topic_name = std::move(topic_name),
      .pkg_path = "test_pkg",
      .module_name = std::move(module_name),
      .index = 1,
      .msg_type_support_ref =
          aimrt::util::TypeSupportRef(GetRos2MessageTypeSupport<MsgType>())};
}

bool SubscriberLoanIsRequiredByTestEnvironment() {
  const char* disable_loan = std::getenv("ROS_DISABLE_LOANED_MESSAGES");
  const char* rmw = std::getenv("RMW_IMPLEMENTATION");
  return disable_loan != nullptr && std::string_view(disable_loan) == "0" &&
         rmw != nullptr && std::string_view(rmw) == "rmw_fastrtps_cpp";
}

class Ros2ChannelBackendLoanedTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    if (!rclcpp::ok()) {
      int argc = 0;
      rclcpp::init(argc, nullptr);
    }
  }

  static void TearDownTestSuite() {
    if (rclcpp::ok()) rclcpp::shutdown();
  }
};

TEST_F(Ros2ChannelBackendLoanedTest, PublisherUsesRclBorrowPublishAndReturn) {
  auto node = std::make_shared<rclcpp::Node>("aimrt_loaned_publisher_test");
  Ros2ChannelBackend backend;
  backend.SetNodePtr(node);
  backend.Initialize(YAML::Node());

  runtime::core::channel::PublishTypeWrapper publish_type{
      .info = MakeTopicInfo<std_msgs::msg::UInt64>("loaned_publish_topic")};
  ASSERT_TRUE(backend.RegisterPublishType(publish_type));
  backend.Start();

  runtime::core::channel::BackendLoanedPublisher route;
  auto status = backend.PrepareLoanedPublisher(publish_type, route);
  if (status == AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN) {
    backend.Shutdown();
    GTEST_SKIP() << "The selected publisher runtime cannot loan messages.";
  }
  ASSERT_EQ(status, AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt_channel_loaned_message_base_t abandoned{};
  ASSERT_EQ(route.borrow(route.impl, abandoned), AIMRT_CHANNEL_LOAN_STATUS_OK);
  ASSERT_NE(abandoned.msg_ptr, nullptr);
  abandoned.release(abandoned.impl, abandoned.msg_ptr);
  abandoned = {};

  aimrt_channel_loaned_message_base_t loaned_msg{};
  ASSERT_EQ(route.borrow(route.impl, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  static_cast<std_msgs::msg::UInt64*>(loaned_msg.msg_ptr)->data = 42;
  aimrt::channel::Context ctx;
  EXPECT_EQ(route.publish(route.impl, ctx, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(loaned_msg.msg_ptr, nullptr);
  EXPECT_EQ(loaned_msg.release, nullptr);
  backend.Shutdown();
}

TEST_F(Ros2ChannelBackendLoanedTest, PublisherSerializesConcurrentBorrowAndReturn) {
  auto node = std::make_shared<rclcpp::Node>("aimrt_concurrent_loaned_publisher_test");
  Ros2ChannelBackend backend;
  backend.SetNodePtr(node);
  backend.Initialize(YAML::Node());

  runtime::core::channel::PublishTypeWrapper publish_type{
      .info = MakeTopicInfo<std_msgs::msg::UInt64>("concurrent_loaned_publish_topic")};
  ASSERT_TRUE(backend.RegisterPublishType(publish_type));
  backend.Start();

  runtime::core::channel::BackendLoanedPublisher route;
  const auto status = backend.PrepareLoanedPublisher(publish_type, route);
  if (status == AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN) {
    backend.Shutdown();
    GTEST_SKIP() << "The selected publisher runtime cannot loan messages.";
  }
  ASSERT_EQ(status, AIMRT_CHANNEL_LOAN_STATUS_OK);

  constexpr size_t kThreadCount = 8;
  constexpr size_t kIterationsPerThread = 100;
  std::atomic_size_t ready_count = 0;
  std::atomic_bool start = false;
  std::atomic_size_t failure_count = 0;
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (size_t ii = 0; ii < kThreadCount; ++ii) {
    threads.emplace_back([&]() {
      ready_count.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();

      for (size_t jj = 0; jj < kIterationsPerThread; ++jj) {
        aimrt_channel_loaned_message_base_t loaned_msg{};
        if (route.borrow(route.impl, loaned_msg) != AIMRT_CHANNEL_LOAN_STATUS_OK) {
          failure_count.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        loaned_msg.release(loaned_msg.impl, loaned_msg.msg_ptr);
      }
    });
  }

  while (ready_count.load(std::memory_order_acquire) != kThreadCount)
    std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();

  EXPECT_EQ(failure_count.load(), 0u);
  backend.Shutdown();
}

TEST_F(Ros2ChannelBackendLoanedTest, AimrtLoanPublisherToSubscriberUsesRmwLoanPath) {
  auto node = std::make_shared<rclcpp::Node>("aimrt_loaned_end_to_end_test");
  Ros2ChannelBackend backend;
  backend.SetNodePtr(node);
  backend.Initialize(YAML::Node());

  const auto topic_info =
      MakeTopicInfo<std_msgs::msg::UInt64>("loaned_end_to_end_topic");
  std::atomic_bool received = false;
  std::atomic_uint64_t received_value = 0;
  runtime::core::channel::LoanedSubscribeWrapper subscribe_wrapper{
      .info = topic_info,
      .callback = [&](aimrt::channel::ContextRef, const void* msg_ptr) {
        received_value.store(
            static_cast<const std_msgs::msg::UInt64*>(msg_ptr)->data);
        received.store(true);
      }};
  auto subscribe_status = backend.SubscribeLoaned(subscribe_wrapper);
  if (subscribe_status == AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN) {
    backend.Shutdown();
    if (SubscriberLoanIsRequiredByTestEnvironment())
      FAIL() << "Subscriber loan was required by the test environment but is unavailable.";
    GTEST_SKIP() << "Subscriber loan is disabled. Set ROS_DISABLE_LOANED_MESSAGES=0 before process startup and use a loan-capable RMW.";
  }
  ASSERT_EQ(subscribe_status, AIMRT_CHANNEL_LOAN_STATUS_OK);

  runtime::core::channel::PublishTypeWrapper publish_type{.info = topic_info};
  ASSERT_TRUE(backend.RegisterPublishType(publish_type));
  backend.Start();

  runtime::core::channel::BackendLoanedPublisher route;
  auto prepare_status = backend.PrepareLoanedPublisher(publish_type, route);
  if (prepare_status == AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN) {
    backend.Shutdown();
    GTEST_SKIP() << "Publisher runtime cannot loan messages.";
  }
  ASSERT_EQ(prepare_status, AIMRT_CHANNEL_LOAN_STATUS_OK);

  aimrt_channel_loaned_message_base_t loaned_msg{};
  ASSERT_EQ(route.borrow(route.impl, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  static_cast<std_msgs::msg::UInt64*>(loaned_msg.msg_ptr)->data = 123;
  aimrt::channel::Context ctx;
  ASSERT_EQ(route.publish(route.impl, ctx, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_OK);

  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);
  for (size_t ii = 0; ii < 100 && !received.load(); ++ii) {
    executor.spin_some();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(received.load());
  EXPECT_EQ(received_value.load(), 123u);
  executor.remove_node(node);
  backend.Shutdown();
}

TEST_F(Ros2ChannelBackendLoanedTest, PublisherUsesRclCapabilityForUnsupportedType) {
  auto node = std::make_shared<rclcpp::Node>("aimrt_unbounded_loan_test");
  Ros2ChannelBackend backend;
  backend.SetNodePtr(node);
  backend.Initialize(YAML::Node());

  runtime::core::channel::PublishTypeWrapper publish_type{
      .info = MakeTopicInfo<std_msgs::msg::String>("unbounded_publish_topic")};
  ASSERT_TRUE(backend.RegisterPublishType(publish_type));
  backend.Start();
  runtime::core::channel::BackendLoanedPublisher route;
  EXPECT_EQ(backend.PrepareLoanedPublisher(publish_type, route),
            AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN);
  EXPECT_EQ(route.impl, nullptr);
  backend.Shutdown();
}

TEST_F(Ros2ChannelBackendLoanedTest, SerializedPublisherConfigIsRejected) {
  auto node = std::make_shared<rclcpp::Node>("aimrt_serialized_loan_test");
  YAML::Node options;
  YAML::Node pub_option;
  pub_option["topic_name"] = "serialized_publish_topic";
  pub_option["use_serialized"] = true;
  options["pub_topics_options"].push_back(pub_option);

  Ros2ChannelBackend backend;
  backend.SetNodePtr(node);
  backend.Initialize(options);
  runtime::core::channel::PublishTypeWrapper publish_type{
      .info = MakeTopicInfo<std_msgs::msg::UInt64>("serialized_publish_topic")};
  ASSERT_TRUE(backend.RegisterPublishType(publish_type));
  backend.Start();
  runtime::core::channel::BackendLoanedPublisher route;
  EXPECT_EQ(backend.PrepareLoanedPublisher(publish_type, route),
            AIMRT_CHANNEL_LOAN_STATUS_INCOMPATIBLE_BACKEND_CONFIG);
  backend.Shutdown();
}

TEST_F(Ros2ChannelBackendLoanedTest, SubscriberReceivesRmwLoanedSampleDirectly) {
  auto node = std::make_shared<rclcpp::Node>("aimrt_loaned_subscriber_test");
  Ros2ChannelBackend backend;
  backend.SetNodePtr(node);
  backend.Initialize(YAML::Node());

  std::atomic_bool received = false;
  std::atomic_uint64_t received_value = 0;
  std::atomic<const void*> received_ptr = nullptr;
  runtime::core::channel::LoanedSubscribeWrapper subscribe_wrapper{
      .info = MakeTopicInfo<std_msgs::msg::UInt64>("loaned_subscribe_topic"),
      .callback = [&](aimrt::channel::ContextRef, const void* msg_ptr) {
        received_ptr.store(msg_ptr);
        received_value.store(
            static_cast<const std_msgs::msg::UInt64*>(msg_ptr)->data);
        received.store(true);
      }};

  auto status = backend.SubscribeLoaned(subscribe_wrapper);
  if (status == AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN) {
    backend.Shutdown();
    if (SubscriberLoanIsRequiredByTestEnvironment())
      FAIL() << "Subscriber loan was required by the test environment but is unavailable.";
    GTEST_SKIP() << "Subscriber loan is disabled. Set ROS_DISABLE_LOANED_MESSAGES=0 before process startup and use a loan-capable RMW.";
  }
  ASSERT_EQ(status, AIMRT_CHANNEL_LOAN_STATUS_OK);
  backend.Start();

  auto publisher = node->create_publisher<std_msgs::msg::UInt64>(
      "loaned_subscribe_topic", 10);
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);

  for (size_t ii = 0; ii < 100 && !received.load(); ++ii) {
    std_msgs::msg::UInt64 msg;
    msg.data = 73;
    publisher->publish(msg);
    executor.spin_some();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_TRUE(received.load());
  EXPECT_EQ(received_value.load(), 73u);
  EXPECT_NE(received_ptr.load(), nullptr);
  executor.remove_node(node);
  backend.Shutdown();
}

TEST_F(Ros2ChannelBackendLoanedTest, AdapterDeliversExactLoanPointerAndCopiesForOrdinarySubscriber) {
  auto node = std::make_shared<rclcpp::Node>("aimrt_loaned_adapter_test");
  auto topic_info = MakeTopicInfo<std_msgs::msg::UInt64>("loaned_adapter_topic");

  const void* loaned_callback_ptr = nullptr;
  const void* ordinary_callback_ptr = nullptr;
  uint64_t ordinary_value = 0;
  runtime::core::channel::LoanedSubscribeWrapper loaned_wrapper{
      .info = topic_info,
      .callback = [&](aimrt::channel::ContextRef, const void* msg_ptr) {
        loaned_callback_ptr = msg_ptr;
      }};
  runtime::core::channel::SubscribeWrapper ordinary_wrapper{
      .info = topic_info,
      .callback = [&](runtime::core::channel::MsgWrapper& msg_wrapper,
                      std::function<void()>&& release_callback) {
        ordinary_callback_ptr = msg_wrapper.msg_ptr;
        ordinary_value =
            static_cast<const std_msgs::msg::UInt64*>(msg_wrapper.msg_ptr)->data;
        release_callback();
      }};
  runtime::core::channel::LoanedSubscribeTool loaned_tool;
  loaned_tool.AddSubscribeWrapper(&loaned_wrapper);
  runtime::core::channel::SubscribeTool ordinary_tool;
  ordinary_tool.AddSubscribeWrapper(&ordinary_wrapper);

  rclcpp::QoS qos(10);
  const rclcpp::SubscriptionOptionsWithAllocator<std::allocator<void>> options;
  Ros2AdapterSubscription adapter(
      node->get_node_base_interface().get(),
      *static_cast<const rosidl_message_type_support_t*>(
          topic_info.msg_type_support_ref.CustomTypeSupportPtr()),
      "loaned_adapter_topic",
#if RCLCPP_VERSION_MAJOR == 16
      options.to_rcl_subscription_options<void>(qos),
#elif RCLCPP_VERSION_MAJOR == 28
      options.to_rcl_subscription_options(qos),
#endif
      topic_info,
      &ordinary_tool,
      &loaned_tool,
      false);
  adapter.Start();

  std_msgs::msg::UInt64 rmw_loaned_sample;
  rmw_loaned_sample.data = 91;
  rclcpp::MessageInfo message_info;
  adapter.handle_loaned_message(&rmw_loaned_sample, message_info);

  EXPECT_EQ(loaned_callback_ptr, &rmw_loaned_sample);
  EXPECT_NE(ordinary_callback_ptr, &rmw_loaned_sample);
  EXPECT_EQ(ordinary_value, 91u);
  adapter.Shutdown();
}

}  // namespace
}  // namespace aimrt::plugins::ros2_plugin
