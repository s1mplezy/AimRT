// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <atomic>
#include <future>
#include <string>

#include "aimrt_module_cpp_interface/module_base.h"

namespace aimrt::examples::cpp::dds_chn::normal_publisher_module {

class NormalPublisherModule : public aimrt::ModuleBase {
 public:
  ModuleInfo Info() const override { return ModuleInfo{.name = "DdsNormalPublisherModule"}; }
  bool Initialize(aimrt::CoreRef core) override;
  bool Start() override;
  void Shutdown() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }
  void MainLoop();

  aimrt::CoreRef core_;
  aimrt::executor::ExecutorRef executor_;
  aimrt::channel::PublisherRef publisher_;
  std::string topic_name_ = "example/dds/channel";
  double channel_frequency_ = 2.0;
  std::atomic_bool run_flag_ = false;
  std::promise<void> stop_signal_;
};

}  // namespace aimrt::examples::cpp::dds_chn::normal_publisher_module
