// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "dds_plugin/dds_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include <fastdds/dds/subscriber/DataReader.hpp>

#include "Calculator.h"
#include "dds_plugin/dds_test_process.h"
#include "dds_plugin/serialized_message_adapter.h"
#if defined(AIMRT_BUILD_WITH_PROTOBUF)
  #include "DdsRpcWrapper.pb.h"
  #include "aimrt_module_protobuf_interface/util/protobuf_type_support.h"
#endif
#if defined(AIMRT_BUILD_WITH_ROS2)
  #include "aimrt_module_ros2_interface/util/ros2_type_support.h"
  #include "std_msgs/msg/int32.hpp"
#endif

namespace aimrt::plugins::dds_plugin {
namespace {

using namespace std::chrono_literals;

runtime::core::rpc::FuncInfo MakeRpcInfo(
    std::string module_name, uint64_t index,
    std::string func_name = "dds:/example::Calculator/Add") {
  return runtime::core::rpc::FuncInfo{
      .func_name = std::move(func_name),
      .pkg_path = "dds_rpc_test",
      .module_name = std::move(module_name),
      .index = index,
      .custom_type_support_ptr = nullptr,
      .req_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetDdsMessageTypeSupport<example::AddRequest>()),
      .rsp_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetDdsMessageTypeSupport<example::AddResponse>())};
}

bool WaitFor(std::function<bool()> predicate,
             std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

#if defined(AIMRT_BUILD_WITH_ROS2)
class ScopedRos2Runtime {
 public:
  ScopedRos2Runtime() {
    if (!rclcpp::ok()) {
      int argc = 0;
      rclcpp::init(argc, nullptr);
      initialized_here_ = true;
    }
  }

  ~ScopedRos2Runtime() {
    if (initialized_here_ && rclcpp::ok()) rclcpp::shutdown();
  }

 private:
  bool initialized_here_ = false;
};
#endif

struct ServiceControl {
  std::atomic_uint64_t calls = 0;
  std::atomic_bool hold = false;
  std::mutex mutex;
  std::vector<std::shared_ptr<runtime::core::rpc::InvokeWrapper>> held;
};

class RpcHarness {
 public:
  explicit RpcHarness(
      uint32_t domain_id = 228,
      std::string participant_name = "aimrt_dds_rpc_behavior") {
    context_.Initialize(DdsPluginOptions{
        .domain_id = domain_id,
        .participant_name = std::move(participant_name),
        .executor = {.type = "asio_thread", .thread_num = 4}});
    runtime_.Initialize(context_);
    state_ = std::make_shared<DdsBackendState>(context_, runtime_);
    backend_ = std::make_unique<DdsRpcBackend>(state_);
    backend_->Initialize(YAML::Load("{}"));
  }

  ~RpcHarness() {
    if (backend_) backend_->Shutdown();
    runtime_.Shutdown();
    context_.Shutdown();
  }

  bool RegisterServer(const runtime::core::rpc::FuncInfo& info,
                      const std::shared_ptr<ServiceControl>& control) {
    runtime::core::rpc::ServiceFuncWrapper wrapper{
        .info = info,
        .service_func = [control](
                            const std::shared_ptr<runtime::core::rpc::InvokeWrapper>& invoke) {
          control->calls.fetch_add(1);
          if (control->hold.load()) {
            std::lock_guard lock(control->mutex);
            control->held.emplace_back(invoke);
            return;
          }
          const auto& request = *static_cast<const example::AddRequest*>(invoke->req_ptr);
          auto& response = *static_cast<example::AddResponse*>(invoke->rsp_ptr);
          response.sum(request.lhs() + request.rhs());
          invoke->callback(aimrt::rpc::Status());
        }};
    return backend_->RegisterServiceFunc(wrapper);
  }

  bool RegisterClient(const runtime::core::rpc::FuncInfo& info) {
    return backend_->RegisterClientFunc(
        runtime::core::rpc::ClientFuncWrapper{.info = info});
  }

  void Start() {
    context_.Start();
    runtime_.Start();
    backend_->Start();
  }

  bool WaitReady(const runtime::core::rpc::FuncInfo& info,
                 std::chrono::milliseconds timeout = 5s) {
    return WaitFor([this, &info] { return backend_->ReadinessForTesting(info) == "MATCHED"; },
                   timeout);
  }

  aimrt::rpc::Status Invoke(const runtime::core::rpc::FuncInfo& info,
                            const example::AddRequest& request,
                            example::AddResponse& response,
                            std::chrono::milliseconds timeout = 2s,
                            std::atomic_uint32_t* callback_count = nullptr) {
    aimrt::rpc::Context context;
    context.SetTimeout(timeout);
    std::promise<aimrt::rpc::Status> promise;
    auto future = promise.get_future();
    auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
        runtime::core::rpc::InvokeWrapper{.info = info,
                                          .req_ptr = &request,
                                          .rsp_ptr = &response,
                                          .ctx_ref = context});
    invoke->callback = [&promise, callback_count](aimrt::rpc::Status status) {
      if (callback_count != nullptr && callback_count->fetch_add(1) != 0) return;
      promise.set_value(std::move(status));
    };
    backend_->Invoke(invoke);
    if (future.wait_for(timeout + 2s) != std::future_status::ready) {
      return aimrt::rpc::Status(AIMRT_RPC_STATUS_UNKNOWN);
    }
    return future.get();
  }

  DdsRpcBackend& Backend() { return *backend_; }
  DdsRuntime& Runtime() { return runtime_; }
  DdsParticipantContext& Context() { return context_; }
  void DestroyBackendBeforeRuntimeForTesting() {
    backend_->Shutdown();
    backend_.reset();
  }

 private:
  DdsParticipantContext context_;
  DdsRuntime runtime_;
  std::shared_ptr<DdsBackendState> state_;
  std::unique_ptr<DdsRpcBackend> backend_;
};

struct AsyncRpcCall {
  example::AddRequest request;
  example::AddResponse response;
  aimrt::rpc::Context context;
  std::shared_ptr<std::promise<aimrt::rpc::Status>> promise =
      std::make_shared<std::promise<aimrt::rpc::Status>>();
  std::future<aimrt::rpc::Status> future = promise->get_future();
  std::shared_ptr<std::atomic_uint32_t> callback_count =
      std::make_shared<std::atomic_uint32_t>(0);
  std::shared_ptr<runtime::core::rpc::InvokeWrapper> invoke;
};

std::unique_ptr<AsyncRpcCall> PrepareAsyncCall(
    const runtime::core::rpc::FuncInfo& info, int32_t lhs, int32_t rhs,
    std::chrono::milliseconds timeout = 5s) {
  auto call = std::make_unique<AsyncRpcCall>();
  call->request.lhs(lhs);
  call->request.rhs(rhs);
  call->context.SetTimeout(timeout);
  call->invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = info,
                                        .req_ptr = &call->request,
                                        .rsp_ptr = &call->response,
                                        .ctx_ref = call->context});
  call->invoke->callback = [promise = call->promise,
                            callbacks = call->callback_count](
                               aimrt::rpc::Status status) {
    if (callbacks->fetch_add(1) == 0) promise->set_value(std::move(status));
  };
  return call;
}

std::unique_ptr<AsyncRpcCall> StartAsyncCall(
    RpcHarness& harness, const runtime::core::rpc::FuncInfo& info, int32_t lhs,
    int32_t rhs, std::chrono::milliseconds timeout = 5s) {
  auto call = PrepareAsyncCall(info, lhs, rhs, timeout);
  harness.Backend().Invoke(call->invoke);
  return call;
}

std::shared_ptr<runtime::core::rpc::InvokeWrapper> TakeHeldInvoke(
    const std::shared_ptr<ServiceControl>& control) {
  std::lock_guard lock(control->mutex);
  if (control->held.empty()) return {};
  auto invoke = std::move(control->held.front());
  control->held.erase(control->held.begin());
  return invoke;
}

bool WriterMatched(eprosima::fastdds::dds::DataWriter* writer) {
  eprosima::fastdds::dds::PublicationMatchedStatus status;
  return writer != nullptr &&
         writer->get_publication_matched_status(status) ==
             eprosima::fastdds::dds::RETCODE_OK &&
         status.current_count >= 1;
}

bool WriteProcessEvent(int fd, char event) {
  return write(fd, &event, 1) == 1;
}

bool ReadProcessControl(int fd, char expected) {
  char control = 0;
  return read(fd, &control, 1) == 1 && control == expected;
}

int RunIndependentClientProcess(int event_fd, int control_fd,
                                const std::string& function_name,
                                const std::vector<int>& expected_closed_fds) {
  alarm(20);
  for (const int fd : expected_closed_fds) {
    errno = 0;
    if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF) return 9;
  }
  RpcHarness harness(228, "aimrt_dds_rpc_independent_client");
  const auto client =
      MakeRpcInfo("independent_process_client", 1, function_name);
  if (!harness.RegisterClient(client)) return 10;
  harness.Start();
  if (!harness.WaitReady(client, 8s)) return 11;
  if (!WriteProcessEvent(event_fd, 'R')) return 12;

  example::AddRequest request;
  request.lhs(19);
  request.rhs(23);
  example::AddResponse response;
  std::atomic_uint32_t callbacks = 0;
  const auto status = harness.Invoke(client, request, response, 2s, &callbacks);
  if (!status.OK() || response.sum() != 42 || callbacks.load() != 1 ||
      harness.Backend().PendingCountForTesting(client) != 0) {
    return 13;
  }
  if (!WriteProcessEvent(event_fd, 'P')) return 14;
  return ReadProcessControl(control_fd, 'Q') ? 0 : 15;
}

int RunServerProcess(int event_fd, int control_fd,
                     const std::string& function_name, bool hold_response) {
  alarm(20);
  RpcHarness harness(228, hold_response ? "aimrt_dds_rpc_exiting_server"
                                        : "aimrt_dds_rpc_restarted_server");
  const auto server = MakeRpcInfo(hold_response ? "exiting_server"
                                                : "restarted_server",
                                  1, function_name);
  auto control = std::make_shared<ServiceControl>();
  control->hold = hold_response;
  if (!harness.RegisterServer(server, control)) return 20;
  harness.Start();
  if (!WriteProcessEvent(event_fd, 'S')) return 21;
  if (!WaitFor([&] { return control->calls.load() != 0; }, 8s)) return 22;
  if (!WriteProcessEvent(event_fd, 'H')) return 23;
  if (!hold_response) {
    std::this_thread::sleep_for(300ms);
    if (control->calls.load() != 1 || !WriteProcessEvent(event_fd, '1')) {
      return 24;
    }
  }
  return ReadProcessControl(control_fd, 'Q') ? 0 : 25;
}

int RunProtocolProbeProcess(int event_fd, int control_fd,
                            bool close_control) {
  alarm(20);
  if (close_control) close(control_fd);
  if (!WriteProcessEvent(event_fd, close_control ? 'C' : 'W')) return 30;
  if (close_control) {
    while (true) pause();
  }
  return ReadProcessControl(control_fd, 'Q') ? 0 : 31;
}

TEST(DdsRpcReadiness, ClassifiesZeroOneSidedUnequalAndEqualCounts) {
  EXPECT_EQ(DdsRpcBackend::ClassifyReadinessForTesting(0, 0), "UNMATCHED");
  EXPECT_EQ(DdsRpcBackend::ClassifyReadinessForTesting(0, 1), "UNMATCHED");
  EXPECT_EQ(DdsRpcBackend::ClassifyReadinessForTesting(1, 0), "PARTIALLY_MATCHED");
  EXPECT_EQ(DdsRpcBackend::ClassifyReadinessForTesting(2, 1), "PARTIALLY_MATCHED");
  EXPECT_EQ(DdsRpcBackend::ClassifyReadinessForTesting(2, 2), "MATCHED");
}

TEST(DdsRpcIdentity, UnknownSequenceIsFilledEqualIsPreservedAndMismatchIsRejected) {
  RpcHarness harness;
  const auto client = MakeRpcInfo("identity_client", 1);
  ASSERT_TRUE(harness.RegisterClient(client));

  eprosima::fastdds::rtps::SampleIdentity request;
  request.writer_guid(harness.Backend().RequestWriterGuidForTesting(client));
  request.sequence_number(eprosima::fastdds::rtps::SequenceNumber_t(0, 42));
  eprosima::fastdds::rtps::SampleIdentity related;
  eprosima::fastdds::rtps::SampleIdentity token;
  EXPECT_FALSE(DdsRpcBackend::FillCorrelationTokenForTesting(request, related, token));
  related.writer_guid(harness.Backend().ResponseReaderGuidForTesting(client));
  related.sequence_number(eprosima::fastdds::rtps::c_SequenceNumber_Unknown);

  ASSERT_TRUE(DdsRpcBackend::FillCorrelationTokenForTesting(request, related, token));
  EXPECT_EQ(token.writer_guid(), related.writer_guid());
  EXPECT_EQ(token.sequence_number(), request.sequence_number());

  related.sequence_number(request.sequence_number());
  ASSERT_TRUE(DdsRpcBackend::FillCorrelationTokenForTesting(request, related, token));
  EXPECT_EQ(token, related);

  related.sequence_number(eprosima::fastdds::rtps::SequenceNumber_t(0, 43));
  EXPECT_FALSE(DdsRpcBackend::FillCorrelationTokenForTesting(request, related, token));
  related.writer_guid(request.writer_guid());
  related.sequence_number(eprosima::fastdds::rtps::c_SequenceNumber_Unknown);
  EXPECT_FALSE(DdsRpcBackend::FillCorrelationTokenForTesting(request, related, token));
}

