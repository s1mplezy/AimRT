// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "loaned_publisher_module/loaned_publisher_module.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

#include "aimrt_module_ros2_interface/channel/ros2_channel.h"
#include "example_ros2/msg/ros_loaned_msg.hpp"
#include "loaned_channel_status.h"
#include "yaml-cpp/yaml.h"

namespace aimrt::examples::cpp::ros2_chn::loaned_publisher_module {

bool LoanedPublisherModule::Initialize(aimrt::CoreRef core) {
  core_ = core;

  try {
    const auto file_path = core_.GetConfigurator().GetConfigFilePath();
    if (!file_path.empty()) {
      const YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
      topic_name_ = cfg_node["topic_name"].as<std::string>();
      channel_frequency_ = cfg_node["channel_frequency"].as<double>();
    }

    executor_ = core_.GetExecutorManager().GetExecutor("work_thread_pool");
    AIMRT_CHECK_ERROR_THROW(
        executor_ && executor_.SupportTimerSchedule(),
        "Get executor 'work_thread_pool' failed.");

    publisher_ = core_.GetChannelHandle().GetPublisher(topic_name_);
    AIMRT_CHECK_ERROR_THROW(
        publisher_, "Get publisher for topic '{}' failed.", topic_name_);
    AIMRT_CHECK_ERROR_THROW(
        aimrt::channel::RegisterPublishType<example_ros2::msg::RosLoanedMsg>(
            publisher_),
        "Register RosLoanedMsg failed.");
  } catch (const std::exception& e) {
    AIMRT_ERROR("Initialize failed: {}", e.what());
    return false;
  }

  AIMRT_INFO("Initialize succeeded.");
  return true;
}

bool LoanedPublisherModule::Start() {
  try {
    run_flag_ = true;
    executor_.Execute(std::bind(&LoanedPublisherModule::MainLoop, this));
  } catch (const std::exception& e) {
    AIMRT_ERROR("Start failed: {}", e.what());
    return false;
  }

  AIMRT_INFO("Start succeeded.");
  return true;
}

void LoanedPublisherModule::Shutdown() {
  try {
    if (run_flag_.exchange(false)) stop_signal_.get_future().wait();
  } catch (const std::exception& e) {
    AIMRT_ERROR("Shutdown failed: {}", e.what());
    return;
  }

  AIMRT_INFO("Shutdown succeeded.");
}

void LoanedPublisherModule::MainLoop() {
  try {
    aimrt::channel::PublisherProxy<example_ros2::msg::RosLoanedMsg>
        publisher_proxy(publisher_);
    const auto period = std::chrono::duration<double>(1.0 / channel_frequency_);
    uint64_t sequence = 0;

    while (run_flag_) {
      std::this_thread::sleep_for(period);
      if (!run_flag_) break;

      auto loaned_message = publisher_proxy.BorrowLoanedMessage();
      AIMRT_CHECK_ERROR_THROW(
          loaned_message,
          "Borrow loaned message failed: {} ({})",
          LoanStatusName(loaned_message.Status()),
          static_cast<int>(loaned_message.Status()));

      loaned_message->sequence = ++sequence;
      loaned_message->data.fill(static_cast<uint8_t>(sequence));

      const auto status = publisher_proxy.Publish(std::move(loaned_message));
      AIMRT_CHECK_ERROR_THROW(
          aimrt::channel::LoanSucceeded(status),
          "Publish loaned message failed: {} ({})",
          LoanStatusName(status),
          static_cast<int>(status));
      AIMRT_INFO("Published loaned message, sequence={}", sequence);
    }
  } catch (const std::exception& e) {
    AIMRT_ERROR("Main loop stopped: {}", e.what());
  }

  run_flag_ = false;
  stop_signal_.set_value();
}

}  // namespace aimrt::examples::cpp::ros2_chn::loaned_publisher_module
