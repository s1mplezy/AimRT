// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <asio.hpp>
#include <fastdds/dds/core/status/BaseStatus.hpp>
#include <fastdds/dds/core/status/DeadlineMissedStatus.hpp>
#include <fastdds/dds/core/status/IncompatibleQosStatus.hpp>
#include <fastdds/dds/core/status/LivelinessChangedStatus.hpp>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/core/status/SampleRejectedStatus.hpp>
#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/topic/ContentFilteredTopic.hpp>
#include <fastdds/dds/topic/IContentFilterFactory.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "dds_plugin/dds_config.h"

namespace aimrt::plugins::dds_plugin {

enum class DdsRuntimeState : uint8_t { kRunning,
                                       kStopping,
                                       kStopped };

struct DdsDiagnosticsSnapshot {
  uint64_t reader_match_events = 0;
  uint64_t writer_match_events = 0;
  uint64_t requested_incompatible_qos_total = 0;
  uint64_t offered_incompatible_qos_total = 0;
  uint64_t reader_take_failure_total = 0;
  uint64_t channel_write_failure_total = 0;
  uint64_t rpc_invalid_request_total = 0;
  uint64_t rpc_invalid_response_total = 0;
  uint64_t rpc_foreign_response_total = 0;
  uint64_t rpc_late_response_total = 0;
  uint64_t rpc_duplicate_response_total = 0;
  uint64_t rpc_unknown_correlation_total = 0;
  uint64_t rpc_pending_rejected_total = 0;
  uint64_t rpc_terminal_history_evicted_total = 0;
  uint64_t rpc_server_non_ok_completion_total = 0;
  uint64_t rpc_late_server_completion_total = 0;
  uint64_t rpc_requester_unmatched_on_reply_total = 0;
  uint64_t rpc_reply_match_timeout_total = 0;
  uint64_t rpc_multiple_servers_total = 0;
  uint64_t sample_lost_total = 0;
  uint64_t sample_rejected_total = 0;
  uint64_t requested_deadline_missed_total = 0;
  uint64_t offered_deadline_missed_total = 0;
  uint64_t reader_liveliness_change_events = 0;
  uint64_t writer_liveliness_lost_total = 0;
  int32_t last_sample_rejected_reason = 0;
  int32_t sample_lost_last_change = 0;
  int32_t sample_rejected_last_change = 0;
  int32_t requested_deadline_last_change = 0;
  int32_t offered_deadline_last_change = 0;
  int32_t reader_alive_count = 0;
  int32_t reader_not_alive_count = 0;
  int32_t reader_alive_last_change = 0;
  int32_t reader_not_alive_last_change = 0;
  int32_t writer_liveliness_lost_last_change = 0;
  int32_t reader_current_matches = 0;
  int32_t writer_current_matches = 0;
  int32_t rpc_matched_servers = 0;
  int32_t last_incompatible_qos_policy = 0;
};

class DdsDiagnostics {
 public:
  DdsDiagnosticsSnapshot Snapshot() const noexcept;
  uint64_t RecordReaderTakeFailure() noexcept;
  uint64_t RecordChannelWriteFailure() noexcept;
  uint64_t RecordRpcInvalidRequest() noexcept;
  uint64_t RecordRpcInvalidResponse() noexcept;
  uint64_t RecordRpcForeignResponse() noexcept;
  uint64_t RecordRpcLateResponse() noexcept;
  uint64_t RecordRpcDuplicateResponse() noexcept;
  uint64_t RecordRpcUnknownCorrelation() noexcept;
  uint64_t RecordRpcPendingRejected() noexcept;
  uint64_t RecordRpcTerminalHistoryEvicted() noexcept;
  uint64_t RecordRpcServerNonOkCompletion() noexcept;
  uint64_t RecordRpcLateServerCompletion() noexcept;
  uint64_t RecordRpcRequesterUnmatchedOnReply() noexcept;
  uint64_t RecordRpcReplyMatchTimeout() noexcept;
  uint64_t RecordRpcMultipleServers() noexcept;
  void AdjustRpcMatchedServers(int32_t change) noexcept {
    rpc_matched_servers_.fetch_add(change);
  }

 private:
  friend class DdsDataReaderListener;
  friend class DdsDataWriterListener;

