// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <memory>

#include "aimrt_core_plugin_interface/aimrt_core_plugin_base.h"
#include "dds_plugin/dds_config.h"
#include "dds_plugin/dds_runtime.h"

namespace aimrt::plugins::dds_plugin {

class DdsPlugin final : public AimRTCorePluginBase {
 public:
  std::string_view Name() const noexcept override { return "dds_plugin"; }
  bool Initialize(runtime::core::AimRTCore* core_ptr) noexcept override;
  void Shutdown() noexcept override;
  std::list<std::pair<std::string, std::string>> GenInitializationReport() const override;

 private:
  runtime::core::AimRTCore* core_ptr_ = nullptr;
  DdsPluginOptions options_;
  std::shared_ptr<DdsParticipantContext> participant_context_ =
      std::make_shared<DdsParticipantContext>();
  std::shared_ptr<DdsRuntime> runtime_ = std::make_shared<DdsRuntime>();
  bool initialized_ = false;
};

}  // namespace aimrt::plugins::dds_plugin