TEST(DdsRpcIdentity, FullEqualReachesHandlerButMismatchIsDroppedBeforeHandler) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("identity_server", 1);
  const auto client = MakeRpcInfo("identity_client_runtime", 2);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  harness.Backend().SetRequestIdentityOverrideForTesting(
      [](eprosima::fastdds::rtps::SampleIdentity& related,
         const eprosima::fastdds::rtps::SampleIdentity& request) {
        related.sequence_number(request.sequence_number());
      });
  example::AddRequest request;
  request.lhs(1);
  request.rhs(41);
  example::AddResponse response;
  EXPECT_TRUE(harness.Invoke(client, request, response).OK());
  EXPECT_EQ(response.sum(), 42);
  EXPECT_EQ(control->calls.load(), 1U);
  EXPECT_EQ(harness.Backend().InvalidRequestCountForTesting(), 0U);

  harness.Backend().SetRequestIdentityOverrideForTesting(
      [](eprosima::fastdds::rtps::SampleIdentity& related,
         const eprosima::fastdds::rtps::SampleIdentity& request) {
        auto mismatch = request.sequence_number();
        ++mismatch;
        related.sequence_number(mismatch);
      });
  EXPECT_EQ(harness.Invoke(client, request, response, 80ms).Code(), AIMRT_RPC_STATUS_TIMEOUT);
  EXPECT_EQ(control->calls.load(), 1U);
  EXPECT_EQ(harness.Backend().InvalidRequestCountForTesting(), 1U);
}

TEST(DdsRpcRoundtrip, NativeRequestResponseUsesExplicitReaderGuidToken) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("native_server", 1);
  const auto client = MakeRpcInfo("native_client", 2);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  example::AddRequest request;
  request.lhs(19);
  request.rhs(23);
  example::AddResponse response;
  const auto status = harness.Invoke(client, request, response);
  ASSERT_TRUE(status.OK()) << status.ToString();
  EXPECT_EQ(response.sum(), 42);
  EXPECT_EQ(control->calls.load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
  EXPECT_TRUE(harness.Backend().ResponseReaderGuidForTesting(client).entityId.is_reader());
  EXPECT_EQ(harness.Backend().RequestWriterMaxBlockingTimeUsForTesting(client), 0);

  const auto report = harness.Backend().GenInitializationReport();
  const std::map<std::string, std::string> report_map(report.begin(), report.end());
  EXPECT_EQ(report_map.at("dds.rpc.reply_target_mode"),
            "explicit_response_reader_guid_in_related_sample_identity");
  EXPECT_EQ(report_map.at("dds.rpc.related_entity_setters"), "unsupported_not_called");
  EXPECT_EQ(report_map.at("dds.rpc.fastdds_compatibility"),
            "fastdds_3.6.2_dd66ef2a_explicit_identity");
  EXPECT_FALSE(report_map.contains("dds.rpc.compatibility"));
  EXPECT_EQ(std::ranges::count_if(report, [](const auto& entry) {
              return entry.first.ends_with(".request_writer_max_blocking_time_us") &&
                     entry.second == "0";
            }),
            1);
  EXPECT_EQ(std::ranges::count_if(report, [](const auto& entry) {
              return entry.first.ends_with(".request_writer_max_blocking_time_source") &&
                     entry.second == "fixed_rpc_zero";
            }),
            1);
}

TEST(DdsRpcMultipleServers, BroadcastsAndFirstValidResponseWins) {
  RpcHarness harness;
  const auto slow_info = MakeRpcInfo("multi_server_slow", 1);
  const auto fast_info = MakeRpcInfo("multi_server_fast", 2);
  const auto removed_info = MakeRpcInfo("multi_server_removed", 3);
  const auto client = MakeRpcInfo("multi_server_client", 4);
  auto slow_control = std::make_shared<ServiceControl>();
  auto fast_control = std::make_shared<ServiceControl>();
  auto removed_control = std::make_shared<ServiceControl>();
  slow_control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(slow_info, slow_control));
  ASSERT_TRUE(harness.RegisterServer(fast_info, fast_control));
  ASSERT_TRUE(harness.RegisterServer(removed_info, removed_control));
  ASSERT_TRUE(harness.RegisterClient(client));
  EXPECT_EQ(harness.Backend().ReadinessForTesting(client), "MATCHED");
  harness.Start();
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend().ReadinessForTesting(client) == "MATCHED" &&
           harness.Backend()
                   .DiagnosticsForTesting()
                   .rpc_matched_servers == 3;
  }));
  EXPECT_EQ(harness.Backend().DiagnosticsForTesting().rpc_multiple_servers_total,
            1U);
  harness.Backend().DisconnectServerForTesting(removed_info);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend().DiagnosticsForTesting().rpc_matched_servers == 2;
  }));
  EXPECT_EQ(harness.Backend().DiagnosticsForTesting().rpc_multiple_servers_total,
            1U);

  auto call = StartAsyncCall(harness, client, 20, 22, 2s);
  ASSERT_EQ(call->future.wait_for(2s), std::future_status::ready);
  ASSERT_TRUE(call->future.get().OK());
  EXPECT_EQ(call->response.sum(), 42);
  ASSERT_TRUE(WaitFor([&] {
    return slow_control->calls.load() == 1 && fast_control->calls.load() == 1;
  }));

  auto held = TakeHeldInvoke(slow_control);
  ASSERT_TRUE(held);
  auto& response = *static_cast<example::AddResponse*>(held->rsp_ptr);
  response.sum(42);
  held->callback(aimrt::rpc::Status());
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend()
               .DiagnosticsForTesting()
               .rpc_duplicate_response_total == 1;
  }));
  EXPECT_EQ(call->callback_count->load(), 1U);
}

TEST(DdsRpcCorrelation, ConcurrentCallsCompleteWithTheirOwnResponses) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("concurrent_server", 1);
  const auto client = MakeRpcInfo("concurrent_client", 2);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  std::vector<std::future<bool>> calls;
  for (int value = 0; value < 24; ++value) {
    calls.emplace_back(std::async(std::launch::async, [&harness, client, value] {
      example::AddRequest request;
      request.lhs(value);
      request.rhs(1000 - value);
      example::AddResponse response;
      return harness.Invoke(client, request, response).OK() && response.sum() == 1000;
    }));
  }
  for (auto& call : calls) EXPECT_TRUE(call.get());
  EXPECT_EQ(control->calls.load(), calls.size());
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
}

TEST(DdsRpcCorrelation, ReplyWaitsForPublicationGateWithoutTaking) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("publication_gate_server", 1);
  const auto client = MakeRpcInfo("publication_gate_client", 2);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  bool observed_gate_retry = false;
  harness.Backend().SetAfterRequestWriteHookForTesting([&] {
    observed_gate_retry = WaitFor(
        [&] {
          return harness.Backend().ResponseGateBusyRetryCountForTesting(client) != 0 &&
                 harness.Backend().ResponseUnreadCountForTesting(client) != 0;
        },
        2s);
  });

  example::AddRequest request;
  request.lhs(17);
  request.rhs(25);
  example::AddResponse response;
  const auto status = harness.Invoke(client, request, response);
  harness.Backend().SetAfterRequestWriteHookForTesting({});

  ASSERT_TRUE(status.OK()) << status.ToString();
  EXPECT_TRUE(observed_gate_retry);
  EXPECT_EQ(response.sum(), 42);
  EXPECT_EQ(control->calls.load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
  EXPECT_EQ(harness.Backend().ResponseUnreadCountForTesting(client), 0);
  EXPECT_GE(harness.Backend().ResponseRetryScheduleCountForTesting(client), 1U);
  EXPECT_GE(harness.Backend().ResponseGateBusyRetryCountForTesting(client), 1U);
  EXPECT_GE(harness.Backend().ResponseLastRetryDelayUsForTesting(client), 1000U);
}

TEST(DdsRpcCorrelation, ForeignClientReplyTokenCannotCompleteAnotherClient) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("foreign_server", 1);
  const auto client_one = MakeRpcInfo("foreign_client_one", 2);
  const auto client_two = MakeRpcInfo("foreign_client_two", 3);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client_one));
  ASSERT_TRUE(harness.RegisterClient(client_two));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client_one));
  ASSERT_TRUE(harness.WaitReady(client_two));

  const auto foreign_reader =
      harness.Backend().ResponseReaderGuidForTesting(client_two);
  harness.Backend().SetRequestIdentityOverrideForTesting(
      [foreign_reader](eprosima::fastdds::rtps::SampleIdentity& related,
                       const eprosima::fastdds::rtps::SampleIdentity&) {
        related.writer_guid(foreign_reader);
      });
  example::AddRequest request;
  request.lhs(20);
  request.rhs(22);
  example::AddResponse response;
  EXPECT_EQ(harness.Invoke(client_one, request, response, 100ms).Code(),
            AIMRT_RPC_STATUS_TIMEOUT);
  EXPECT_EQ(control->calls.load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client_one), 0U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client_two), 0U);

  harness.Backend().SetRequestIdentityOverrideForTesting({});
  example::AddResponse response_one;
  example::AddResponse response_two;
  auto call_one = std::async(std::launch::async, [&] {
    return harness.Invoke(client_one, request, response_one);
  });
  auto call_two = std::async(std::launch::async, [&] {
    return harness.Invoke(client_two, request, response_two);
  });
  EXPECT_TRUE(call_one.get().OK());
  EXPECT_TRUE(call_two.get().OK());
  EXPECT_EQ(response_one.sum(), 42);
  EXPECT_EQ(response_two.sum(), 42);
  EXPECT_EQ(control->calls.load(), 3U);
}

TEST(DdsRpcMultiProcess,
     IndependentClientsUseOwnCorrelationAcrossParticipants) {
  test::ProcessTestScope processes(20s);
  ASSERT_TRUE(processes.Ready());
  RpcHarness harness;
  const std::string function_name =
      "dds:/example::Calculator/AddR04_" + std::to_string(getpid());
  const auto server = MakeRpcInfo("multi_process_server", 1, function_name);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  harness.Start();

  auto* first =
      processes.Launch("--dds-independent-client", {function_name}, true);
  ASSERT_NE(first, nullptr);
  auto* second = processes.Launch(
      "--dds-independent-client",
      {function_name, std::to_string(first->EventFdForTesting()),
       std::to_string(first->ControlFdForTesting())},
      true);
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(processes.ReadEvent(first, 'R', 8s));
  ASSERT_TRUE(processes.ReadEvent(second, 'R', 8s));
  ASSERT_TRUE(processes.ReadEvent(first, 'P', 5s));
  ASSERT_TRUE(processes.ReadEvent(second, 'P', 5s));
  EXPECT_EQ(control->calls.load(), 2U);

  const auto first_result = processes.Finish(first, 0ms);
  const auto second_result = processes.Finish(second, 0ms);
  for (const auto& result : {first_result, second_result}) {
    EXPECT_TRUE(result.process_group_established);
    ASSERT_TRUE(result.child_reaped);
    ASSERT_TRUE(WIFSIGNALED(result.status));
    EXPECT_EQ(WTERMSIG(result.status), SIGTERM);
    EXPECT_TRUE(result.term_sent);
    EXPECT_FALSE(result.kill_fallback_sent);
    EXPECT_TRUE(result.no_residual_process_group);
  }
  EXPECT_EQ(processes.ReceivedSignal(), 0);
  EXPECT_FALSE(processes.Expired());
}

TEST(DdsProcessGroupHelper, ClosedControlPipeDoesNotRaiseSigpipe) {
  test::ProcessTestScope processes(5s);
  ASSERT_TRUE(processes.Ready());
  auto* child = processes.Launch("--dds-close-control", {}, true);
  ASSERT_NE(child, nullptr);
  ASSERT_TRUE(processes.ReadEvent(child, 'C', 2s));
  EXPECT_FALSE(processes.SendControl(child, 'Q'));
  const auto result = processes.Finish(child, 0ms);
  EXPECT_TRUE(result.term_sent);
  EXPECT_FALSE(result.kill_fallback_sent);
  EXPECT_TRUE(result.child_reaped);
  EXPECT_TRUE(result.no_residual_process_group);
  EXPECT_EQ(processes.ReceivedSignal(), 0);
}

