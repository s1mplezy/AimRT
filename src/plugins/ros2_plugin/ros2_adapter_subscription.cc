// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "ros2_plugin/ros2_adapter_subscription.h"

#include <array>
#include <charconv>

#include "aimrt_module_cpp_interface/util/type_support.h"
#include "ros2_plugin/global.h"

namespace aimrt::plugins::ros2_plugin {
namespace {

template <typename Integer>
void SetIntegerMeta(
    aimrt::channel::Context& ctx,
    std::string_view key,
    Integer value) {
  std::array<char, 32> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (error == std::errc()) {
    ctx.SetMetaValue(
        key,
        std::string_view(
            buffer.data(), static_cast<size_t>(end - buffer.data())));
  }
}

void SetMessageInfoMeta(
    aimrt::channel::Context& ctx,
    const rclcpp::MessageInfo& message_info) {
  const auto& rmw_info = message_info.get_rmw_message_info();
  SetIntegerMeta(
      ctx, AIMRT_CHANNEL_CONTEXT_KEY_PUB_TIMESTAMP, rmw_info.source_timestamp);
  SetIntegerMeta(
      ctx,
      AIMRT_CHANNEL_CONTEXT_KEY_PUB_SEQ,
      rmw_info.publication_sequence_number);
}

}  // namespace

std::shared_ptr<void> Ros2AdapterSubscription::create_message() {
  return topic_info_.msg_type_support_ref.CreateSharedPtr();
}

std::shared_ptr<rclcpp::SerializedMessage>
Ros2AdapterSubscription::create_serialized_message() {
  return std::make_shared<rclcpp::SerializedMessage>();
}

void Ros2AdapterSubscription::handle_message(
    std::shared_ptr<void>& message, const rclcpp::MessageInfo& message_info) {
  if (!run_flag_.load()) return;
  if (loaned_sub_tool_ptr_ != nullptr) {
    AIMRT_ERROR(
        "ROS2 loaned subscription received a non-loaned sample; "
        "the loaned callback was not invoked.");
  }
  if (sub_tool_ptr_ == nullptr) {
    return;
  }

  try {
    auto ctx_ptr = std::make_shared<aimrt::channel::Context>(aimrt_channel_context_type_t::AIMRT_CHANNEL_SUBSCRIBER_CONTEXT);

    ctx_ptr->SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_PUB_TIMESTAMP, std::to_string(message_info.get_rmw_message_info().source_timestamp));
    ctx_ptr->SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_PUB_SEQ, std::to_string(message_info.get_rmw_message_info().publication_sequence_number));

    sub_tool_ptr_->DoSubscribeCallback(ctx_ptr, *sub_tool_ptr_->FirstSubscribeWrapper(), message);
  } catch (const std::exception& e) {
    AIMRT_ERROR("{}", e.what());
  }
}

void Ros2AdapterSubscription::handle_serialized_message(
    const std::shared_ptr<rclcpp::SerializedMessage>& serialized_message,
    const rclcpp::MessageInfo& message_info) {
  if (!run_flag_.load()) return;
  if (sub_tool_ptr_ == nullptr) return;

  try {
    auto ctx_ptr = std::make_shared<aimrt::channel::Context>(aimrt_channel_context_type_t::AIMRT_CHANNEL_SUBSCRIBER_CONTEXT);
    ctx_ptr->SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_PUB_TIMESTAMP, std::to_string(message_info.get_rmw_message_info().source_timestamp));
    ctx_ptr->SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_PUB_SEQ, std::to_string(message_info.get_rmw_message_info().publication_sequence_number));

    const auto serialization_type = std::string(topic_info_.msg_type_support_ref.DefaultSerializationType());
    const auto& rcl_ser = serialized_message->get_rcl_serialized_message();

    sub_tool_ptr_->DoSubscribeCallback(
        ctx_ptr, serialization_type, static_cast<const void*>(rcl_ser.buffer), rcl_ser.buffer_length);
  } catch (const std::exception& e) {
    AIMRT_ERROR("{}", e.what());
  }
}

void Ros2AdapterSubscription::handle_loaned_message(
    void* loaned_message, const rclcpp::MessageInfo& message_info) {
  if (!run_flag_.load() || loaned_message == nullptr) return;

  try {
    aimrt::channel::Context loaned_ctx(
        aimrt_channel_context_type_t::AIMRT_CHANNEL_SUBSCRIBER_CONTEXT);
    loaned_ctx.SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_BACKEND, "ros2");
    SetMessageInfoMeta(loaned_ctx, message_info);

    if (loaned_sub_tool_ptr_ != nullptr) {
      loaned_sub_tool_ptr_->DoSubscribeCallback(loaned_ctx, loaned_message);
    }

    if (sub_tool_ptr_ != nullptr) {
      auto ctx_ptr = std::make_shared<aimrt::channel::Context>(loaned_ctx);
      auto copied_msg_ptr = topic_info_.msg_type_support_ref.CreateSharedPtr();
      topic_info_.msg_type_support_ref.Copy(loaned_message, copied_msg_ptr.get());
      sub_tool_ptr_->DoSubscribeCallback(
          ctx_ptr, *sub_tool_ptr_->FirstSubscribeWrapper(), copied_msg_ptr);
    }
  } catch (const std::exception& e) {
    AIMRT_ERROR("{}", e.what());
  } catch (...) {
    AIMRT_ERROR("ROS2 loaned subscription callback failed with an unknown exception.");
  }
}

void Ros2AdapterSubscription::return_message(std::shared_ptr<void>& message) {
  message.reset();
}

void Ros2AdapterSubscription::return_serialized_message(
    std::shared_ptr<rclcpp::SerializedMessage>& message) {
  message.reset();
}

}  // namespace aimrt::plugins::ros2_plugin
