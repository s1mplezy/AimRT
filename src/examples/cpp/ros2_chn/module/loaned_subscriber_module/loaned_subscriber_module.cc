// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "loaned_subscriber_module/loaned_subscriber_module.h"

#include <functional>

#include "loaned_channel_status.h"
#include "yaml-cpp/yaml.h"

namespace aimrt::examples::cpp::ros2_chn::loaned_subscriber_module {

bool LoanedSubscriberModule::Initialize(aimrt::CoreRef core) {
  core_ = core;

  try {
    const auto file_path = core_.GetConfigurator().GetConfigFilePath();
    if (!file_path.empty()) {
      const YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
      topic_name_ = cfg_node["topic_name"].as<std::string>();
    }

    subscriber_ = core_.GetChannelHandle().GetSubscriber(topic_name_);
    AIMRT_CHECK_ERROR_THROW(
        subscriber_, "Get subscriber for topic '{}' failed.", topic_name_);

    aimrt::channel::SubscriberProxy<example_ros2::msg::RosLoanedMsg>
        subscriber_proxy(subscriber_);
    const auto status = subscriber_proxy.SubscribeLoaned(
        std::bind(
            &LoanedSubscriberModule::HandleLoanedMessage,
            this,
            std::placeholders::_1,
            std::placeholders::_2));
    AIMRT_CHECK_ERROR_THROW(
        aimrt::channel::LoanSucceeded(status),
        "Subscribe loaned message failed: {} ({})",
        LoanStatusName(status),
        static_cast<int>(status));
  } catch (const std::exception& e) {
    AIMRT_ERROR("Initialize failed: {}", e.what());
    return false;
  }

  AIMRT_INFO("Initialize succeeded.");
  return true;
}

void LoanedSubscriberModule::HandleLoanedMessage(
    aimrt::channel::ContextRef,
    const aimrt::channel::LoanedMessageView<
        const example_ros2::msg::RosLoanedMsg>& message) {
  // The view is valid only for this synchronous callback. Do not retain it.
  AIMRT_INFO(
      "Received loaned message, sequence={}, first_byte={}, last_byte={}",
      message->sequence,
      message->data.front(),
      message->data.back());
}

}  // namespace aimrt::examples::cpp::ros2_chn::loaned_subscriber_module