TEST(DdsProcessGroupHelper, ReceivedTerminationSignalUsesOwnedCleanup) {
  test::ProcessTestScope processes(5s);
  ASSERT_TRUE(processes.Ready());
  auto* child = processes.Launch("--dds-waiting-child", {}, true);
  ASSERT_NE(child, nullptr);
  ASSERT_TRUE(processes.ReadEvent(child, 'W', 2s));
  ASSERT_EQ(kill(getpid(), SIGTERM), 0);
  ASSERT_TRUE(WaitFor([&] { return processes.ReceivedSignal() == SIGTERM; }, 1s));
  const auto result = processes.Finish(child, 0ms);
  EXPECT_TRUE(result.term_sent);
  EXPECT_FALSE(result.kill_fallback_sent);
  EXPECT_TRUE(result.child_reaped);
  EXPECT_TRUE(result.no_residual_process_group);
}

TEST(DdsProcessGroupHelper, CaseDeadlineExpiresAndCleansOwnedGroup) {
  test::ProcessTestScope processes(150ms);
  ASSERT_TRUE(processes.Ready());
  auto* child = processes.Launch("--dds-waiting-child", {}, true);
  ASSERT_NE(child, nullptr);
  ASSERT_TRUE(processes.ReadEvent(child, 'W', 100ms));
  EXPECT_FALSE(processes.ReadEvent(child, 'X', 1s));
  std::this_thread::sleep_for(2ms);
  EXPECT_TRUE(processes.Expired());
  const auto result = processes.Finish(child, 0ms);
  EXPECT_TRUE(result.term_sent);
  EXPECT_FALSE(result.kill_fallback_sent);
  EXPECT_TRUE(result.child_reaped);
  EXPECT_TRUE(result.no_residual_process_group);
}

TEST(DdsRpcHandlerConcurrency, MultipleRequestsCanReachHandlersBeforeEarlierCompletion) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("handler_concurrency_server", 1);
  const auto client = MakeRpcInfo("handler_concurrency_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  std::vector<example::AddRequest> requests(4);
  std::vector<example::AddResponse> responses(4);
  std::vector<std::future<aimrt::rpc::Status>> calls;
  for (size_t index = 0; index < requests.size(); ++index) {
    requests[index].lhs(static_cast<int32_t>(index));
    requests[index].rhs(42 - static_cast<int32_t>(index));
    calls.emplace_back(std::async(std::launch::async, [&, index] {
      return harness.Invoke(client, requests[index], responses[index]);
    }));
  }
  ASSERT_TRUE(WaitFor([&] { return control->calls.load() == requests.size(); }));

  std::vector<std::shared_ptr<runtime::core::rpc::InvokeWrapper>> held;
  {
    std::lock_guard lock(control->mutex);
    held.swap(control->held);
  }
  ASSERT_EQ(held.size(), requests.size());
  for (auto iterator = held.rbegin(); iterator != held.rend(); ++iterator) {
    const auto& request =
        *static_cast<const example::AddRequest*>((*iterator)->req_ptr);
    auto& response = *static_cast<example::AddResponse*>((*iterator)->rsp_ptr);
    response.sum(request.lhs() + request.rhs());
    (*iterator)->callback(aimrt::rpc::Status());
  }
  for (size_t index = 0; index < calls.size(); ++index) {
    ASSERT_TRUE(calls[index].get().OK());
    EXPECT_EQ(responses[index].sum(), 42);
  }
}

#if defined(AIMRT_BUILD_WITH_PROTOBUF)
struct AsyncProtobufServiceControl {
  std::atomic_uint64_t calls = 0;
  std::atomic_bool throw_after_hold = false;
  std::mutex mutex;
  std::vector<std::shared_ptr<runtime::core::rpc::InvokeWrapper>> held;
};

std::shared_ptr<runtime::core::rpc::InvokeWrapper> TakeHeldProtobufInvoke(
    const std::shared_ptr<AsyncProtobufServiceControl>& control);

runtime::core::rpc::FuncInfo MakeProtobufRpcInfo(std::string module_name,
                                                 uint64_t index) {
  return runtime::core::rpc::FuncInfo{
      .func_name = "pb:/aimrt.dds.rpc.test.Calculator/Add",
      .pkg_path = "dds_rpc_async_request_lifetime",
      .module_name = std::move(module_name),
      .index = index,
      .custom_type_support_ptr = nullptr,
      .req_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetProtobufMessageTypeSupport<aimrt::dds::rpc::test::AddRequest>()),
      .rsp_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetProtobufMessageTypeSupport<aimrt::dds::rpc::test::AddResponse>())};
}

TEST(DdsRpcBadRequest, MalformedWrapperIsDroppedBeforeHandler) {
  RpcHarness harness;
  const auto server = MakeProtobufRpcInfo("bad_request_server", 1);
  auto control = std::make_shared<AsyncProtobufServiceControl>();
  runtime::core::rpc::ServiceFuncWrapper service{
      .info = server,
      .service_func = [control](
                          const std::shared_ptr<runtime::core::rpc::InvokeWrapper>&) {
        control->calls.fetch_add(1);
      }};
  ASSERT_TRUE(harness.Backend().RegisterServiceFunc(service));

  const auto names = DeriveDdsRpcTopicNames(server.func_name);
  auto& endpoints = harness.Runtime().Endpoints();
  auto* request_writer = endpoints.GetOrCreateWriter(
      names.request, GetSerializedMessageTypeSupport(),
      harness.Context().Qos().rpc_writer, "dds09 malformed request writer");
  auto reply_reader = endpoints.GetOrCreateReader(
      names.response, GetSerializedMessageTypeSupport(),
      harness.Context().Qos().rpc_reader, "dds09 malformed request reply reader");
  ASSERT_NE(request_writer, nullptr);
  ASSERT_NE(reply_reader.reader, nullptr);
  harness.Start();
  ASSERT_TRUE(WaitFor([&] { return WriterMatched(request_writer); }));

  eprosima::fastdds::rtps::SampleIdentity related;
  related.writer_guid(reply_reader.reader->guid());
  related.sequence_number(eprosima::fastdds::rtps::c_SequenceNumber_Unknown);
  const auto write_malformed = [&](aimrt::dds::SerializedMessage& malformed) {
    eprosima::fastdds::rtps::WriteParams params;
    params.related_sample_identity(related);
    return request_writer->write(&malformed, params);
  };

  aimrt::dds::SerializedMessage wrong_type;
  wrong_type.type_name("pb:/wrong.Request");
  wrong_type.serialization_type("pb");
  ASSERT_EQ(write_malformed(wrong_type), eprosima::fastdds::dds::RETCODE_OK);

  aimrt::dds::SerializedMessage wrong_serialization;
  wrong_serialization.type_name(
      std::string(server.req_type_support_ref.TypeName()));
  wrong_serialization.serialization_type("ros2");
  ASSERT_EQ(write_malformed(wrong_serialization),
            eprosima::fastdds::dds::RETCODE_OK);

  aimrt::dds::SerializedMessage unexpected_metadata;
  unexpected_metadata.type_name(
      std::string(server.req_type_support_ref.TypeName()));
  unexpected_metadata.serialization_type(
      std::string(server.req_type_support_ref.DefaultSerializationType()));
  aimrt::dds::MetadataEntry metadata;
  metadata.key("unexpected");
  metadata.value("value");
  unexpected_metadata.metadata().emplace_back(std::move(metadata));
  ASSERT_EQ(write_malformed(unexpected_metadata),
            eprosima::fastdds::dds::RETCODE_OK);

  aimrt::dds::SerializedMessage invalid_payload;
  invalid_payload.type_name(
      std::string(server.req_type_support_ref.TypeName()));
  invalid_payload.serialization_type(
      std::string(server.req_type_support_ref.DefaultSerializationType()));
  invalid_payload.data().push_back(0xff);
  ASSERT_EQ(write_malformed(invalid_payload),
            eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend()
               .DiagnosticsForTesting()
               .rpc_invalid_request_total == 4;
  }));
  EXPECT_EQ(control->calls.load(), 0U);
  EXPECT_EQ(reply_reader.reader->get_unread_count(), 0);
}

TEST(DdsRpcBadResponse, MalformedWrapperKeepsPendingForLaterValidReply) {
  RpcHarness harness;
  const auto server = MakeProtobufRpcInfo("bad_response_server", 1);
  const auto client = MakeProtobufRpcInfo("bad_response_client", 2);
  auto control = std::make_shared<AsyncProtobufServiceControl>();
  runtime::core::rpc::ServiceFuncWrapper service{
      .info = server,
      .service_func = [control](
                          const std::shared_ptr<runtime::core::rpc::InvokeWrapper>& invoke) {
        control->calls.fetch_add(1);
        std::lock_guard lock(control->mutex);
        control->held.emplace_back(invoke);
      }};
  ASSERT_TRUE(harness.Backend().RegisterServiceFunc(service));
  ASSERT_TRUE(harness.Backend().RegisterClientFunc(
      runtime::core::rpc::ClientFuncWrapper{.info = client}));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  aimrt::dds::rpc::test::AddRequest request;
  request.set_lhs(17);
  request.set_rhs(25);
  aimrt::dds::rpc::test::AddResponse response;
  aimrt::rpc::Context context;
  context.SetTimeout(2s);
  std::promise<aimrt::rpc::Status> promise;
  auto future = promise.get_future();
  auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &request,
                                        .rsp_ptr = &response,
                                        .ctx_ref = context});
  invoke->callback = [&promise](aimrt::rpc::Status status) {
    promise.set_value(std::move(status));
  };
  harness.Backend().Invoke(invoke);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend().PendingCountForTesting(client) == 1 &&
           control->calls.load() == 1;
  }));
  const auto tokens = harness.Backend().PendingTokensForTesting(client);
  ASSERT_EQ(tokens.size(), 1U);

  const auto names = DeriveDdsRpcTopicNames(client.func_name);
  auto* response_writer = harness.Runtime().Endpoints().GetOrCreateWriter(
      names.response, GetSerializedMessageTypeSupport(),
      harness.Context().Qos().rpc_writer, "dds09 malformed response writer");
  ASSERT_TRUE(WaitFor([&] { return WriterMatched(response_writer); }));
  aimrt::dds::SerializedMessage malformed;
  malformed.type_name(
      std::string(client.rsp_type_support_ref.TypeName()));
  malformed.serialization_type(
      std::string(client.rsp_type_support_ref.DefaultSerializationType()));
  malformed.data().push_back(0xff);
  eprosima::fastdds::rtps::WriteParams params;
  params.related_sample_identity(tokens.front());
  ASSERT_EQ(response_writer->write(&malformed, params),
            eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend()
               .DiagnosticsForTesting()
               .rpc_invalid_response_total == 1;
  }));
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 1U);
  EXPECT_EQ(future.wait_for(50ms), std::future_status::timeout);

  auto held = TakeHeldProtobufInvoke(control);
  ASSERT_TRUE(held);
  auto& valid_response =
      *static_cast<aimrt::dds::rpc::test::AddResponse*>(held->rsp_ptr);
  valid_response.set_sum(42);
  held->callback(aimrt::rpc::Status());
  ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(future.get().OK());
  EXPECT_EQ(response.sum(), 42);
}

std::shared_ptr<runtime::core::rpc::InvokeWrapper> TakeHeldProtobufInvoke(
    const std::shared_ptr<AsyncProtobufServiceControl>& control) {
  std::lock_guard lock(control->mutex);
  if (control->held.empty()) return {};
  auto invoke = std::move(control->held.front());
  control->held.erase(control->held.begin());
  return invoke;
}

TEST(DdsRpcWrapperRoundtrip, ProtobufUsesBoundedSerializedMessageWithEmptyMetadata) {
  RpcHarness harness;
  const runtime::core::rpc::FuncInfo server{
      .func_name = "pb:/aimrt.dds.rpc.test.Calculator/Add",
      .pkg_path = "dds_rpc_test",
      .module_name = "protobuf_server",
      .index = 1,
      .custom_type_support_ptr = nullptr,
      .req_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetProtobufMessageTypeSupport<aimrt::dds::rpc::test::AddRequest>()),
      .rsp_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetProtobufMessageTypeSupport<aimrt::dds::rpc::test::AddResponse>())};
  auto client = server;
  client.module_name = "protobuf_client";
  client.index = 2;
  runtime::core::rpc::ServiceFuncWrapper service{
      .info = server,
      .service_func = [](const std::shared_ptr<runtime::core::rpc::InvokeWrapper>& invoke) {
        const auto& request =
            *static_cast<const aimrt::dds::rpc::test::AddRequest*>(invoke->req_ptr);
        auto& response =
            *static_cast<aimrt::dds::rpc::test::AddResponse*>(invoke->rsp_ptr);
        response.set_sum(request.lhs() + request.rhs());
        invoke->callback(aimrt::rpc::Status());
      }};
  ASSERT_TRUE(harness.Backend().RegisterServiceFunc(service));
  ASSERT_TRUE(harness.Backend().RegisterClientFunc(
      runtime::core::rpc::ClientFuncWrapper{.info = client}));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  aimrt::dds::rpc::test::AddRequest request;
  request.set_lhs(11);
  request.set_rhs(31);
  aimrt::dds::rpc::test::AddResponse response;
  aimrt::rpc::Context context;
  context.SetTimeout(2s);
  std::promise<aimrt::rpc::Status> promise;
  auto future = promise.get_future();
  auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &request,
                                        .rsp_ptr = &response,
                                        .ctx_ref = context});
  invoke->callback = [&promise](aimrt::rpc::Status status) {
    promise.set_value(std::move(status));
  };
  harness.Backend().Invoke(invoke);
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(future.get().OK());
  EXPECT_EQ(response.sum(), 42);
}