  std::atomic_uint64_t reader_match_events_{0};
  std::atomic_uint64_t writer_match_events_{0};
  std::atomic_uint64_t requested_incompatible_qos_total_{0};
  std::atomic_uint64_t offered_incompatible_qos_total_{0};
  std::atomic_uint64_t reader_take_failure_total_{0};
  std::atomic_uint64_t channel_write_failure_total_{0};
  std::atomic_uint64_t rpc_invalid_request_total_{0};
  std::atomic_uint64_t rpc_invalid_response_total_{0};
  std::atomic_uint64_t rpc_foreign_response_total_{0};
  std::atomic_uint64_t rpc_late_response_total_{0};
  std::atomic_uint64_t rpc_duplicate_response_total_{0};
  std::atomic_uint64_t rpc_unknown_correlation_total_{0};
  std::atomic_uint64_t rpc_pending_rejected_total_{0};
  std::atomic_uint64_t rpc_terminal_history_evicted_total_{0};
  std::atomic_uint64_t rpc_server_non_ok_completion_total_{0};
  std::atomic_uint64_t rpc_late_server_completion_total_{0};
  std::atomic_uint64_t rpc_requester_unmatched_on_reply_total_{0};
  std::atomic_uint64_t rpc_reply_match_timeout_total_{0};
  std::atomic_uint64_t rpc_multiple_servers_total_{0};
  std::atomic_uint64_t sample_lost_total_{0};
  std::atomic_uint64_t sample_rejected_total_{0};
  std::atomic_uint64_t requested_deadline_missed_total_{0};
  std::atomic_uint64_t offered_deadline_missed_total_{0};
  std::atomic_uint64_t reader_liveliness_change_events_{0};
  std::atomic_uint64_t writer_liveliness_lost_total_{0};
  std::atomic_int32_t last_sample_rejected_reason_{0};
  std::atomic_int32_t sample_lost_last_change_{0};
  std::atomic_int32_t sample_rejected_last_change_{0};
  std::atomic_int32_t requested_deadline_last_change_{0};
  std::atomic_int32_t offered_deadline_last_change_{0};
  std::atomic_int32_t reader_alive_count_{0};
  std::atomic_int32_t reader_not_alive_count_{0};
  std::atomic_int32_t reader_alive_last_change_{0};
  std::atomic_int32_t reader_not_alive_last_change_{0};
  std::atomic_int32_t writer_liveliness_lost_last_change_{0};
  std::atomic_int32_t reader_current_matches_{0};
  std::atomic_int32_t writer_current_matches_{0};
  std::atomic_int32_t rpc_matched_servers_{0};
  std::atomic_int32_t last_incompatible_qos_policy_{0};
};

class DdsAsioExecutor : public std::enable_shared_from_this<DdsAsioExecutor> {
 public:
  explicit DdsAsioExecutor(uint32_t thread_num);
  ~DdsAsioExecutor();
  DdsAsioExecutor(const DdsAsioExecutor&) = delete;
  DdsAsioExecutor& operator=(const DdsAsioExecutor&) = delete;

  void Start();
  void Shutdown();
  bool Post(std::function<void()> task);
  bool ScheduleAfter(std::chrono::steady_clock::duration delay, std::function<void()> task);

  DdsRuntimeState State() const noexcept { return state_.load(); }
  uint32_t ThreadNum() const noexcept { return thread_num_; }
  bool IsWorkerThread() const;

 private:
  uint32_t thread_num_;
  std::atomic<DdsRuntimeState> state_{DdsRuntimeState::kStopped};
  mutable std::mutex mutex_;
  asio::io_context io_context_;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
  std::vector<std::thread> threads_;
  std::vector<std::weak_ptr<asio::steady_timer>> timers_;
  bool started_once_ = false;
};

class DdsReaderDrainState : public std::enable_shared_from_this<DdsReaderDrainState> {
 public:
  enum class DrainResult { kConsumed,
                           kNoData,
                           kRetryLater,
                           kRetryTemporaryLoanFailure,
                           kRetryRpcPublicationGateBusy };
  using DrainOne = std::function<DrainResult()>;
  using HasUnread = std::function<bool()>;

  DdsReaderDrainState(std::weak_ptr<DdsAsioExecutor> executor, DrainOne drain_one,
                      HasUnread has_unread);

