// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "normal_rpc_sync_client_module/normal_rpc_sync_client_module.h"

#include <chrono>
#include <thread>

#include "Example.h"
#include "yaml-cpp/yaml.h"

namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_client_module {

bool NormalRpcSyncClientModule::Initialize(aimrt::CoreRef core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    if (!config_path.empty()) {
      const auto config = YAML::LoadFile(std::string(config_path));
      rpc_frequency_ = config["rpc_frequency"].as<double>(rpc_frequency_);
    }
    AIMRT_CHECK_ERROR_THROW(rpc_frequency_ > 0.0, "rpc_frequency must be positive");
    executor_ = core_.GetExecutorManager().GetExecutor("work_thread_pool");
    AIMRT_CHECK_ERROR_THROW(executor_ && executor_.SupportTimerSchedule(),
                            "Get executor 'work_thread_pool' failed.");
    AIMRT_CHECK_ERROR_THROW(
        aimrt_examples::dds::RegisterCalculatorClientFunc(core_.GetRpcHandle()),
        "Register DDS Calculator client failed.");
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS RPC client initialization failed: {}", error.what());
    return false;
  }
  return true;
}

bool NormalRpcSyncClientModule::Start() {
  run_flag_ = true;
  executor_.Execute([this] { MainLoop(); });
  return true;
}

void NormalRpcSyncClientModule::Shutdown() {
  if (!run_flag_.exchange(false)) return;
  stop_signal_.get_future().wait();
}

void NormalRpcSyncClientModule::MainLoop() {
  aimrt_examples::dds::CalculatorSyncProxy proxy(core_.GetRpcHandle());
  int32_t value = 0;
  while (run_flag_) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<uint32_t>(1000.0 / rpc_frequency_)));
    if (!run_flag_) break;
    aimrt_examples::dds::AddRequest request;
    aimrt_examples::dds::AddResponse response;
    request.lhs(++value);
    request.rhs(100);
    auto context = proxy.NewContextSharedPtr();
    context->SetTimeout(std::chrono::seconds(3));
    const auto status = proxy.Add(context, request, response);
    if (status.OK()) {
      AIMRT_INFO("DDS_RPC_RESPONSE lhs={} rhs={} sum={}", request.lhs(), request.rhs(), response.sum());
    } else {
      AIMRT_DEBUG("DDS RPC server is not ready yet: {}", status.ToString());
    }
  }
  stop_signal_.set_value();
}

}  // namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_client_module
