// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "normal_subscriber_module/normal_subscriber_module.h"

#include "aimrt_module_dds_interface/channel/dds_channel.h"
#include "yaml-cpp/yaml.h"

namespace aimrt::examples::cpp::dds_chn::normal_subscriber_module {

bool NormalSubscriberModule::Initialize(aimrt::CoreRef core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    if (!config_path.empty()) {
      const auto config = YAML::LoadFile(std::string(config_path));
      topic_name_ = config["topic_name"].as<std::string>(topic_name_);
    }
    subscriber_ = core_.GetChannelHandle().GetSubscriber(topic_name_);
    AIMRT_CHECK_ERROR_THROW(subscriber_, "Get subscriber for topic '{}' failed.", topic_name_);
    AIMRT_CHECK_ERROR_THROW(
        aimrt::channel::Subscribe<aimrt_examples::dds::ChannelMessage>(
            subscriber_, [this](const auto& message) { HandleMessage(message); }),
        "Subscribe DDS ChannelMessage failed.");
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS subscriber initialization failed: {}", error.what());
    return false;
  }
  return true;
}

void NormalSubscriberModule::HandleMessage(
    const std::shared_ptr<const aimrt_examples::dds::ChannelMessage>& message) {
  AIMRT_INFO("DDS_CHANNEL_RECEIVED sequence={} text={}", message->count(), message->text().c_str());
}

}  // namespace aimrt::examples::cpp::dds_chn::normal_subscriber_module
