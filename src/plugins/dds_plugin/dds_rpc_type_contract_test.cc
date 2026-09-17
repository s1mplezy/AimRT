// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "dds_plugin/dds_backend.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#include <gtest/gtest.h>
#include <unistd.h>

#include "Calculator.h"
#include "aimrt_module_cpp_interface/co/sync_wait.h"
#include "aimrt_module_cpp_interface/module_base.h"
#include "aimrt_module_dds_interface/channel/dds_channel.h"
#include "core/aimrt_core.h"

namespace aimrt::plugins::dds_plugin {
namespace {

bool WaitFor(std::function<bool()> predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

runtime::core::rpc::FuncInfo MakeFunctionInfo(const void* function_support) {
  return runtime::core::rpc::FuncInfo{
      .func_name = "dds:/example::Calculator/Add",
      .pkg_path = "test",
      .module_name = "test",
      .index = 1,
      .custom_type_support_ptr = function_support,
      .req_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetDdsMessageTypeSupport<example::AddRequest>()),
      .rsp_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetDdsMessageTypeSupport<example::AddResponse>())};
}

class CoreRegistrationModule final : public aimrt::ModuleBase {
 public:
  aimrt::ModuleInfo Info() const override {
    return {.name = "dds_core_registration_smoke"};
  }

  bool Initialize(aimrt::CoreRef core) override {
    auto publisher = core.GetChannelHandle().GetPublisher("business/dds_smoke");
    if (!publisher ||
        !aimrt::channel::RegisterPublishType<example::AddRequest>(publisher)) {
      return false;
    }
    auto subscriber = core.GetChannelHandle().GetSubscriber("business/dds_smoke");
    if (!subscriber ||
        !aimrt::channel::Subscribe<example::AddRequest>(
            subscriber, [](const std::shared_ptr<const example::AddRequest>&) {})) {
      return false;
    }
    rpc_ = core.GetRpcHandle();
    return rpc_.RegisterService(&service_) &&
           example::RegisterCalculatorClientFunc(rpc_);
  }

  bool Start() override {
    using namespace std::chrono_literals;
    example::AddRequest request;
    request.lhs(20);
    request.rhs(22);

    example::CalculatorSyncProxy sync_proxy(rpc_);
    example::AddResponse sync_response;
    aimrt::rpc::Status sync_status(AIMRT_RPC_STATUS_SVR_NOT_FOUND);
    const auto ready_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < ready_deadline) {
      aimrt::rpc::Context context;
      context.SetTimeout(2s);
      sync_status = sync_proxy.Add(context, request, sync_response);
      if (sync_status.OK()) break;
      if (sync_status.Code() != AIMRT_RPC_STATUS_SVR_NOT_FOUND) return false;
      std::this_thread::sleep_for(10ms);
    }
    if (!sync_status.OK() || sync_response.sum() != 42) return false;
    successful_calls_.fetch_add(1);

    example::CalculatorAsyncProxy async_proxy(rpc_);
    example::AddResponse async_response;
    aimrt::rpc::Context async_context;
    async_context.SetTimeout(2s);
    std::promise<aimrt::rpc::Status> async_promise;
    auto async_future = async_promise.get_future();
    async_proxy.Add(async_context, request, async_response,
                    [&async_promise](aimrt::rpc::Status status) {
                      async_promise.set_value(std::move(status));
                    });
    if (async_future.wait_for(3s) != std::future_status::ready ||
        !async_future.get().OK() || async_response.sum() != 42) {
      return false;
    }
    successful_calls_.fetch_add(1);

    example::CalculatorFutureProxy future_proxy(rpc_);
    example::AddResponse future_response;
    aimrt::rpc::Context future_context;
    future_context.SetTimeout(2s);
    auto future_status = future_proxy.Add(future_context, request, future_response);
    if (future_status.wait_for(3s) != std::future_status::ready ||
        !future_status.get().OK() || future_response.sum() != 42) {
      return false;
    }
    successful_calls_.fetch_add(1);

    example::CalculatorCoProxy co_proxy(rpc_);
    example::AddResponse co_response;
    aimrt::rpc::Context co_context;
    co_context.SetTimeout(2s);
    auto co_status = aimrt::co::SyncWait(co_proxy.Add(co_context, request, co_response));
    if (!co_status || !co_status->OK() || co_response.sum() != 42) return false;
    successful_calls_.fetch_add(1);
    return true;
  }
  void Shutdown() override {}

  uint32_t SuccessfulCalls() const noexcept { return successful_calls_.load(); }

 private:
  class Service final : public example::CalculatorSyncService {
   public:
    aimrt::rpc::Status Add(aimrt::rpc::ContextRef,
                           const example::AddRequest& request,
                           example::AddResponse& response) override {
      response.sum(request.lhs() + request.rhs());
      return aimrt::rpc::Status();
    }
  } service_;
  aimrt::rpc::RpcHandleRef rpc_;
  std::atomic_uint32_t successful_calls_ = 0;
};

