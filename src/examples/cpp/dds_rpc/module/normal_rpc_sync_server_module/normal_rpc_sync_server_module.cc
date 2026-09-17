// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "normal_rpc_sync_server_module/normal_rpc_sync_server_module.h"

namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_server_module {

bool NormalRpcSyncServerModule::Initialize(aimrt::CoreRef core) {
  core_ = core;
  service_ = std::make_shared<CalculatorService>();
  return core_.GetRpcHandle().RegisterService(service_.get());
}

}  // namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_server_module