TEST(DdsRpcWrapperRoundtrip,
     AsyncProtobufRequestSurvivesDrainAndDelayedCompletion) {
  RpcHarness harness;
  const auto server = MakeProtobufRpcInfo("protobuf_async_server", 1);
  auto client = MakeProtobufRpcInfo("protobuf_async_client", 2);
  auto control = std::make_shared<AsyncProtobufServiceControl>();
  runtime::core::rpc::ServiceFuncWrapper service{
      .info = server,
      .service_func = [control](
                          const std::shared_ptr<runtime::core::rpc::InvokeWrapper>& invoke) {
        control->calls.fetch_add(1);
        {
          std::lock_guard lock(control->mutex);
          control->held.emplace_back(invoke);
        }
        if (control->throw_after_hold.load()) {
          throw std::runtime_error("async protobuf handler failure");
        }
      }};
  ASSERT_TRUE(harness.Backend().RegisterServiceFunc(service));
  ASSERT_TRUE(harness.Backend().RegisterClientFunc(
      runtime::core::rpc::ClientFuncWrapper{.info = client}));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  aimrt::dds::rpc::test::AddRequest request;
  request.set_lhs(17);
  request.set_rhs(25);
  aimrt::dds::rpc::test::AddResponse response;
  aimrt::rpc::Context context;
  context.SetTimeout(2s);
  std::promise<aimrt::rpc::Status> promise;
  auto future = promise.get_future();
  std::atomic_uint32_t callbacks = 0;
  auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &request,
                                        .rsp_ptr = &response,
                                        .ctx_ref = context});
  invoke->callback = [&promise, &callbacks](aimrt::rpc::Status status) {
    if (callbacks.fetch_add(1) == 0) promise.set_value(std::move(status));
  };
  harness.Backend().Invoke(invoke);
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard lock(control->mutex);
    return control->held.size() == 1 &&
           harness.Backend().ServerDrainsInFlightForTesting() == 0;
  }));

  auto held = TakeHeldProtobufInvoke(control);
  ASSERT_TRUE(held);
  const auto& delayed_request =
      *static_cast<const aimrt::dds::rpc::test::AddRequest*>(held->req_ptr);
  auto& delayed_response =
      *static_cast<aimrt::dds::rpc::test::AddResponse*>(held->rsp_ptr);
  EXPECT_EQ(delayed_request.lhs(), 17);
  EXPECT_EQ(delayed_request.rhs(), 25);
  delayed_response.set_sum(delayed_request.lhs() + delayed_request.rhs());
  held->callback(aimrt::rpc::Status());

  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(future.get().OK());
  EXPECT_EQ(response.sum(), 42);
  EXPECT_EQ(callbacks.load(), 1U);
}

TEST(DdsRpcWrapperRoundtrip,
     AsyncProtobufRequestSurvivesExceptionTimeoutAndShutdownOverlap) {
  RpcHarness harness;
  const auto server = MakeProtobufRpcInfo("protobuf_overlap_server", 1);
  auto client = MakeProtobufRpcInfo("protobuf_overlap_client", 2);
  auto control = std::make_shared<AsyncProtobufServiceControl>();
  runtime::core::rpc::ServiceFuncWrapper service{
      .info = server,
      .service_func = [control](
                          const std::shared_ptr<runtime::core::rpc::InvokeWrapper>& invoke) {
        control->calls.fetch_add(1);
        {
          std::lock_guard lock(control->mutex);
          control->held.emplace_back(invoke);
        }
        if (control->throw_after_hold.load()) {
          throw std::runtime_error("async protobuf handler failure");
        }
      }};
  ASSERT_TRUE(harness.Backend().RegisterServiceFunc(service));
  ASSERT_TRUE(harness.Backend().RegisterClientFunc(
      runtime::core::rpc::ClientFuncWrapper{.info = client}));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  control->throw_after_hold = true;
  aimrt::dds::rpc::test::AddRequest timeout_request;
  timeout_request.set_lhs(19);
  timeout_request.set_rhs(23);
  aimrt::dds::rpc::test::AddResponse timeout_response;
  aimrt::rpc::Context timeout_context;
  timeout_context.SetTimeout(100ms);
  std::promise<aimrt::rpc::Status> timeout_promise;
  auto timeout_future = timeout_promise.get_future();
  std::atomic_uint32_t timeout_callbacks = 0;
  auto timeout_invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &timeout_request,
                                        .rsp_ptr = &timeout_response,
                                        .ctx_ref = timeout_context});
  timeout_invoke->callback =
      [&timeout_promise, &timeout_callbacks](aimrt::rpc::Status status) {
        if (timeout_callbacks.fetch_add(1) == 0) {
          timeout_promise.set_value(std::move(status));
        }
      };
  harness.Backend().Invoke(timeout_invoke);
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard lock(control->mutex);
    return control->held.size() == 1 &&
           harness.Backend().ServerDrainsInFlightForTesting() == 0;
  }));
  ASSERT_EQ(timeout_future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(timeout_future.get().Code(), AIMRT_RPC_STATUS_TIMEOUT);
  auto held_after_exception = TakeHeldProtobufInvoke(control);
  ASSERT_TRUE(held_after_exception);
  const auto& request_after_exception =
      *static_cast<const aimrt::dds::rpc::test::AddRequest*>(
          held_after_exception->req_ptr);
  EXPECT_EQ(request_after_exception.lhs(), 19);
  EXPECT_EQ(request_after_exception.rhs(), 23);
  held_after_exception->callback(aimrt::rpc::Status());
  EXPECT_EQ(timeout_callbacks.load(), 1U);

  control->throw_after_hold = false;
  aimrt::dds::rpc::test::AddRequest shutdown_request;
  shutdown_request.set_lhs(21);
  shutdown_request.set_rhs(21);
  aimrt::dds::rpc::test::AddResponse shutdown_response;
  aimrt::rpc::Context shutdown_context;
  shutdown_context.SetTimeout(2s);
  std::promise<aimrt::rpc::Status> shutdown_promise;
  auto shutdown_future = shutdown_promise.get_future();
  std::atomic_uint32_t shutdown_callbacks = 0;
  auto shutdown_invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &shutdown_request,
                                        .rsp_ptr = &shutdown_response,
                                        .ctx_ref = shutdown_context});
  shutdown_invoke->callback =
      [&shutdown_promise, &shutdown_callbacks](aimrt::rpc::Status status) {
        if (shutdown_callbacks.fetch_add(1) == 0) {
          shutdown_promise.set_value(std::move(status));
        }
      };
  harness.Backend().Invoke(shutdown_invoke);
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard lock(control->mutex);
    return control->held.size() == 1 &&
           harness.Backend().ServerDrainsInFlightForTesting() == 0;
  }));
  harness.Backend().Shutdown();
  ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(shutdown_future.get().Code(),
            AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
  auto held_during_shutdown = TakeHeldProtobufInvoke(control);
  ASSERT_TRUE(held_during_shutdown);
  const auto& request_during_shutdown =
      *static_cast<const aimrt::dds::rpc::test::AddRequest*>(
          held_during_shutdown->req_ptr);
  EXPECT_EQ(request_during_shutdown.lhs(), 21);
  EXPECT_EQ(request_during_shutdown.rhs(), 21);
  held_during_shutdown->callback(aimrt::rpc::Status());
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(shutdown_callbacks.load(), 1U);
}

TEST(DdsRpcWrapperRoundtrip, SerializationFailuresTerminateWithoutLeakingPending) {
  RpcHarness harness;
  const runtime::core::rpc::FuncInfo server{
      .func_name = "pb:/aimrt.dds.rpc.test.Calculator/Add",
      .pkg_path = "dds_rpc_test_serialization_failure",
      .module_name = "protobuf_failure_server",
      .index = 1,
      .custom_type_support_ptr = nullptr,
      .req_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetProtobufMessageTypeSupport<aimrt::dds::rpc::test::AddRequest>()),
      .rsp_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetProtobufMessageTypeSupport<aimrt::dds::rpc::test::AddResponse>())};
  auto client = server;
  client.module_name = "protobuf_failure_client";
  client.index = 2;
  runtime::core::rpc::ServiceFuncWrapper service{
      .info = server,
      .service_func = [](const std::shared_ptr<runtime::core::rpc::InvokeWrapper>& invoke) {
        const auto& request =
            *static_cast<const aimrt::dds::rpc::test::AddRequest*>(invoke->req_ptr);
        auto& response =
            *static_cast<aimrt::dds::rpc::test::AddResponse*>(invoke->rsp_ptr);
        response.set_sum(request.lhs() + request.rhs());
        invoke->callback(aimrt::rpc::Status());
      }};
  ASSERT_TRUE(harness.Backend().RegisterServiceFunc(service));
  ASSERT_TRUE(harness.Backend().RegisterClientFunc(
      runtime::core::rpc::ClientFuncWrapper{.info = client}));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  aimrt::dds::rpc::test::AddRequest request;
  request.set_lhs(20);
  request.set_rhs(22);
  aimrt::dds::rpc::test::AddResponse response;
  auto invoke_once = [&](std::chrono::milliseconds timeout) {
    aimrt::rpc::Context context;
    context.SetTimeout(timeout);
    std::promise<aimrt::rpc::Status> promise;
    auto future = promise.get_future();
    auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
        runtime::core::rpc::InvokeWrapper{.info = client,
                                          .req_ptr = &request,
                                          .rsp_ptr = &response,
                                          .ctx_ref = context});
    invoke->callback = [&promise](aimrt::rpc::Status status) {
      promise.set_value(std::move(status));
    };
    harness.Backend().Invoke(invoke);
    if (future.wait_for(timeout + 2s) != std::future_status::ready) {
      return aimrt::rpc::Status(AIMRT_RPC_STATUS_UNKNOWN);
    }
    return future.get();
  };

  harness.Backend().FailNextRequestSerializationForTesting();
  EXPECT_EQ(invoke_once(1s).Code(), AIMRT_RPC_STATUS_CLI_SEND_REQ_FAILED);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);

  harness.Backend().FailNextResponseSerializationForTesting();
  EXPECT_EQ(invoke_once(100ms).Code(), AIMRT_RPC_STATUS_TIMEOUT);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
}
#endif

#if defined(AIMRT_BUILD_WITH_ROS2)
TEST(DdsRpcWrapperRoundtrip, Ros2UsesBoundedSerializedMessageWithEmptyMetadata) {
  ScopedRos2Runtime ros2_runtime;
  RpcHarness harness;
  const runtime::core::rpc::FuncInfo server{
      .func_name = "ros2:/std_msgs::msg::Int32/AddOne",
      .pkg_path = "dds_rpc_test",
      .module_name = "ros2_server",
      .index = 1,
      .custom_type_support_ptr = nullptr,
      .req_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetRos2MessageTypeSupport<std_msgs::msg::Int32>()),
      .rsp_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetRos2MessageTypeSupport<std_msgs::msg::Int32>())};
  auto client = server;
  client.module_name = "ros2_client";
  client.index = 2;
  runtime::core::rpc::ServiceFuncWrapper service{
      .info = server,
      .service_func = [](const std::shared_ptr<runtime::core::rpc::InvokeWrapper>& invoke) {
        const auto& request =
            *static_cast<const std_msgs::msg::Int32*>(invoke->req_ptr);
        auto& response = *static_cast<std_msgs::msg::Int32*>(invoke->rsp_ptr);
        response.data = request.data + 1;
        invoke->callback(aimrt::rpc::Status());
      }};
  ASSERT_TRUE(harness.Backend().RegisterServiceFunc(service));
  ASSERT_TRUE(harness.Backend().RegisterClientFunc(
      runtime::core::rpc::ClientFuncWrapper{.info = client}));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  std_msgs::msg::Int32 request;
  request.data = 41;
  std_msgs::msg::Int32 response;
  aimrt::rpc::Context context;
  context.SetTimeout(2s);
  std::promise<aimrt::rpc::Status> promise;
  auto future = promise.get_future();
  auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &request,
                                        .rsp_ptr = &response,
                                        .ctx_ref = context});
  invoke->callback = [&promise](aimrt::rpc::Status status) {
    promise.set_value(std::move(status));
  };
  harness.Backend().Invoke(invoke);
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(future.get().OK());
  EXPECT_EQ(response.data, 42);
}
#endif

