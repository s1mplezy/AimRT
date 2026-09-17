// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <atomic>
#include <future>

#include "aimrt_module_cpp_interface/module_base.h"

namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_client_module {

class NormalRpcSyncClientModule : public aimrt::ModuleBase {
 public:
  ModuleInfo Info() const override { return ModuleInfo{.name = "DdsNormalRpcSyncClientModule"}; }
  bool Initialize(aimrt::CoreRef core) override;
  bool Start() override;
  void Shutdown() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }
  void MainLoop();

  aimrt::CoreRef core_;
  aimrt::executor::ExecutorRef executor_;
  double rpc_frequency_ = 1.0;
  std::atomic_bool run_flag_ = false;
  std::promise<void> stop_signal_;
};

}  // namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_client_module
