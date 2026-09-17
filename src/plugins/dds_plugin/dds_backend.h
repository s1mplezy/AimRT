// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "core/channel/channel_backend_base.h"
#include "core/channel/channel_backend_tools.h"
#include "core/rpc/rpc_backend_base.h"
#include "dds_plugin/dds_config.h"
#include "dds_plugin/dds_runtime.h"

namespace aimrt::plugins::dds_plugin {

struct DdsChannelSubscriptionState;
struct DdsLoanedPublisherState;

struct DdsBackendState {
  DdsBackendState(DdsParticipantContext& participant_context, DdsRuntime& runtime)
      : participant_context(participant_context), runtime(runtime) {}

  DdsParticipantContext& participant_context;
  DdsRuntime& runtime;
  mutable std::mutex mutex;
  DdsNameRegistry names;
  std::vector<eprosima::fastdds::dds::TypeSupport> type_support_handles;
  size_t channel_registrations = 0;
  size_t rpc_registrations = 0;
};

class DdsChannelBackend final : public runtime::core::channel::ChannelBackendBase {
 public:
  explicit DdsChannelBackend(std::shared_ptr<DdsBackendState> state);
  ~DdsChannelBackend() override;

  std::string_view Name() const noexcept override { return "dds"; }
  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;
  std::list<std::pair<std::string, std::string>> GenInitializationReport() const noexcept override;
  bool RegisterPublishType(const runtime::core::channel::PublishTypeWrapper& wrapper) noexcept override;
  bool Subscribe(const runtime::core::channel::SubscribeWrapper& wrapper) noexcept override;
  void Publish(runtime::core::channel::MsgWrapper&) noexcept override;
  aimrt_channel_loan_status_t PrepareLoanedPublisher(
      const runtime::core::channel::PublishTypeWrapper& wrapper,
      runtime::core::channel::BackendLoanedPublisher& loaned_publisher) noexcept override;
  aimrt_channel_loan_status_t SubscribeLoaned(
      const runtime::core::channel::LoanedSubscribeWrapper& wrapper) noexcept override;
#if defined(BUILD_TESTING)
  void SetWriteResultForTesting(eprosima::fastdds::dds::ReturnCode_t result) noexcept {
    forced_write_result_ = static_cast<int32_t>(result);
  }
  void SetBeforeWriteHookForTesting(std::function<void()> hook);
  void SetBeforeLoanSampleHookForTesting(std::function<void()> hook);
  void SetBeforeLoanedWriteHookForTesting(std::function<void()> hook);
  void SetBeforeDiscardLoanHookForTesting(std::function<void()> hook);
  void SetLoanSampleResultForTesting(eprosima::fastdds::dds::ReturnCode_t result) noexcept {
    forced_loan_sample_result_ = static_cast<int32_t>(result);
  }
  void SetDiscardLoanResultForTesting(eprosima::fastdds::dds::ReturnCode_t result) noexcept {
    forced_discard_loan_result_ = static_cast<int32_t>(result);
  }
  void SetReaderTakeResultForTesting(eprosima::fastdds::dds::ReturnCode_t result) noexcept {
    forced_reader_take_result_ = static_cast<int32_t>(result);
  }
  uint64_t LoanSampleCallCountForTesting() const noexcept {
    return loan_sample_calls_.load();
  }
  uint64_t DiscardLoanCallCountForTesting() const noexcept {
    return discard_loan_calls_.load();
  }
  uint64_t LoanReturnCountForTesting(std::string_view topic,
                                     std::string_view msg_type) const noexcept;
  uint64_t ReaderRetryScheduleCountForTesting(
      std::string_view topic, std::string_view msg_type) const noexcept;
  uint32_t ReaderLastRetryDelayMsForTesting(
      std::string_view topic, std::string_view msg_type) const noexcept;
  size_t ReaderRetryStepForTesting(
      std::string_view topic, std::string_view msg_type) const noexcept;
  int64_t ReaderUnreadCountForTesting(
      std::string_view topic, std::string_view msg_type) const noexcept;
#endif

 private:
  struct WriterLoanCapability {
    bool can_loan = false;
    std::string representation;
    std::string reason;
  };