TEST(DdsRpcBadRequest, RequestReaderTakeFailureRetriesWithoutCorrelation) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("bad_request_take_server", 1);
  const auto client = MakeRpcInfo("bad_request_take_client", 2);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  harness.Backend().ForceNextRequestTakeResultForTesting(
      server, static_cast<int32_t>(eprosima::fastdds::dds::RETCODE_ERROR));
  auto call = StartAsyncCall(harness, client, 20, 22, 2s);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend().DiagnosticsForTesting().reader_take_failure_total ==
           1;
  }));
  ASSERT_EQ(call->future.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(call->future.get().OK());
  EXPECT_EQ(call->response.sum(), 42);
  EXPECT_EQ(call->callback_count->load(), 1U);
  EXPECT_EQ(control->calls.load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
}

TEST(DdsRpcTimeout, LocalTimerCompletesExactlyOnce) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("timeout_server", 1);
  const auto client = MakeRpcInfo("timeout_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  example::AddRequest request;
  example::AddResponse response;
  std::atomic_uint32_t callbacks = 0;
  const auto status = harness.Invoke(client, request, response, 80ms, &callbacks);
  EXPECT_EQ(status.Code(), AIMRT_RPC_STATUS_TIMEOUT);
  EXPECT_EQ(callbacks.load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
}

TEST(DdsRpcTimeout, ExitedAndRestartedServerDoesNotRetryTimedOutCall) {
  test::ProcessTestScope processes(20s);
  ASSERT_TRUE(processes.Ready());
  RpcHarness harness;
  const std::string function_name =
      "dds:/example::Calculator/AddR05_" + std::to_string(getpid());
  const auto client = MakeRpcInfo("restart_client", 1, function_name);
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();

  auto* exiting_server =
      processes.Launch("--dds-held-server", {function_name}, true);
  ASSERT_TRUE(exiting_server);
  ASSERT_TRUE(processes.ReadEvent(exiting_server, 'S', 5s));
  ASSERT_TRUE(harness.WaitReady(client, processes.Remaining(8s)));

  auto timed_out_call = StartAsyncCall(harness, client, 19, 23, 500ms);
  ASSERT_TRUE(processes.ReadEvent(exiting_server, 'H', 5s));
  ASSERT_TRUE(processes.SendControl(exiting_server, 'Q'));
  const auto exit_result = processes.Finish(exiting_server, 5s);
  EXPECT_TRUE(exit_result.process_group_established);
  ASSERT_TRUE(exit_result.child_reaped);
  ASSERT_TRUE(WIFEXITED(exit_result.status));
  EXPECT_EQ(WEXITSTATUS(exit_result.status), 0);
  EXPECT_FALSE(exit_result.term_sent);
  EXPECT_FALSE(exit_result.kill_fallback_sent);
  EXPECT_TRUE(exit_result.no_residual_process_group);

  ASSERT_EQ(timed_out_call->future.wait_for(processes.Remaining(2s)),
            std::future_status::ready);
  EXPECT_EQ(timed_out_call->future.get().Code(), AIMRT_RPC_STATUS_TIMEOUT);
  EXPECT_EQ(timed_out_call->callback_count->load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
  ASSERT_TRUE(WaitFor(
      [&] { return harness.Backend().ReadinessForTesting(client) != "MATCHED"; },
      processes.Remaining(5s)));

  auto* restarted_server =
      processes.Launch("--dds-responding-server", {function_name}, true);
  ASSERT_TRUE(restarted_server);
  ASSERT_TRUE(processes.ReadEvent(restarted_server, 'S', 5s));
  ASSERT_TRUE(harness.WaitReady(client, processes.Remaining(8s)));
  example::AddRequest request;
  request.lhs(20);
  request.rhs(22);
  example::AddResponse response;
  const auto status =
      harness.Invoke(client, request, response, processes.Remaining(2s));
  ASSERT_TRUE(status.OK()) << status.ToString();
  EXPECT_EQ(response.sum(), 42);
  ASSERT_TRUE(processes.ReadEvent(restarted_server, 'H', 5s));
  ASSERT_TRUE(processes.ReadEvent(restarted_server, '1', 5s));
  const auto restart_result = processes.Finish(restarted_server, 0ms);
  EXPECT_TRUE(restart_result.process_group_established);
  ASSERT_TRUE(restart_result.child_reaped);
  ASSERT_TRUE(WIFSIGNALED(restart_result.status));
  EXPECT_EQ(WTERMSIG(restart_result.status), SIGTERM);
  EXPECT_TRUE(restart_result.term_sent);
  EXPECT_FALSE(restart_result.kill_fallback_sent);
  EXPECT_TRUE(restart_result.no_residual_process_group);
  EXPECT_EQ(timed_out_call->callback_count->load(), 1U);
  EXPECT_EQ(processes.ReceivedSignal(), 0);
  EXPECT_FALSE(processes.Expired());
}

TEST(DdsRpcResponseAnomalies, ClassifiesForeignLateDuplicateAndUnknown) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("anomaly_server", 1);
  const auto client = MakeRpcInfo("anomaly_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  auto long_call = StartAsyncCall(harness, client, 20, 22, 2s);
  ASSERT_TRUE(WaitFor(
      [&] { return harness.Backend().PendingCountForTesting(client) == 1; }));
  auto long_tokens = harness.Backend().PendingTokensForTesting(client);
  ASSERT_EQ(long_tokens.size(), 1U);
  const auto long_token = long_tokens.front();

  auto short_call = StartAsyncCall(harness, client, 1, 2, 80ms);
  ASSERT_TRUE(WaitFor(
      [&] { return harness.Backend().PendingCountForTesting(client) == 2; }));
  auto tokens = harness.Backend().PendingTokensForTesting(client);
  ASSERT_EQ(tokens.size(), 2U);
  const auto short_token = tokens.front() == long_token ? tokens.back() : tokens.front();
  ASSERT_EQ(short_call->future.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(short_call->future.get().Code(), AIMRT_RPC_STATUS_TIMEOUT);

  const auto names = DeriveDdsRpcTopicNames(client.func_name);
  auto& endpoints = harness.Runtime().Endpoints();
  auto* response_writer = endpoints.GetOrCreateWriter(
      names.response,
      aimrt::GetStaticDdsTypeSupportHandle<example::AddResponse>(),
      harness.Context().Qos().rpc_writer, "dds09 anomaly response writer");
  auto foreign_reader = endpoints.GetOrCreateReader(
      names.response,
      aimrt::GetStaticDdsTypeSupportHandle<example::AddResponse>(),
      harness.Context().Qos().rpc_reader, "dds09 anomaly foreign reader");
  ASSERT_TRUE(WaitFor([&] { return WriterMatched(response_writer); }));

  example::AddResponse response;
  response.sum(42);
  const auto write_response = [&](
                                  const eprosima::fastdds::rtps::SampleIdentity& token) {
    eprosima::fastdds::rtps::WriteParams params;
    params.related_sample_identity(token);
    return response_writer->write(&response, params);
  };

  auto foreign_token = long_token;
  foreign_token.writer_guid(foreign_reader.reader->guid());
  ASSERT_EQ(write_response(foreign_token), eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend()
               .DiagnosticsForTesting()
               .rpc_foreign_response_total == 1;
  }));
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 1U);

  ASSERT_EQ(write_response(short_token), eprosima::fastdds::dds::RETCODE_OK);
  auto unknown_token = long_token;
  unknown_token.sequence_number(
      eprosima::fastdds::rtps::SequenceNumber_t(0, 2000000000U));
  ASSERT_EQ(write_response(unknown_token), eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_EQ(write_response(long_token), eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_EQ(long_call->future.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(long_call->future.get().OK());
  ASSERT_EQ(write_response(long_token), eprosima::fastdds::dds::RETCODE_OK);

  ASSERT_TRUE(WaitFor([&] {
    const auto snapshot = harness.Backend().DiagnosticsForTesting();
    return snapshot.rpc_late_response_total == 1 &&
           snapshot.rpc_duplicate_response_total == 1 &&
           snapshot.rpc_unknown_correlation_total == 1;
  }));
  EXPECT_EQ(long_call->callback_count->load(), 1U);
  EXPECT_EQ(short_call->callback_count->load(), 1U);
}

TEST(DdsRpcResponseAnomalies,
     ResponseTimeoutAndShutdownRaceCompletesExactlyOnce) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("terminal_race_server", 1);
  const auto client = MakeRpcInfo("terminal_race_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  std::promise<void> response_ready;
  std::promise<void> timeout_ready;
  std::promise<void> shutdown_ready;
  std::promise<void> release_race;
  auto release_future = release_race.get_future().share();
  harness.Backend().SetBeforeResponseWriteHookForTesting(
      [&] {
        response_ready.set_value();
        release_future.wait();
      });
  harness.Backend().SetBeforeTimeoutCompletionHookForTesting(
      [&] {
        timeout_ready.set_value();
        release_future.wait();
      });
  harness.Backend().SetAfterShutdownStartedHookForTesting(
      [&] {
        shutdown_ready.set_value();
        release_future.wait();
      });

  auto call = StartAsyncCall(harness, client, 20, 22, 80ms);
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard lock(control->mutex);
    return harness.Backend().PendingCountForTesting(client) == 1 &&
           control->held.size() == 1;
  }));
  auto held = TakeHeldInvoke(control);
  ASSERT_TRUE(held);
  auto response_task = std::async(std::launch::async, [&] {
    auto& response = *static_cast<example::AddResponse*>(held->rsp_ptr);
    response.sum(42);
    held->callback(aimrt::rpc::Status());
  });
  ASSERT_EQ(response_ready.get_future().wait_for(2s),
            std::future_status::ready);
  ASSERT_EQ(timeout_ready.get_future().wait_for(2s),
            std::future_status::ready);
  auto shutdown_task =
      std::async(std::launch::async, [&] { harness.Backend().Shutdown(); });
  ASSERT_EQ(shutdown_ready.get_future().wait_for(2s),
            std::future_status::ready);
  const auto response_writes_before =
      harness.Backend().ResponseWriteCallCountForTesting();

  release_race.set_value();
  response_task.get();
  shutdown_task.get();
  ASSERT_EQ(call->future.wait_for(2s), std::future_status::ready);
  const auto status = call->future.get();
  EXPECT_EQ(status.Code(), AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
  EXPECT_EQ(call->callback_count->load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
  EXPECT_EQ(harness.Backend().TerminalHistoryCountForTesting(client), 1U);
  EXPECT_EQ(harness.Backend().ResponseWriteCallCountForTesting(),
            response_writes_before);
  EXPECT_EQ(harness.Backend()
                .DiagnosticsForTesting()
                .rpc_late_server_completion_total,
            1U);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(call->callback_count->load(), 1U);
}

TEST(DdsRpcResponseAnomalies, TerminalHistoryIsBoundedFifo) {
  RpcHarness harness;
  const auto client = MakeRpcInfo("terminal_history_client", 1);
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();

  const auto evicted =
      harness.Backend().FillTerminalHistoryForTesting(client, 1023);
  EXPECT_EQ(harness.Backend().TerminalHistoryCountForTesting(client), 1023U);
  harness.Backend().FillTerminalHistoryForTesting(client, 1);
  EXPECT_EQ(harness.Backend().TerminalHistoryCountForTesting(client), 1024U);
  harness.Backend().FillTerminalHistoryForTesting(client, 1);
  EXPECT_EQ(harness.Backend().TerminalHistoryCountForTesting(client), 1024U);
  EXPECT_EQ(harness.Backend()
                .DiagnosticsForTesting()
                .rpc_terminal_history_evicted_total,
            1U);

  const auto names = DeriveDdsRpcTopicNames(client.func_name);
  auto* response_writer = harness.Runtime().Endpoints().GetOrCreateWriter(
      names.response,
      aimrt::GetStaticDdsTypeSupportHandle<example::AddResponse>(),
      harness.Context().Qos().rpc_writer,
      "dds09 terminal history response writer");
  ASSERT_TRUE(WaitFor([&] { return WriterMatched(response_writer); }));
  example::AddResponse response;
  response.sum(42);
  eprosima::fastdds::rtps::WriteParams params;
  params.related_sample_identity(evicted);
  ASSERT_EQ(response_writer->write(&response, params),
            eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend()
               .DiagnosticsForTesting()
               .rpc_unknown_correlation_total == 1;
  }));
}

TEST(DdsRpcTakeAtomicity, ReaderFailureDoesNotCorrelateOrCompletePending) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("take_atomicity_server", 1);
  const auto client = MakeRpcInfo("take_atomicity_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  auto call = StartAsyncCall(harness, client, 19, 23, 2s);
  ASSERT_TRUE(WaitFor(
      [&] { return harness.Backend().PendingCountForTesting(client) == 1; }));
  const auto tokens = harness.Backend().PendingTokensForTesting(client);
  ASSERT_EQ(tokens.size(), 1U);
  const auto names = DeriveDdsRpcTopicNames(client.func_name);
  auto* response_writer = harness.Runtime().Endpoints().GetOrCreateWriter(
      names.response,
      aimrt::GetStaticDdsTypeSupportHandle<example::AddResponse>(),
      harness.Context().Qos().rpc_writer, "dds09 atomicity response writer");
  ASSERT_TRUE(WaitFor([&] { return WriterMatched(response_writer); }));

  harness.Backend().ForceNextResponseTakeResultForTesting(
      client, static_cast<int32_t>(eprosima::fastdds::dds::RETCODE_ERROR));
  example::AddResponse response;
  response.sum(42);
  eprosima::fastdds::rtps::WriteParams params;
  params.related_sample_identity(tokens.front());
  ASSERT_EQ(response_writer->write(&response, params),
            eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend().DiagnosticsForTesting().reader_take_failure_total ==
           1;
  }));
  ASSERT_EQ(call->future.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(call->future.get().OK());
  EXPECT_EQ(call->response.sum(), 42);
  EXPECT_EQ(call->callback_count->load(), 1U);
}

