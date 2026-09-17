// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <memory>
#include <string>

#include "Example.h"
#include "aimrt_module_cpp_interface/module_base.h"

namespace aimrt::examples::cpp::dds_chn::normal_subscriber_module {

class NormalSubscriberModule : public aimrt::ModuleBase {
 public:
  ModuleInfo Info() const override { return ModuleInfo{.name = "DdsNormalSubscriberModule"}; }
  bool Initialize(aimrt::CoreRef core) override;
  bool Start() override { return true; }
  void Shutdown() override {}

 private:
  auto GetLogger() { return core_.GetLogger(); }
  void HandleMessage(const std::shared_ptr<const aimrt_examples::dds::ChannelMessage>& message);

  aimrt::CoreRef core_;
  aimrt::channel::SubscriberRef subscriber_;
  std::string topic_name_ = "example/dds/channel";
};

}  // namespace aimrt::examples::cpp::dds_chn::normal_subscriber_module