  void Start();
  void Stop() noexcept;
  void NotifyData() noexcept;
  bool DrainScheduled() const noexcept { return drain_scheduled_.load(); }
  bool AcceptingWork() const noexcept { return accepting_work_.load(); }
#if defined(BUILD_TESTING)
  uint64_t DrainExecutionCount() const noexcept { return drain_execution_count_.load(); }
  uint64_t RetryScheduleCount() const noexcept { return retry_schedule_count_.load(); }
  uint32_t LastRetryDelayUs() const noexcept { return last_retry_delay_us_.load(); }
  uint32_t LastRetryDelayMs() const noexcept { return LastRetryDelayUs() / 1000; }
  size_t RetryStep() const noexcept { return retry_step_.load(); }
  uint64_t TemporaryLoanFailureRetryCount() const noexcept {
    return temporary_loan_failure_retry_count_.load();
  }
  uint64_t RpcPublicationGateBusyRetryCount() const noexcept {
    return rpc_publication_gate_busy_retry_count_.load();
  }
#endif

 private:
  bool ScheduleDrain() noexcept;
  bool ScheduleRetry(DrainResult reason) noexcept;
  void DrainOnce() noexcept;

  std::weak_ptr<DdsAsioExecutor> executor_;
  DrainOne drain_one_;
  HasUnread has_unread_;
  std::atomic_bool accepting_work_{false};
  std::atomic_bool drain_scheduled_{false};
  std::atomic_bool wake_pending_{false};
  std::atomic_size_t retry_step_{0};
#if defined(BUILD_TESTING)
  std::atomic_uint64_t drain_execution_count_{0};
  std::atomic_uint64_t retry_schedule_count_{0};
  std::atomic_uint32_t last_retry_delay_us_{0};
  std::atomic_uint64_t temporary_loan_failure_retry_count_{0};
  std::atomic_uint64_t rpc_publication_gate_busy_retry_count_{0};
#endif
};

struct DdsEndpointMetadata {
  std::string participant;
  std::string topic;
  std::string type;
};

#if defined(BUILD_TESTING)
struct DdsListenerLifetimeTracker {
  std::mutex mutex;
  std::vector<std::string> events;

  void Record(std::string event);
};
#endif

class DdsDataReaderListener final : public eprosima::fastdds::dds::DataReaderListener {
 public:
  DdsDataReaderListener(DdsEndpointMetadata metadata,
                        std::shared_ptr<DdsReaderDrainState> drain_state,
                        std::shared_ptr<DdsDiagnostics> diagnostics
#if defined(BUILD_TESTING)
                        ,
                        std::shared_ptr<DdsListenerLifetimeTracker> lifetime = {});
#else
  );
#endif
  ~DdsDataReaderListener() override;

  void on_data_available(eprosima::fastdds::dds::DataReader*) override;
  void on_subscription_matched(
      eprosima::fastdds::dds::DataReader* reader,
      const eprosima::fastdds::dds::SubscriptionMatchedStatus& status) override;
  void on_requested_incompatible_qos(
      eprosima::fastdds::dds::DataReader* reader,
      const eprosima::fastdds::dds::RequestedIncompatibleQosStatus& status) override;
  void on_requested_deadline_missed(
      eprosima::fastdds::dds::DataReader* reader,
      const eprosima::fastdds::dds::RequestedDeadlineMissedStatus& status) override;
  void on_liveliness_changed(
      eprosima::fastdds::dds::DataReader* reader,
      const eprosima::fastdds::dds::LivelinessChangedStatus& status) override;
  void on_sample_rejected(
      eprosima::fastdds::dds::DataReader* reader,
      const eprosima::fastdds::dds::SampleRejectedStatus& status) override;
  void on_sample_lost(
      eprosima::fastdds::dds::DataReader* reader,
      const eprosima::fastdds::dds::SampleLostStatus& status) override;

 private:
  DdsEndpointMetadata metadata_;
  std::shared_ptr<DdsReaderDrainState> drain_state_;
  std::shared_ptr<DdsDiagnostics> diagnostics_;
#if defined(BUILD_TESTING)
  std::shared_ptr<DdsListenerLifetimeTracker> lifetime_;
#endif
  std::atomic_int32_t current_matches_{0};
  std::atomic_uint64_t deadline_log_events_{0};
  std::atomic_uint64_t liveliness_log_events_{0};
  std::atomic_uint64_t sample_rejected_log_events_{0};
  std::atomic_uint64_t sample_lost_log_events_{0};
};

