// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <atomic>
#include <future>

#include "aimrt_module_cpp_interface/module_base.h"

namespace aimrt::examples::cpp::ros2_chn::loaned_publisher_module {

class LoanedPublisherModule : public aimrt::ModuleBase {
 public:
  ModuleInfo Info() const override {
    return ModuleInfo{.name = "LoanedPublisherModule"};
  }

  bool Initialize(aimrt::CoreRef core) override;
  bool Start() override;
  void Shutdown() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }
  void MainLoop();

  aimrt::CoreRef core_;
  aimrt::executor::ExecutorRef executor_;
  aimrt::channel::PublisherRef publisher_;
  std::atomic_bool run_flag_ = false;
  std::promise<void> stop_signal_;
  std::string topic_name_ = "loaned_topic";
  double channel_frequency_ = 1.0;
};

}  // namespace aimrt::examples::cpp::ros2_chn::loaned_publisher_module
