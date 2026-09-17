// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include "aimrt_module_cpp_interface/module_base.h"
#include "aimrt_module_ros2_interface/channel/ros2_channel.h"
#include "example_ros2/msg/ros_loaned_msg.hpp"

namespace aimrt::examples::cpp::ros2_chn::loaned_subscriber_module {

class LoanedSubscriberModule : public aimrt::ModuleBase {
 public:
  ModuleInfo Info() const override {
    return ModuleInfo{.name = "LoanedSubscriberModule"};
  }

  bool Initialize(aimrt::CoreRef core) override;
  bool Start() override { return true; }
  void Shutdown() override {}

 private:
  auto GetLogger() { return core_.GetLogger(); }
  void HandleLoanedMessage(
      aimrt::channel::ContextRef context,
      const aimrt::channel::LoanedMessageView<
          const example_ros2::msg::RosLoanedMsg>& message);

  aimrt::CoreRef core_;
  aimrt::channel::SubscriberRef subscriber_;
  std::string topic_name_ = "loaned_topic";
};

}  // namespace aimrt::examples::cpp::ros2_chn::loaned_subscriber_module