class DdsDataWriterListener final : public eprosima::fastdds::dds::DataWriterListener {
 public:
  DdsDataWriterListener(DdsEndpointMetadata metadata,
                        std::shared_ptr<DdsDiagnostics> diagnostics,
                        std::function<void(int32_t)> match_callback = {}
#if defined(BUILD_TESTING)
                        ,
                        std::shared_ptr<DdsListenerLifetimeTracker> lifetime = {});
#else
  );
#endif
  ~DdsDataWriterListener() override;

  void on_publication_matched(
      eprosima::fastdds::dds::DataWriter* writer,
      const eprosima::fastdds::dds::PublicationMatchedStatus& status) override;
  void on_offered_incompatible_qos(
      eprosima::fastdds::dds::DataWriter* writer,
      const eprosima::fastdds::dds::OfferedIncompatibleQosStatus& status) override;
  void on_offered_deadline_missed(
      eprosima::fastdds::dds::DataWriter* writer,
      const eprosima::fastdds::dds::OfferedDeadlineMissedStatus& status) override;
  void on_liveliness_lost(
      eprosima::fastdds::dds::DataWriter* writer,
      const eprosima::fastdds::dds::LivelinessLostStatus& status) override;

 private:
  DdsEndpointMetadata metadata_;
  std::shared_ptr<DdsDiagnostics> diagnostics_;
  std::function<void(int32_t)> match_callback_;
#if defined(BUILD_TESTING)
  std::shared_ptr<DdsListenerLifetimeTracker> lifetime_;
#endif
  std::atomic_int32_t current_matches_{0};
  std::atomic_uint64_t deadline_log_events_{0};
  std::atomic_uint64_t liveliness_log_events_{0};
};

class DdsEndpointManager {
 public:
  class WriterOperation {
   public:
    WriterOperation() = default;
    ~WriterOperation();
    WriterOperation(const WriterOperation&) = delete;
    WriterOperation& operator=(const WriterOperation&) = delete;
    WriterOperation(WriterOperation&& other) noexcept;
    WriterOperation& operator=(WriterOperation&& other) noexcept;

    explicit operator bool() const noexcept { return owner_ != nullptr; }
    bool Stopping() const noexcept;

   private:
    friend class DdsEndpointManager;
    explicit WriterOperation(DdsEndpointManager* owner) : owner_(owner) {}
    void Reset() noexcept;

    DdsEndpointManager* owner_ = nullptr;
  };

  struct ReaderEndpoint {
    eprosima::fastdds::dds::DataReader* reader = nullptr;
    std::shared_ptr<DdsReaderDrainState> drain_state;
    eprosima::fastdds::dds::ContentFilteredTopic* filtered_topic = nullptr;
  };

  DdsEndpointManager();
  ~DdsEndpointManager();
  DdsEndpointManager(const DdsEndpointManager&) = delete;
  DdsEndpointManager& operator=(const DdsEndpointManager&) = delete;

  void Initialize(eprosima::fastdds::dds::DomainParticipant* participant,
                  const DdsQosSnapshot& qos,
                  std::shared_ptr<DdsAsioExecutor> executor,
                  std::shared_ptr<DdsDiagnostics> diagnostics);
  eprosima::fastdds::dds::DataWriter* GetOrCreateWriter(
      std::string_view topic, const eprosima::fastdds::dds::TypeSupport& type_support,
      const eprosima::fastdds::dds::DataWriterQos& writer_qos,
      std::string_view registration_origin,
      std::function<void(int32_t)> match_callback = {});
  ReaderEndpoint GetOrCreateReader(
      std::string_view topic, const eprosima::fastdds::dds::TypeSupport& type_support,
      const eprosima::fastdds::dds::DataReaderQos& reader_qos,
      std::string_view registration_origin,
      DdsReaderDrainState::DrainOne drain_one = {},
      DdsReaderDrainState::HasUnread has_unread = {});
  ReaderEndpoint GetOrCreateFilteredReader(
      std::string_view topic, const eprosima::fastdds::dds::TypeSupport& type_support,
      const eprosima::fastdds::dds::DataReaderQos& reader_qos,
      std::string_view registration_origin, std::string filter_name,
      std::string filter_expression, std::vector<std::string> filter_parameters,
      DdsReaderDrainState::DrainOne drain_one,
      DdsReaderDrainState::HasUnread has_unread);
  WriterOperation AcquireWriterOperation() noexcept;
  bool EnableWriter(eprosima::fastdds::dds::DataWriter* writer) noexcept;
  bool EnableReader(eprosima::fastdds::dds::DataReader* reader) noexcept;
  void DeleteWriter(eprosima::fastdds::dds::DataWriter* writer);
  void DeleteReader(eprosima::fastdds::dds::DataReader* reader);

