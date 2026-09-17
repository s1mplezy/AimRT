// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "normal_publisher_module/normal_publisher_module.h"

#include <chrono>
#include <thread>

#include "Example.h"
#include "aimrt_module_dds_interface/channel/dds_channel.h"
#include "yaml-cpp/yaml.h"

namespace aimrt::examples::cpp::dds_chn::normal_publisher_module {

bool NormalPublisherModule::Initialize(aimrt::CoreRef core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    if (!config_path.empty()) {
      const auto config = YAML::LoadFile(std::string(config_path));
      topic_name_ = config["topic_name"].as<std::string>(topic_name_);
      channel_frequency_ = config["channel_frequency"].as<double>(channel_frequency_);
    }
    AIMRT_CHECK_ERROR_THROW(channel_frequency_ > 0.0, "channel_frequency must be positive");
    executor_ = core_.GetExecutorManager().GetExecutor("work_thread_pool");
    AIMRT_CHECK_ERROR_THROW(executor_ && executor_.SupportTimerSchedule(),
                            "Get executor 'work_thread_pool' failed.");
    publisher_ = core_.GetChannelHandle().GetPublisher(topic_name_);
    AIMRT_CHECK_ERROR_THROW(publisher_, "Get publisher for topic '{}' failed.", topic_name_);
    AIMRT_CHECK_ERROR_THROW(
        aimrt::channel::RegisterPublishType<aimrt_examples::dds::ChannelMessage>(publisher_),
        "Register DDS ChannelMessage failed.");
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS publisher initialization failed: {}", error.what());
    return false;
  }
  return true;
}

bool NormalPublisherModule::Start() {
  run_flag_ = true;
  executor_.Execute([this] { MainLoop(); });
  return true;
}

void NormalPublisherModule::Shutdown() {
  if (!run_flag_.exchange(false)) return;
  stop_signal_.get_future().wait();
}

void NormalPublisherModule::MainLoop() {
  aimrt::channel::PublisherProxy<aimrt_examples::dds::ChannelMessage> proxy(publisher_);
  uint32_t sequence = 0;
  while (run_flag_) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<uint32_t>(1000.0 / channel_frequency_)));
    if (!run_flag_) break;
    aimrt_examples::dds::ChannelMessage message;
    message.count(++sequence);
    message.text("hello from AimRT DDS");
    proxy.Publish(message);
    AIMRT_INFO("DDS_CHANNEL_PUBLISHED sequence={} text={}", message.count(), message.text().c_str());
  }
  stop_signal_.set_value();
}

}  // namespace aimrt::examples::cpp::dds_chn::normal_publisher_module