TEST(DdsRpcSendFailure, FailedWriterDoesNotPublishPendingOrRetry) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("send_failure_server", 1);
  const auto client = MakeRpcInfo("send_failure_client", 2);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  harness.Backend().FailNextRequestWriteForTesting();
  example::AddRequest request;
  example::AddResponse response;
  std::atomic_uint32_t callbacks = 0;
  EXPECT_EQ(harness.Invoke(client, request, response, 1s, &callbacks).Code(),
            AIMRT_RPC_STATUS_CLI_SEND_REQ_FAILED);
  EXPECT_EQ(callbacks.load(), 1U);
  EXPECT_EQ(control->calls.load(), 0U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
}

TEST(DdsRpcPendingLimit, RejectsThe1025thActiveCallWithoutWriting) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("pending_limit_server", 1);
  const auto client = MakeRpcInfo("pending_limit_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  std::vector<std::unique_ptr<AsyncRpcCall>> calls;
  calls.reserve(1025);
  for (size_t index = 0; index < 1025; ++index) {
    calls.emplace_back(StartAsyncCall(harness, client,
                                      static_cast<int32_t>(index), 1, 30s));
  }
  ASSERT_EQ(calls.back()->future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(calls.back()->future.get().Code(), AIMRT_RPC_STATUS_CLI_NOT_READY);
  EXPECT_EQ(calls.back()->callback_count->load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 1024U);
  EXPECT_EQ(harness.Backend()
                .DiagnosticsForTesting()
                .rpc_pending_rejected_total,
            1U);

  harness.Backend().Shutdown();
  for (size_t index = 0; index < 1024; ++index) {
    ASSERT_EQ(calls[index]->future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(calls[index]->future.get().Code(),
              AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
    EXPECT_EQ(calls[index]->callback_count->load(), 1U);
  }
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
  EXPECT_EQ(harness.Backend().TerminalHistoryCountForTesting(client), 1024U);
}

TEST(DdsRpcPendingLimit,
     ReclaimsSlotsAcrossResponseTimeoutAndWriteFailureBoundaries) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("pending_reclaim_server", 1);
  const auto client = MakeRpcInfo("pending_reclaim_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  std::vector<std::unique_ptr<AsyncRpcCall>> calls;
  calls.reserve(1023);
  for (size_t index = 0; index < 1023; ++index) {
    calls.emplace_back(StartAsyncCall(
        harness, client, static_cast<int32_t>(index), 1, 30s));
  }
  ASSERT_TRUE(WaitFor(
      [&] { return harness.Backend().PendingCountForTesting(client) == 1023; },
      5s));
  auto timeout_call = StartAsyncCall(harness, client, 1023, 1, 150ms);
  ASSERT_TRUE(WaitFor(
      [&] { return harness.Backend().PendingCountForTesting(client) == 1024; }));

  auto overflow = StartAsyncCall(harness, client, 1024, 1, 30s);
  ASSERT_EQ(overflow->future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(overflow->future.get().Code(), AIMRT_RPC_STATUS_CLI_NOT_READY);
  EXPECT_EQ(overflow->callback_count->load(), 1U);

  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard lock(control->mutex);
    return control->held.size() >= 1024;
  },
                      5s));
  auto response_invoke = TakeHeldInvoke(control);
  ASSERT_TRUE(response_invoke);
  auto& response = *static_cast<example::AddResponse*>(response_invoke->rsp_ptr);
  const auto& request =
      *static_cast<const example::AddRequest*>(response_invoke->req_ptr);
  response.sum(request.lhs() + request.rhs());
  auto response_task = std::async(std::launch::async, [response_invoke] {
    response_invoke->callback(aimrt::rpc::Status());
  });
  ASSERT_TRUE(WaitFor(
      [&] { return harness.Backend().PendingCountForTesting(client) <= 1023; }));

  harness.Backend().FailNextRequestWriteForTesting();
  auto write_failure = StartAsyncCall(harness, client, 2000, 1, 30s);
  ASSERT_EQ(write_failure->future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(write_failure->future.get().Code(),
            AIMRT_RPC_STATUS_CLI_SEND_REQ_FAILED);
  EXPECT_EQ(write_failure->callback_count->load(), 1U);
  response_task.get();
  ASSERT_EQ(calls.front()->future.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(calls.front()->future.get().OK());
  EXPECT_EQ(calls.front()->callback_count->load(), 1U);

  ASSERT_EQ(timeout_call->future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(timeout_call->future.get().Code(), AIMRT_RPC_STATUS_TIMEOUT);
  EXPECT_EQ(timeout_call->callback_count->load(), 1U);
  ASSERT_TRUE(WaitFor(
      [&] { return harness.Backend().PendingCountForTesting(client) == 1022; }));

  auto refill_one = StartAsyncCall(harness, client, 3000, 1, 30s);
  auto refill_two = StartAsyncCall(harness, client, 3001, 1, 30s);
  ASSERT_TRUE(WaitFor(
      [&] { return harness.Backend().PendingCountForTesting(client) == 1024; }));
  auto second_overflow = StartAsyncCall(harness, client, 3002, 1, 30s);
  ASSERT_EQ(second_overflow->future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(second_overflow->future.get().Code(),
            AIMRT_RPC_STATUS_CLI_NOT_READY);
  EXPECT_EQ(second_overflow->callback_count->load(), 1U);

  harness.Backend().Shutdown();
  for (size_t index = 1; index < calls.size(); ++index) {
    ASSERT_EQ(calls[index]->future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(calls[index]->future.get().Code(),
              AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
    EXPECT_EQ(calls[index]->callback_count->load(), 1U);
  }
  for (auto* refill : {refill_one.get(), refill_two.get()}) {
    ASSERT_EQ(refill->future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(refill->future.get().Code(),
              AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
    EXPECT_EQ(refill->callback_count->load(), 1U);
  }
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
  EXPECT_EQ(harness.Backend().TerminalHistoryCountForTesting(client), 1024U);
}

TEST(DdsRpcServerNonOk, DoesNotPutStatusOnWireAndClientTimesOut) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("server_non_ok_server", 1);
  const auto client = MakeRpcInfo("server_non_ok_client", 2);
  runtime::core::rpc::ServiceFuncWrapper service{
      .info = server,
      .service_func = [](
                          const std::shared_ptr<runtime::core::rpc::InvokeWrapper>& invoke) {
        invoke->callback(
            aimrt::rpc::Status(AIMRT_RPC_STATUS_SVR_HANDLE_FAILED));
        invoke->callback(aimrt::rpc::Status());
      }};
  ASSERT_TRUE(harness.Backend().RegisterServiceFunc(service));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  example::AddRequest request;
  example::AddResponse response;
  const auto status = harness.Invoke(client, request, response, 100ms);
  EXPECT_EQ(status.Code(), AIMRT_RPC_STATUS_TIMEOUT);
  const auto diagnostics = harness.Backend().DiagnosticsForTesting();
  EXPECT_EQ(diagnostics.rpc_server_non_ok_completion_total, 1U);
  EXPECT_EQ(diagnostics.rpc_late_server_completion_total, 1U);
}

TEST(DdsRpcReplyMatchWait, PartialMatchUsesAsioAndStopsAfterThreeSeconds) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("match_wait_server", 1);
  const auto client = MakeRpcInfo("match_wait_client", 2);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  std::atomic_uint32_t attempts = 0;
  harness.Backend().SetRequesterMatchOverrideForTesting([&attempts] {
    return attempts.fetch_add(1) < 3 ? 1 : 2;
  });
  example::AddRequest request;
  request.lhs(40);
  request.rhs(2);
  example::AddResponse response;
  const auto delayed_begin = std::chrono::steady_clock::now();
  const auto delayed_status = harness.Invoke(client, request, response, 2s);
  const auto delayed_elapsed = std::chrono::steady_clock::now() - delayed_begin;
  ASSERT_TRUE(delayed_status.OK()) << delayed_status.ToString();
  EXPECT_EQ(response.sum(), 42);
  EXPECT_GE(delayed_elapsed, 250ms);

  harness.Backend().SetRequesterMatchOverrideForTesting([] { return 1; });
  const auto timeout_begin = std::chrono::steady_clock::now();
  const auto timeout_status = harness.Invoke(client, request, response, 3200ms);
  const auto timeout_elapsed = std::chrono::steady_clock::now() - timeout_begin;
  EXPECT_EQ(timeout_status.Code(), AIMRT_RPC_STATUS_TIMEOUT);
  EXPECT_GE(timeout_elapsed, 3s);
  EXPECT_LT(timeout_elapsed, 4s);
}

TEST(DdsRpcRealDiscovery,
     DelayedExactReplyReaderAppearsAndMissingReaderExpiresWithoutBlocking) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("real_discovery_server", 1);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));

  const auto names = DeriveDdsRpcTopicNames(server.func_name);
  auto& endpoints = harness.Runtime().Endpoints();
  auto* request_writer = endpoints.GetOrCreateWriter(
      names.request, aimrt::GetStaticDdsTypeSupportHandle<example::AddRequest>(),
      harness.Context().Qos().rpc_writer,
      "rpc test real discovery request writer");
  const auto delayed_reader = endpoints.GetOrCreateReader(
      names.response, aimrt::GetStaticDdsTypeSupportHandle<example::AddResponse>(),
      harness.Context().Qos().rpc_reader,
      "rpc test real discovery delayed response reader");
  const auto ready_reader = endpoints.GetOrCreateReader(
      names.response, aimrt::GetStaticDdsTypeSupportHandle<example::AddResponse>(),
      harness.Context().Qos().rpc_reader,
      "rpc test real discovery ready response reader");
  const auto expired_reader = endpoints.GetOrCreateReader(
      names.response, aimrt::GetStaticDdsTypeSupportHandle<example::AddResponse>(),
      harness.Context().Qos().rpc_reader,
      "rpc test real discovery expired response reader");
  ASSERT_NE(request_writer, nullptr);
  ASSERT_NE(delayed_reader.reader, nullptr);
  ASSERT_NE(ready_reader.reader, nullptr);
  ASSERT_NE(expired_reader.reader, nullptr);
  harness.Start();
  ASSERT_TRUE(endpoints.EnableWriter(request_writer));

  const auto writer_is_matched = [&] {
    eprosima::fastdds::dds::PublicationMatchedStatus status;
    return request_writer->get_publication_matched_status(status) ==
               eprosima::fastdds::dds::RETCODE_OK &&
           status.current_count >= 1;
  };
  const auto reader_is_matched = [](eprosima::fastdds::dds::DataReader* reader) {
    eprosima::fastdds::dds::SubscriptionMatchedStatus status;
    return reader->get_subscription_matched_status(status) ==
               eprosima::fastdds::dds::RETCODE_OK &&
           status.current_count >= 1;
  };
  const auto write_request = [request_writer](
                                 int32_t lhs, int32_t rhs,
                                 const eprosima::fastdds::rtps::GUID_t& reply_reader) {
    example::AddRequest request;
    request.lhs(lhs);
    request.rhs(rhs);
    eprosima::fastdds::rtps::SampleIdentity related;
    related.writer_guid(reply_reader);
    related.sequence_number(eprosima::fastdds::rtps::c_SequenceNumber_Unknown);
    eprosima::fastdds::rtps::WriteParams params;
    params.related_sample_identity(related);
    const auto result = request_writer->write(&request, params);
    return std::pair{result, params.sample_identity()};
  };
  const auto take_response = [](eprosima::fastdds::dds::DataReader* reader) {
    example::AddResponse response;
    eprosima::fastdds::dds::SampleInfo info;
    const auto result = reader->take_next_sample(&response, &info);
    return std::tuple{result, response, info};
  };

  ASSERT_TRUE(WaitFor(writer_is_matched));
  const auto [delayed_write_result, delayed_request_identity] =
      write_request(10, 32, delayed_reader.reader->guid());
  ASSERT_EQ(delayed_write_result, eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] { return control->calls.load() == 1; }));
  EXPECT_TRUE(writer_is_matched());

  ASSERT_TRUE(endpoints.EnableReader(ready_reader.reader));
  ASSERT_TRUE(WaitFor([&] { return reader_is_matched(ready_reader.reader); }));
  const auto prompt_begin = std::chrono::steady_clock::now();
  const auto [ready_write_result, ready_request_identity] =
      write_request(20, 22, ready_reader.reader->guid());
  ASSERT_EQ(ready_write_result, eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] { return ready_reader.reader->get_unread_count() > 0; },
                      1s));
  const auto prompt_elapsed = std::chrono::steady_clock::now() - prompt_begin;
  auto [ready_take_result, ready_response, ready_info] =
      take_response(ready_reader.reader);
  ASSERT_EQ(ready_take_result, eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(ready_info.valid_data);
  EXPECT_EQ(ready_response.sum(), 42);
  EXPECT_EQ(ready_info.related_sample_identity.writer_guid(),
            ready_reader.reader->guid());
  EXPECT_EQ(ready_info.related_sample_identity.sequence_number(),
            ready_request_identity.sequence_number());
  EXPECT_LT(prompt_elapsed, 1s);

  ASSERT_TRUE(endpoints.EnableReader(delayed_reader.reader));
  ASSERT_TRUE(WaitFor([&] { return reader_is_matched(delayed_reader.reader); }));
  ASSERT_TRUE(WaitFor(
      [&] { return delayed_reader.reader->get_unread_count() > 0; }, 2s));
  auto [delayed_take_result, delayed_response, delayed_info] =
      take_response(delayed_reader.reader);
  ASSERT_EQ(delayed_take_result, eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(delayed_info.valid_data);
  EXPECT_EQ(delayed_response.sum(), 42);
  EXPECT_EQ(delayed_info.related_sample_identity.writer_guid(),
            delayed_reader.reader->guid());
  EXPECT_EQ(delayed_info.related_sample_identity.sequence_number(),
            delayed_request_identity.sequence_number());
  EXPECT_EQ(control->calls.load(), 2U);
  EXPECT_TRUE(writer_is_matched());

  const auto expiry_begin = std::chrono::steady_clock::now();
  const auto [expired_write_result, expired_request_identity] =
      write_request(40, 2, expired_reader.reader->guid());
  ASSERT_EQ(expired_write_result, eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] { return control->calls.load() == 3; }));
  while (std::chrono::steady_clock::now() - expiry_begin < 3300ms) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(endpoints.EnableReader(expired_reader.reader));
  ASSERT_TRUE(WaitFor([&] { return reader_is_matched(expired_reader.reader); }));
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(expired_reader.reader->get_unread_count(), 0);
  EXPECT_EQ(expired_request_identity.writer_guid(), request_writer->guid());
  EXPECT_TRUE(writer_is_matched());
}