  static std::string EndpointKey(std::string_view topic, std::string_view msg_type);
  static aimrt_channel_loan_status_t BorrowLoanedMessage(
      void* impl, aimrt_channel_loaned_message_base_t& output) noexcept;
  static aimrt_channel_loan_status_t PublishLoanedMessage(
      void* impl, aimrt::channel::ContextRef ctx_ref,
      aimrt_channel_loaned_message_base_t& loaned_msg) noexcept;
  static aimrt_channel_loan_status_t ReleaseLoanedMessage(
      void* impl, void* msg_ptr) noexcept;

  std::shared_ptr<DdsBackendState> state_;
  std::shared_ptr<std::atomic_bool> running_ = std::make_shared<std::atomic_bool>(false);
  mutable std::mutex mutex_;
  std::unordered_map<std::string, eprosima::fastdds::dds::DataWriter*> writers_;
  std::map<std::string, WriterLoanCapability, std::less<>> writer_loan_capabilities_;
  std::unordered_map<std::string, DdsLoanedPublisherState*> loan_routes_;
  std::vector<std::unique_ptr<DdsLoanedPublisherState>> loan_route_storage_;
  std::unordered_map<std::string, std::shared_ptr<DdsChannelSubscriptionState>> subscriptions_;
#if defined(BUILD_TESTING)
  std::atomic_int32_t forced_write_result_{
      static_cast<int32_t>(eprosima::fastdds::dds::RETCODE_OK)};
  std::mutex write_hook_mutex_;
  std::function<void()> before_write_hook_;
  std::mutex loan_hook_mutex_;
  std::function<void()> before_loan_sample_hook_;
  std::function<void()> before_loaned_write_hook_;
  std::function<void()> before_discard_loan_hook_;
  std::atomic_int32_t forced_loan_sample_result_{
      static_cast<int32_t>(eprosima::fastdds::dds::RETCODE_OK)};
  std::atomic_int32_t forced_discard_loan_result_{
      static_cast<int32_t>(eprosima::fastdds::dds::RETCODE_OK)};
  std::atomic_int32_t forced_reader_take_result_{
      static_cast<int32_t>(eprosima::fastdds::dds::RETCODE_OK)};
  std::atomic_uint64_t loan_sample_calls_ = 0;
  std::atomic_uint64_t discard_loan_calls_ = 0;
#endif
};

class DdsRpcBackend final : public runtime::core::rpc::RpcBackendBase {
 public:
  explicit DdsRpcBackend(std::shared_ptr<DdsBackendState> state);

