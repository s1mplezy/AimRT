// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <memory>

#include "aimrt_module_cpp_interface/module_base.h"
#include "normal_rpc_sync_server_module/service.h"

namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_server_module {

class NormalRpcSyncServerModule : public aimrt::ModuleBase {
 public:
  ModuleInfo Info() const override { return ModuleInfo{.name = "DdsNormalRpcSyncServerModule"}; }
  bool Initialize(aimrt::CoreRef core) override;
  bool Start() override { return true; }
  void Shutdown() override {}

 private:
  aimrt::CoreRef core_;
  std::shared_ptr<CalculatorService> service_;
};

}  // namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_server_module