TEST(DdsRpcRequesterDisconnect, MissingRequestWriterDropsHeldReplyAsNoData) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("disconnect_server", 1);
  const auto client = MakeRpcInfo("disconnect_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  example::AddRequest request;
  request.lhs(20);
  request.rhs(22);
  example::AddResponse response;
  auto call = std::async(std::launch::async, [&] {
    return harness.Invoke(client, request, response, 200ms);
  });
  ASSERT_TRUE(WaitFor([&] { return control->calls.load() == 1; }));
  harness.Backend().DisconnectRequestWriterForTesting(client);

  std::shared_ptr<runtime::core::rpc::InvokeWrapper> held;
  {
    std::lock_guard lock(control->mutex);
    ASSERT_EQ(control->held.size(), 1U);
    held = std::move(control->held.front());
    control->held.clear();
  }
  auto& held_response = *static_cast<example::AddResponse*>(held->rsp_ptr);
  held_response.sum(42);
  held->callback(aimrt::rpc::Status());
  EXPECT_EQ(call.get().Code(), AIMRT_RPC_STATUS_TIMEOUT);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
}

TEST(DdsRpcTerminalization, ScheduleFailureAndShutdownCompleteExactlyOnce) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("terminal_server", 1);
  const auto client = MakeRpcInfo("terminal_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  harness.Backend().FailNextScheduleForTesting();
  example::AddRequest request;
  example::AddResponse response;
  std::atomic_uint32_t callbacks = 0;
  auto status = harness.Invoke(client, request, response, 5s, &callbacks);
  EXPECT_EQ(status.Code(), AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
  EXPECT_EQ(callbacks.load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);

  aimrt::rpc::Context context;
  context.SetTimeout(5s);
  std::promise<aimrt::rpc::Status> promise;
  auto future = promise.get_future();
  auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &request,
                                        .rsp_ptr = &response,
                                        .ctx_ref = context});
  invoke->callback = [&promise, &callbacks](aimrt::rpc::Status result) {
    callbacks.fetch_add(1);
    promise.set_value(std::move(result));
  };
  callbacks = 0;
  harness.Backend().Invoke(invoke);
  ASSERT_TRUE(WaitFor([&] { return harness.Backend().PendingCountForTesting(client) == 1; }));
  harness.Backend().Shutdown();
  ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(future.get().Code(), AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
  EXPECT_EQ(callbacks.load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
}

TEST(DdsRpcShutdown, AtomicallyMovesPendingToHistoryAndRejectsLateWork) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("shutdown_server", 1);
  const auto client = MakeRpcInfo("shutdown_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  auto call = StartAsyncCall(harness, client, 20, 22, 5s);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend().PendingCountForTesting(client) == 1 &&
           control->calls.load() == 1;
  }));
  harness.Backend().Shutdown();
  ASSERT_EQ(call->future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(call->future.get().Code(),
            AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
  EXPECT_EQ(call->callback_count->load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
  EXPECT_EQ(harness.Backend().TerminalHistoryCountForTesting(client), 1U);

  auto held = TakeHeldInvoke(control);
  ASSERT_TRUE(held);
  auto& held_response = *static_cast<example::AddResponse*>(held->rsp_ptr);
  held_response.sum(42);
  held->callback(aimrt::rpc::Status());
  EXPECT_EQ(harness.Backend()
                .DiagnosticsForTesting()
                .rpc_late_server_completion_total,
            1U);

  auto rejected = StartAsyncCall(harness, client, 1, 1, 1s);
  ASSERT_EQ(rejected->future.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(rejected->future.get().Code(), AIMRT_RPC_STATUS_SVR_NOT_FOUND);
  EXPECT_EQ(rejected->callback_count->load(), 1U);
}

TEST(DdsRpcShutdown,
     CancelsGateBusyRetryAndRejectsQueuedSenderExactlyOnce) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("gate_shutdown_server", 1);
  const auto client = MakeRpcInfo("gate_shutdown_client", 2);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  std::promise<void> gate_busy_ready;
  std::promise<void> release_publication_gate;
  auto release_gate_future = release_publication_gate.get_future().share();
  std::atomic_bool observed_gate_busy = false;
  harness.Backend().SetAfterRequestWriteHookForTesting([&] {
    observed_gate_busy = WaitFor(
        [&] {
          return harness.Backend().ResponseGateBusyRetryCountForTesting(client) !=
                     0 &&
                 harness.Backend().ResponseUnreadCountForTesting(client) != 0;
        },
        2s);
    gate_busy_ready.set_value();
    release_gate_future.wait();
  });

  auto first = PrepareAsyncCall(client, 20, 22, 5s);
  auto first_invoke = std::async(std::launch::async, [&] {
    harness.Backend().Invoke(first->invoke);
  });
  ASSERT_EQ(gate_busy_ready.get_future().wait_for(3s),
            std::future_status::ready);
  ASSERT_TRUE(observed_gate_busy.load());
  auto drain = harness.Backend().ClientDrainForTesting(client);
  ASSERT_TRUE(drain);

  auto queued = PrepareAsyncCall(client, 1, 1, 5s);
  std::promise<void> queued_sender_started;
  auto queued_invoke = std::async(std::launch::async, [&] {
    queued_sender_started.set_value();
    harness.Backend().Invoke(queued->invoke);
  });
  ASSERT_EQ(queued_sender_started.get_future().wait_for(1s),
            std::future_status::ready);

  std::promise<void> shutdown_started;
  harness.Backend().SetAfterShutdownStartedHookForTesting(
      [&] { shutdown_started.set_value(); });
  auto shutdown =
      std::async(std::launch::async, [&] { harness.Backend().Shutdown(); });
  ASSERT_EQ(shutdown_started.get_future().wait_for(2s),
            std::future_status::ready);
  EXPECT_FALSE(harness.Backend().RunningForTesting());
  EXPECT_EQ(shutdown.wait_for(20ms), std::future_status::timeout);

  release_publication_gate.set_value();
  first_invoke.get();
  queued_invoke.get();
  shutdown.get();
  ASSERT_EQ(first->future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(first->future.get().Code(),
            AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
  EXPECT_EQ(first->callback_count->load(), 1U);
  ASSERT_EQ(queued->future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(queued->future.get().Code(), AIMRT_RPC_STATUS_SVR_NOT_FOUND);
  EXPECT_EQ(queued->callback_count->load(), 1U);
  EXPECT_EQ(control->calls.load(), 1U);
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
  EXPECT_EQ(harness.Backend().TerminalHistoryCountForTesting(client), 1U);
  EXPECT_GE(harness.Backend().ResponseUnreadCountForTesting(client), 1);
  const auto drain_executions = drain->DrainExecutionCount();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(drain->DrainExecutionCount(), drain_executions);
  EXPECT_EQ(first->callback_count->load(), 1U);
  EXPECT_EQ(queued->callback_count->load(), 1U);
}

TEST(DdsRpcReentrancy, CompletionCanInvokeAgainWithoutPendingMutexDeadlock) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("reentrant_server", 1);
  const auto client = MakeRpcInfo("reentrant_client", 2);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  example::AddRequest first_request;
  first_request.lhs(1);
  first_request.rhs(2);
  example::AddResponse first_response;
  example::AddRequest second_request;
  second_request.lhs(20);
  second_request.rhs(22);
  example::AddResponse second_response;
  aimrt::rpc::Context first_context;
  first_context.SetTimeout(2s);
  aimrt::rpc::Context second_context;
  second_context.SetTimeout(2s);
  std::promise<bool> promise;
  auto future = promise.get_future();
  auto second = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &second_request,
                                        .rsp_ptr = &second_response,
                                        .ctx_ref = second_context});
  second->callback = [&promise, &second_response](aimrt::rpc::Status status) {
    promise.set_value(status.OK() && second_response.sum() == 42);
  };
  auto first = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &first_request,
                                        .rsp_ptr = &first_response,
                                        .ctx_ref = first_context});
  first->callback = [&harness, second](aimrt::rpc::Status status) {
    if (status.OK()) harness.Backend().Invoke(second);
  };
  harness.Backend().Invoke(first);
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(future.get());
  EXPECT_EQ(harness.Backend().PendingCountForTesting(client), 0U);
}

TEST(DdsRpcEndpointOwnership, SameFunctionEndpointsAreUniqueAndRollbackIsTransactional) {
  RpcHarness harness;
  const auto server_one = MakeRpcInfo("owner_server_one", 1);
  const auto server_two = MakeRpcInfo("owner_server_two", 2);
  const auto client_one = MakeRpcInfo("owner_client_one", 3);
  const auto client_two = MakeRpcInfo("owner_client_two", 4);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server_one, control));
  ASSERT_TRUE(harness.RegisterServer(server_two, control));
  ASSERT_TRUE(harness.RegisterClient(client_one));
  ASSERT_TRUE(harness.RegisterClient(client_two));
  EXPECT_EQ(harness.Runtime().Endpoints().WriterCount(), 4U);
  EXPECT_EQ(harness.Runtime().Endpoints().ReaderCount(), 4U);
  EXPECT_EQ(harness.Runtime().Endpoints().FilteredTopicCount(), 2U);
  EXPECT_EQ(harness.Runtime().Endpoints().TopicCount(), 2U);
  EXPECT_NE(harness.Backend().ResponseReaderGuidForTesting(client_one),
            harness.Backend().ResponseReaderGuidForTesting(client_two));
}