  std::string_view Name() const noexcept override { return "dds"; }
  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;
  std::list<std::pair<std::string, std::string>> GenInitializationReport() const noexcept override;
  bool RegisterServiceFunc(const runtime::core::rpc::ServiceFuncWrapper& wrapper) noexcept override;
  bool RegisterClientFunc(const runtime::core::rpc::ClientFuncWrapper& wrapper) noexcept override;
  void Invoke(const std::shared_ptr<runtime::core::rpc::InvokeWrapper>& wrapper) noexcept override;
#if defined(BUILD_TESTING)
  eprosima::fastdds::rtps::GUID_t ResponseReaderGuidForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  eprosima::fastdds::rtps::GUID_t RequestWriterGuidForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  int64_t RequestWriterMaxBlockingTimeUsForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  size_t PendingCountForTesting(const runtime::core::rpc::FuncInfo& info) const;
  size_t TerminalHistoryCountForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  std::vector<eprosima::fastdds::rtps::SampleIdentity> PendingTokensForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  eprosima::fastdds::rtps::SampleIdentity FillTerminalHistoryForTesting(
      const runtime::core::rpc::FuncInfo& info, size_t count);
  void ForceNextResponseTakeResultForTesting(
      const runtime::core::rpc::FuncInfo& info, int32_t result);
  void ForceNextRequestTakeResultForTesting(
      const runtime::core::rpc::FuncInfo& info, int32_t result);
  DdsDiagnosticsSnapshot DiagnosticsForTesting() const noexcept {
    return state_->runtime.Diagnostics()->Snapshot();
  }
  std::string_view ReadinessForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  uint64_t InvalidRequestCountForTesting() const noexcept;
  static std::string_view ClassifyReadinessForTesting(
      uint32_t response_writer_matches, uint32_t request_reader_matches) noexcept;
  static bool FillCorrelationTokenForTesting(
      const eprosima::fastdds::rtps::SampleIdentity& request_identity,
      const eprosima::fastdds::rtps::SampleIdentity& related_identity,
      eprosima::fastdds::rtps::SampleIdentity& correlation_token) noexcept;
  void FailNextScheduleForTesting() noexcept { fail_next_schedule_ = true; }
  void FailNextRequestWriteForTesting() noexcept { fail_next_request_write_ = true; }
  void FailNextRequestSerializationForTesting() noexcept {
    fail_next_request_serialization_ = true;
  }
  void FailNextResponseSerializationForTesting() noexcept;
  void SetBeforeRequestWriteHookForTesting(std::function<void()> hook);
  void SetAfterRequestWriteHookForTesting(std::function<void()> hook);
  void SetBeforeResponseWriteHookForTesting(std::function<void()> hook);
  void SetBeforeTimeoutCompletionHookForTesting(std::function<void()> hook);
  void SetAfterShutdownStartedHookForTesting(std::function<void()> hook);
  bool RunningForTesting() const noexcept { return running_token_->load(); }
  uint64_t ResponseWriteCallCountForTesting() const noexcept;
  uint64_t ResponseRetryScheduleCountForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  uint64_t ResponseGateBusyRetryCountForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  uint32_t ResponseLastRetryDelayUsForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  std::shared_ptr<DdsReaderDrainState> ClientDrainForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  std::shared_ptr<DdsReaderDrainState> ServerDrainForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  int64_t ResponseUnreadCountForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  int64_t RequestUnreadCountForTesting(
      const runtime::core::rpc::FuncInfo& info) const;
  void DisconnectRequestWriterForTesting(
      const runtime::core::rpc::FuncInfo& info);
  void DisconnectServerForTesting(
      const runtime::core::rpc::FuncInfo& info);
  uint64_t ServerDrainsInFlightForTesting() const noexcept;
  void SetRequesterMatchOverrideForTesting(std::function<int()> hook);
  void SetRequestIdentityOverrideForTesting(
      std::function<void(eprosima::fastdds::rtps::SampleIdentity&,
                         const eprosima::fastdds::rtps::SampleIdentity&)>
          hook);
#endif

 private:
  struct ClientState;
  struct ServerState;
  struct CallbackState;
  using SampleIdentity = eprosima::fastdds::rtps::SampleIdentity;
  enum class TerminalReason : uint8_t { kResponseWon,
                                        kTimeout,
                                        kShutdown };

  bool RegisterFunction(const runtime::core::rpc::FuncInfo& info) noexcept;
  void RegisterClientEndpoint(const std::shared_ptr<ClientState>& state);
  void RegisterServerEndpoint(const std::shared_ptr<ServerState>& state);
  static DdsReaderDrainState::DrainResult DrainClient(
      const std::shared_ptr<CallbackState>& callback_state,
      const std::shared_ptr<ClientState>& state);
  static DdsReaderDrainState::DrainResult DrainServer(
      const std::shared_ptr<CallbackState>& callback_state,
      const std::shared_ptr<ServerState>& state);
  static bool CompleteClient(const std::shared_ptr<ClientState>& state,
                             const SampleIdentity& id, TerminalReason reason,
                             aimrt::rpc::Status status, bool copy_response,
                             void* response);
  static void AddTerminalLocked(ClientState& state, const std::string& key,
                                const SampleIdentity& request_identity,
                                TerminalReason reason);
  static void ObserveMatchedServers(const std::shared_ptr<ClientState>& state,
                                    int32_t matched_servers);

  std::shared_ptr<DdsBackendState> state_;
  std::atomic_bool running_ = false;
  std::shared_ptr<std::atomic_bool> running_token_ = std::make_shared<std::atomic_bool>(false);
  std::shared_ptr<CallbackState> callback_state_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<ClientState>> clients_;
  std::unordered_map<std::string, std::shared_ptr<ServerState>> servers_;
  std::unordered_set<std::string> registered_functions_;
#if defined(BUILD_TESTING)
  std::atomic_bool fail_next_schedule_ = false;
  std::atomic_bool fail_next_request_write_ = false;
  std::atomic_bool fail_next_request_serialization_ = false;
  std::mutex request_write_hook_mutex_;
  std::function<void()> before_request_write_hook_;
  std::function<void()> after_request_write_hook_;
#endif
};

}  // namespace aimrt::plugins::dds_plugin