  void Start();
  void BeginStopping();
  void DeleteEntities();
  size_t WriterCount() const;
  size_t ReaderCount() const;
  size_t TopicCount() const;
  size_t FilteredTopicCount() const;
#if defined(BUILD_TESTING)
  void SetListenerLifetimeTracker(std::shared_ptr<DdsListenerLifetimeTracker> tracker);
  void FailNextFilteredReaderCreationForTesting() noexcept {
    fail_next_filtered_reader_creation_ = true;
  }
  void FailNextEndpointEnableForTesting() noexcept {
    fail_next_endpoint_enable_ = true;
  }
#endif

 private:
  struct TopicEntry;
  struct WriterEntry;
  struct ReaderEntry;

  TopicEntry& GetOrCreateTopicLocked(
      std::string_view topic, const eprosima::fastdds::dds::TypeSupport& type_support);
  void RollbackTopicIfUnusedLocked(std::string_view topic);
  void ReleaseWriterOperation() noexcept;
  bool WriterOperationStopping() const noexcept;

  mutable std::mutex mutex_;
  std::condition_variable writer_operations_drained_;
  eprosima::fastdds::dds::DomainParticipant* participant_ = nullptr;
  eprosima::fastdds::dds::Publisher* publisher_ = nullptr;
  eprosima::fastdds::dds::Subscriber* subscriber_ = nullptr;
  std::shared_ptr<DdsAsioExecutor> executor_;
  std::shared_ptr<DdsDiagnostics> diagnostics_;
#if defined(BUILD_TESTING)
  std::shared_ptr<DdsListenerLifetimeTracker> lifetime_tracker_;
  std::atomic_bool fail_next_filtered_reader_creation_ = false;
  std::atomic_bool fail_next_endpoint_enable_ = false;
#endif
  eprosima::fastdds::dds::TopicQos topic_qos_;
  std::unordered_map<std::string, std::unique_ptr<TopicEntry>> topics_;
  std::unordered_map<std::string, std::unique_ptr<WriterEntry>> writers_;
  std::unordered_map<std::string, std::unique_ptr<ReaderEntry>> readers_;
  std::unique_ptr<eprosima::fastdds::dds::IContentFilterFactory> rpc_filter_factory_;
  bool running_ = false;
  bool stopping_ = false;
  size_t writer_operations_in_flight_ = 0;
};

class DdsRuntime {
 public:
  DdsRuntime();
  ~DdsRuntime();
  DdsRuntime(const DdsRuntime&) = delete;
  DdsRuntime& operator=(const DdsRuntime&) = delete;

  void Initialize(DdsParticipantContext& participant_context);
  void Start();
  void Shutdown();

  DdsRuntimeState State() const noexcept { return state_.load(); }
  uint32_t ThreadNum() const noexcept;
  std::shared_ptr<DdsAsioExecutor> Executor() const { return executor_; }
  std::shared_ptr<DdsDiagnostics> Diagnostics() const { return diagnostics_; }
  DdsEndpointManager& Endpoints() { return endpoints_; }
  const DdsEndpointManager& Endpoints() const { return endpoints_; }

 private:
  mutable std::mutex mutex_;
  std::atomic<DdsRuntimeState> state_{DdsRuntimeState::kStopped};
  bool initialized_ = false;
  std::shared_ptr<DdsAsioExecutor> executor_;
  std::shared_ptr<DdsDiagnostics> diagnostics_;
  DdsEndpointManager endpoints_;
};

}  // namespace aimrt::plugins::dds_plugin