TEST(DdsRpcEndpointRollback, WriterFailureRemovesReaderCftAndAllowsRetry) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("rollback_server", 1);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  const auto writers_before = harness.Runtime().Endpoints().WriterCount();
  const auto readers_before = harness.Runtime().Endpoints().ReaderCount();
  const auto topics_before = harness.Runtime().Endpoints().TopicCount();

  auto invalid_client = MakeRpcInfo("rollback_client", 2);
  invalid_client.req_type_support_ref = aimrt::util::TypeSupportRef(
      aimrt::GetDdsMessageTypeSupport<example::AddResponse>());
  EXPECT_FALSE(harness.RegisterClient(invalid_client));
  EXPECT_EQ(harness.Runtime().Endpoints().WriterCount(), writers_before);
  EXPECT_EQ(harness.Runtime().Endpoints().ReaderCount(), readers_before);
  EXPECT_EQ(harness.Runtime().Endpoints().FilteredTopicCount(), 0U);
  EXPECT_EQ(harness.Runtime().Endpoints().TopicCount(), topics_before);

  const auto valid_client = MakeRpcInfo("rollback_client", 2);
  EXPECT_TRUE(harness.RegisterClient(valid_client));
  EXPECT_EQ(harness.Runtime().Endpoints().WriterCount(), writers_before + 1);
  EXPECT_EQ(harness.Runtime().Endpoints().ReaderCount(), readers_before + 1);
  EXPECT_EQ(harness.Runtime().Endpoints().FilteredTopicCount(), 1U);
}

TEST(DdsRpcEndpointRollback, FilterAndEnableFailuresRollbackAllClientEntities) {
  RpcHarness harness;
  const auto writers_before = harness.Runtime().Endpoints().WriterCount();
  const auto readers_before = harness.Runtime().Endpoints().ReaderCount();
  const auto topics_before = harness.Runtime().Endpoints().TopicCount();

  const auto filter_client = MakeRpcInfo("filter_failure_client", 1);
  harness.Runtime().Endpoints().FailNextFilteredReaderCreationForTesting();
  EXPECT_FALSE(harness.RegisterClient(filter_client));
  EXPECT_EQ(harness.Runtime().Endpoints().WriterCount(), writers_before);
  EXPECT_EQ(harness.Runtime().Endpoints().ReaderCount(), readers_before);
  EXPECT_EQ(harness.Runtime().Endpoints().FilteredTopicCount(), 0U);
  EXPECT_EQ(harness.Runtime().Endpoints().TopicCount(), topics_before);
  EXPECT_TRUE(harness.RegisterClient(filter_client));

  const auto enable_client = MakeRpcInfo("enable_failure_client", 2);
  const auto writers_after_retry = harness.Runtime().Endpoints().WriterCount();
  const auto readers_after_retry = harness.Runtime().Endpoints().ReaderCount();
  const auto filters_after_retry = harness.Runtime().Endpoints().FilteredTopicCount();
  harness.Runtime().Endpoints().FailNextEndpointEnableForTesting();
  EXPECT_FALSE(harness.RegisterClient(enable_client));
  EXPECT_EQ(harness.Runtime().Endpoints().WriterCount(), writers_after_retry);
  EXPECT_EQ(harness.Runtime().Endpoints().ReaderCount(), readers_after_retry);
  EXPECT_EQ(harness.Runtime().Endpoints().FilteredTopicCount(), filters_after_retry);
  EXPECT_TRUE(harness.RegisterClient(enable_client));
}

TEST(DdsRpcLifecycleRace, WriterOperationBlocksDeletionAcrossRequestWrite) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("write_race_server", 1);
  const auto client = MakeRpcInfo("write_race_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  harness.Backend().SetBeforeRequestWriteHookForTesting([&] {
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release; });
  });
  example::AddRequest request;
  example::AddResponse response;
  aimrt::rpc::Context context;
  context.SetTimeout(5s);
  auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &request,
                                        .rsp_ptr = &response,
                                        .ctx_ref = context,
                                        .callback = [](aimrt::rpc::Status) {}});
  std::thread writer([&] { harness.Backend().Invoke(invoke); });
  bool entered_in_time = false;
  {
    std::unique_lock lock(mutex);
    entered_in_time = condition.wait_for(lock, 2s, [&] { return entered; });
  }
  std::atomic_bool stopping_complete = false;
  std::thread stopper([&] {
    harness.Runtime().Endpoints().BeginStopping();
    stopping_complete = true;
  });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(stopping_complete.load());
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  writer.join();
  stopper.join();
  EXPECT_TRUE(entered_in_time);
  EXPECT_TRUE(stopping_complete.load());
  harness.Backend().SetBeforeRequestWriteHookForTesting({});
  harness.Backend().Shutdown();
}

TEST(DdsRpcLifecycleRace, WriterOperationBlocksDeletionAcrossResponseWrite) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("response_race_server", 1);
  const auto client = MakeRpcInfo("response_race_client", 2);
  auto control = std::make_shared<ServiceControl>();
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  harness.Backend().SetBeforeResponseWriteHookForTesting([&] {
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release; });
  });
  example::AddRequest request;
  request.lhs(20);
  request.rhs(22);
  example::AddResponse response;
  aimrt::rpc::Context context;
  context.SetTimeout(5s);
  auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &request,
                                        .rsp_ptr = &response,
                                        .ctx_ref = context,
                                        .callback = [](aimrt::rpc::Status) {}});
  harness.Backend().Invoke(invoke);
  bool entered_in_time = false;
  {
    std::unique_lock lock(mutex);
    entered_in_time = condition.wait_for(lock, 2s, [&] { return entered; });
  }
  std::atomic_bool stopping_complete = false;
  std::thread stopper([&] {
    harness.Runtime().Endpoints().BeginStopping();
    stopping_complete = true;
  });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(stopping_complete.load());
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  stopper.join();
  EXPECT_TRUE(entered_in_time);
  EXPECT_TRUE(stopping_complete.load());
  harness.Backend().SetBeforeResponseWriteHookForTesting({});
  harness.Backend().Shutdown();
}

TEST(DdsRpcLifecycleRace, ReaderDrainsRemainSafeAfterBackendDestruction) {
  RpcHarness harness;
  const auto server = MakeRpcInfo("drain_lifetime_server", 1);
  const auto client = MakeRpcInfo("drain_lifetime_client", 2);
  auto control = std::make_shared<ServiceControl>();
  control->hold = true;
  ASSERT_TRUE(harness.RegisterServer(server, control));
  ASSERT_TRUE(harness.RegisterClient(client));
  harness.Start();
  ASSERT_TRUE(harness.WaitReady(client));

  const auto client_drain = harness.Backend().ClientDrainForTesting(client);
  const auto server_drain = harness.Backend().ServerDrainForTesting(server);
  ASSERT_TRUE(client_drain);
  ASSERT_TRUE(server_drain);

  example::AddRequest request;
  request.lhs(20);
  request.rhs(22);
  example::AddResponse response;
  aimrt::rpc::Context context;
  context.SetTimeout(5s);
  std::promise<aimrt::rpc::Status> promise;
  auto future = promise.get_future();
  std::atomic_uint32_t callback_count = 0;
  auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = client,
                                        .req_ptr = &request,
                                        .rsp_ptr = &response,
                                        .ctx_ref = context});
  invoke->callback = [&promise, &callback_count](aimrt::rpc::Status status) {
    if (callback_count.fetch_add(1) == 0) promise.set_value(std::move(status));
  };
  harness.Backend().Invoke(invoke);
  ASSERT_TRUE(WaitFor([&] { return control->calls.load() == 1; }));
  ASSERT_TRUE(WaitFor([&] {
    return !client_drain->DrainScheduled() && !server_drain->DrainScheduled();
  }));

  const auto names = DeriveDdsRpcTopicNames(server.func_name);
  auto& endpoints = harness.Runtime().Endpoints();
  auto* external_request_writer = endpoints.GetOrCreateWriter(
      names.request, aimrt::GetStaticDdsTypeSupportHandle<example::AddRequest>(),
      harness.Context().Qos().rpc_writer,
      "lifecycle external request writer");
  auto* external_response_writer = endpoints.GetOrCreateWriter(
      names.response, aimrt::GetStaticDdsTypeSupportHandle<example::AddResponse>(),
      harness.Context().Qos().rpc_writer,
      "lifecycle external response writer");
  ASSERT_NE(external_request_writer, nullptr);
  ASSERT_NE(external_response_writer, nullptr);
  const auto writer_is_matched = [](eprosima::fastdds::dds::DataWriter* writer) {
    eprosima::fastdds::dds::PublicationMatchedStatus status;
    return writer->get_publication_matched_status(status) ==
               eprosima::fastdds::dds::RETCODE_OK &&
           status.current_count >= 1;
  };
  ASSERT_TRUE(WaitFor([&] { return writer_is_matched(external_request_writer); }));
  ASSERT_TRUE(WaitFor([&] { return writer_is_matched(external_response_writer); }));

  std::mutex worker_mutex;
  std::condition_variable worker_condition;
  uint32_t workers_entered = 0;
  bool release_workers = false;
  auto release_worker_tasks = [&](int*) {
    {
      std::lock_guard lock(worker_mutex);
      release_workers = true;
    }
    worker_condition.notify_all();
  };
  std::unique_ptr<int, decltype(release_worker_tasks)> worker_release_guard(
      reinterpret_cast<int*>(1), release_worker_tasks);
  for (uint32_t index = 0; index < harness.Runtime().Executor()->ThreadNum(); ++index) {
    ASSERT_TRUE(harness.Runtime().Executor()->Post([&] {
      std::unique_lock lock(worker_mutex);
      ++workers_entered;
      worker_condition.notify_all();
      worker_condition.wait(lock, [&] { return release_workers; });
    }));
  }
  {
    std::unique_lock lock(worker_mutex);
    ASSERT_TRUE(worker_condition.wait_for(lock, 2s, [&] {
      return workers_entered == harness.Runtime().Executor()->ThreadNum();
    }));
  }

  example::AddRequest queued_request;
  queued_request.lhs(1);
  queued_request.rhs(2);
  eprosima::fastdds::rtps::SampleIdentity reply_target;
  reply_target.writer_guid(harness.Backend().ResponseReaderGuidForTesting(client));
  reply_target.sequence_number(eprosima::fastdds::rtps::c_SequenceNumber_Unknown);
  eprosima::fastdds::rtps::WriteParams request_params;
  request_params.related_sample_identity(reply_target);
  ASSERT_EQ(external_request_writer->write(&queued_request, request_params),
            eprosima::fastdds::dds::RETCODE_OK);

  example::AddResponse queued_response;
  queued_response.sum(3);
  reply_target.sequence_number(request_params.sample_identity().sequence_number());
  eprosima::fastdds::rtps::WriteParams response_params;
  response_params.related_sample_identity(reply_target);
  ASSERT_EQ(external_response_writer->write(&queued_response, response_params),
            eprosima::fastdds::dds::RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend().RequestUnreadCountForTesting(server) != 0 &&
           harness.Backend().ResponseUnreadCountForTesting(client) != 0;
  }));
  const auto client_drains_before_shutdown = client_drain->DrainExecutionCount();
  const auto server_drains_before_shutdown = server_drain->DrainExecutionCount();

  harness.DestroyBackendBeforeRuntimeForTesting();
  ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(future.get().Code(), AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
  EXPECT_EQ(callback_count.load(), 1U);

  worker_release_guard.reset();
  ASSERT_TRUE(WaitFor([&] {
    return !client_drain->DrainScheduled() && !server_drain->DrainScheduled();
  }));
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(client_drain->DrainScheduled());
  EXPECT_FALSE(server_drain->DrainScheduled());
  EXPECT_EQ(client_drain->DrainExecutionCount(), client_drains_before_shutdown + 1);
  EXPECT_EQ(server_drain->DrainExecutionCount(), server_drains_before_shutdown + 1);

  std::shared_ptr<runtime::core::rpc::InvokeWrapper> held;
  {
    std::lock_guard lock(control->mutex);
    ASSERT_EQ(control->held.size(), 1U);
    held = control->held.front();
  }
  ASSERT_TRUE(held);
  held->callback(aimrt::rpc::Status());
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(callback_count.load(), 1U);
}

}  // namespace
}  // namespace aimrt::plugins::dds_plugin

int main(int argc, char** argv) {
  if (argc >= 2) {
    const std::string_view mode(argv[1]);
    if (mode == "--dds-independent-client" && (argc == 5 || argc == 7)) {
      const int event_fd = std::stoi(argv[argc - 2]);
      const int control_fd = std::stoi(argv[argc - 1]);
      std::vector<int> expected_closed_fds;
      if (argc == 7) {
        expected_closed_fds = {std::stoi(argv[3]), std::stoi(argv[4])};
      }
      return aimrt::plugins::dds_plugin::RunIndependentClientProcess(
          event_fd, control_fd, argv[2], expected_closed_fds);
    }
    if ((mode == "--dds-held-server" ||
         mode == "--dds-responding-server") &&
        argc == 5) {
      const int event_fd = std::stoi(argv[3]);
      const int control_fd = std::stoi(argv[4]);
      return aimrt::plugins::dds_plugin::RunServerProcess(
          event_fd, control_fd, argv[2], mode == "--dds-held-server");
    }
    if ((mode == "--dds-waiting-child" || mode == "--dds-close-control") &&
        argc == 4) {
      return aimrt::plugins::dds_plugin::RunProtocolProbeProcess(
          std::stoi(argv[2]), std::stoi(argv[3]),
          mode == "--dds-close-control");
    }
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