class TemporaryCoreConfig {
 public:
  explicit TemporaryCoreConfig(std::string_view plugin_path)
      : path_(std::filesystem::temp_directory_path() /
              ("aimrt_dds_core_registration_" + std::to_string(getpid()) + ".yaml")) {
    std::ofstream output(path_, std::ios::binary);
    output << "aimrt:\n"
              "  plugin:\n"
              "    plugins:\n"
              "      - name: dds_plugin\n"
              "        path: "
           << plugin_path
           << "\n"
              "        options:\n"
              "          domain_id: 226\n"
              "          participant_name: aimrt_dds_core_registration\n"
              "  channel:\n"
              "    backends:\n"
              "      - type: dds\n"
              "        options: {}\n"
              "    pub_topics_options:\n"
              "      - topic_name: business/dds_smoke\n"
              "        enable_backends: [dds]\n"
              "    sub_topics_options:\n"
              "      - topic_name: business/dds_smoke\n"
              "        enable_backends: [dds]\n"
              "  rpc:\n"
              "    backends:\n"
              "      - type: dds\n"
              "        options: {}\n"
              "    clients_options:\n"
              "      - func_name: dds:/example::Calculator/Add\n"
              "        enable_backends: [dds]\n"
              "    servers_options:\n"
              "      - func_name: dds:/example::Calculator/Add\n"
              "        enable_backends: [dds]\n"
              "  log:\n"
              "    core_lvl: Error\n"
              "    backends:\n"
              "      - type: console\n";
  }

  ~TemporaryCoreConfig() { std::filesystem::remove(path_); }
  std::string Path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

TEST(DdsRpcTypeContract, RejectsFunctionSupportCopiesMessageHandlesAndKeepsSourcesAlive) {
  const auto* request_source = &aimrt::GetStaticDdsTypeSupportHandle<example::AddRequest>();
  const auto* response_source = &aimrt::GetStaticDdsTypeSupportHandle<example::AddResponse>();
  ASSERT_TRUE(*request_source);
  ASSERT_TRUE(*response_source);

  DdsParticipantContext context;
  context.Initialize(DdsPluginOptions{.domain_id = 227, .participant_name = "aimrt_dds_b07"});
  {
    DdsRuntime runtime;
    runtime.Initialize(context);
    auto state = std::make_shared<DdsBackendState>(context, runtime);
    DdsRpcBackend backend(state);
    backend.Initialize(YAML::Load("{}"));

    int forbidden_function_support = 0;
    runtime::core::rpc::ClientFuncWrapper rejected{
        .info = MakeFunctionInfo(&forbidden_function_support)};
    EXPECT_FALSE(backend.RegisterClientFunc(rejected));
    EXPECT_TRUE(state->type_support_handles.empty());

    runtime::core::rpc::ClientFuncWrapper accepted{.info = MakeFunctionInfo(nullptr)};
    ASSERT_TRUE(backend.RegisterClientFunc(accepted));
    ASSERT_EQ(state->type_support_handles.size(), 2U);
    EXPECT_EQ(state->type_support_handles[0].get(), request_source->get());
    EXPECT_EQ(state->type_support_handles[1].get(), response_source->get());
    EXPECT_EQ(aimrt::GetDdsMessageTypeSupport<example::AddRequest>()->custom_type_support_ptr(
                  aimrt::GetDdsMessageTypeSupport<example::AddRequest>()->impl),
              request_source);
  }
  // Destruction of backend-owned handle copies must not delete or mutate the
  // static message-level source pointers.
  EXPECT_TRUE(*request_source);
  EXPECT_TRUE(*response_source);
  EXPECT_EQ(request_source->get()->get_name(), "example::AddRequest");
  context.Shutdown();
}

TEST(DdsCoreBackendIntegration, CoreInitRegistersOrdinaryChannelAndRpcThroughDds) {
  const char* plugin_path = std::getenv("AIMRT_DDS_PLUGIN_PATH");
  ASSERT_NE(plugin_path, nullptr);
  TemporaryCoreConfig config(plugin_path);
  CoreRegistrationModule module;
  runtime::core::AimRTCore core;
  core.GetModuleManager().RegisterModule(module.NativeHandle());
  ASSERT_NO_THROW(core.Initialize({.cfg_file_path = config.Path()}));
  const auto report = core.GenInitializationReport();
  EXPECT_NE(report.find("DDS Channel Backend"), std::string::npos);
  EXPECT_NE(report.find("DDS RPC Backend"), std::string::npos);
  EXPECT_NE(report.find("dds.rpc.reply_target_mode\n"
                        "explicit_response_reader_guid_in_related_sample_identity"),
            std::string::npos);
  EXPECT_NE(report.find("dds.rpc.fastdds_compatibility\n"
                        "fastdds_3.6.2_dd66ef2a_explicit_identity"),
            std::string::npos);
  EXPECT_EQ(report.find("dds.rpc.compatibility"), std::string::npos);
  auto completion = core.AsyncStart();
  ASSERT_TRUE(WaitFor([&module] { return module.SuccessfulCalls() == 4; },
                      std::chrono::seconds(10)));
  core.Shutdown();
  EXPECT_EQ(completion.wait_for(std::chrono::seconds(5)), std::future_status::ready);
}

}  // namespace
}  // namespace aimrt::plugins::dds_plugin
