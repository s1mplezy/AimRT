// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "dds_plugin/dds_plugin.h"

#include "core/aimrt_core.h"
#include "dds_plugin/dds_backend.h"
#include "dds_plugin/global.h"
#include "util/log_util.h"

namespace aimrt::plugins::dds_plugin {

bool DdsPlugin::Initialize(runtime::core::AimRTCore* core_ptr) noexcept {
  try {
    if (core_ptr == nullptr) throw std::invalid_argument("DDS plugin requires a non-null AimRTCore");
    if (initialized_) throw std::logic_error("DDS plugin cannot be initialized twice");
    core_ptr_ = core_ptr;

    const auto plugin_options_node = core_ptr_->GetPluginManager().GetPluginOptionsNode(Name());
    options_ = DecodeDdsPluginOptions(plugin_options_node);
    participant_context_->Initialize(options_);
    runtime_->Initialize(*participant_context_);

    auto backend_state = std::make_shared<DdsBackendState>(*participant_context_, *runtime_);
    core_ptr_->GetChannelManager().RegisterChannelBackend(
        std::make_unique<DdsChannelBackend>(backend_state));
    core_ptr_->GetRpcManager().RegisterRpcBackend(
        std::make_unique<DdsRpcBackend>(std::move(backend_state)));

    core_ptr_->RegisterHookFunc(runtime::core::AimRTCore::State::kPostInitLog, [core_ptr] {
      SetLogger(aimrt::logger::LoggerRef(core_ptr->GetLoggerManager().GetLoggerProxy().NativeHandle()));
    });
    const auto participant_context = participant_context_;
    const auto runtime = runtime_;
    core_ptr_->RegisterHookFunc(runtime::core::AimRTCore::State::kPreStartPlugin,
                                [participant_context, runtime] {
                                  participant_context->Start();
                                  runtime->Start();
                                });
    core_ptr_->RegisterHookFunc(runtime::core::AimRTCore::State::kPreShutdownPlugin,
                                [participant_context, runtime] {
                                  runtime->Shutdown();
                                  participant_context->Shutdown();
                                });

    core_ptr_->GetPluginManager().UpdatePluginOptionsNode(Name(), EncodeDdsPluginOptions(options_));
    initialized_ = true;
    return true;
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS plugin Initialize failed: {}", error.what());
    try {
      runtime_->Shutdown();
      participant_context_->Shutdown();
    } catch (...) {
    }
    core_ptr_ = nullptr;
    initialized_ = false;
    return false;
  }
}

void DdsPlugin::Shutdown() noexcept {
  try {
    runtime_->Shutdown();
    participant_context_->Shutdown();
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS plugin Shutdown failed: {}", error.what());
  }
  initialized_ = false;
  core_ptr_ = nullptr;
  SetLogger(aimrt::logger::GetSimpleLoggerRef());
}

std::list<std::pair<std::string, std::string>> DdsPlugin::GenInitializationReport() const {
  auto report = participant_context_->GenInitializationReport();
  report.emplace_back("DDS Runtime", "state=" + std::to_string(static_cast<uint32_t>(runtime_->State())) +
                                         ", publisher_count=1, subscriber_count=1, readers=" +
                                         std::to_string(runtime_->Endpoints().ReaderCount()) +
                                         ", writers=" +
                                         std::to_string(runtime_->Endpoints().WriterCount()));
  report.emplace_back("DDS RPC Backend", "registered=true, endpoints=rpc, response_cft_ownership=per_logical_client");
  return report;
}

}  // namespace aimrt::plugins::dds_plugin
