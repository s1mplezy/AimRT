// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "dds_plugin/dds_runtime.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/IContentFilterFactory.hpp>
#include <fastdds/dds/topic/Topic.hpp>

#include "dds_plugin/global.h"
#include "util/log_util.h"

namespace aimrt::plugins::dds_plugin {
namespace {

using namespace eprosima::fastdds::dds;

class AimrtRpcContentFilter final : public IContentFilter {
 public:
  explicit AimrtRpcContentFilter(std::string prefix) : prefix_(std::move(prefix)) {}
  bool evaluate(const SerializedPayload&, const FilterSampleInfo& info,
                const GUID_t&) const override {
    std::ostringstream out;
    out << info.related_sample_identity.writer_guid().guidPrefix;
    return out.str() == prefix_;
  }

 private:
  std::string prefix_;
};

class AimrtRpcContentFilterFactory final : public IContentFilterFactory {
 public:
  ReturnCode_t create_content_filter(const char*, const char*, const TopicDataType*,
                                     const char*, const ParameterSeq& params, IContentFilter*& instance) override {
    if (params.length() == 0 || params[0] == nullptr) return RETCODE_BAD_PARAMETER;
    auto* next = new AimrtRpcContentFilter(params[0]);
    delete instance;
    instance = next;
    return RETCODE_OK;
  }
  ReturnCode_t delete_content_filter(const char*, IContentFilter* instance) override {
    delete instance;
    return RETCODE_OK;
  }
};

std::string HandleToString(const InstanceHandle_t& handle) {
  std::ostringstream output;
  output << handle;
  return output.str();
}

std::string GuidToString(const eprosima::fastdds::rtps::GUID_t& guid) {
  std::ostringstream output;
  output << guid;
  return output.str();
}

bool ShouldLogRateLimited(uint64_t count) {
  return count != 0 && (count & (count - 1)) == 0;
}

bool ShouldLogCallbackEvent(std::atomic_uint64_t& events) {
  return ShouldLogRateLimited(events.fetch_add(1) + 1);
}

std::string_view SampleRejectedReason(SampleRejectedStatusKind reason) {
  switch (reason) {
    case NOT_REJECTED:
      return "not_rejected";
    case REJECTED_BY_INSTANCES_LIMIT:
      return "instances_limit";
    case REJECTED_BY_SAMPLES_LIMIT:
      return "samples_limit";
    case REJECTED_BY_SAMPLES_PER_INSTANCE_LIMIT:
      return "samples_per_instance_limit";
    case REJECTED_BY_UNKNOWN_INSTANCE:
      return "unknown_instance";
  }
  return "unknown";
}

void RequireOk(ReturnCode_t result, std::string_view operation) {
  if (result == RETCODE_OK) return;
  throw std::runtime_error(std::string(operation) + " failed with Fast DDS return code " +
                           std::to_string(result));
}

std::string EndpointKey(std::string_view topic, std::string_view type) {
  return std::string(topic) + "\n" + std::string(type);
}

std::string WriterKey(std::string_view topic, std::string_view type,
                      std::string_view registration_origin) {
  auto key = EndpointKey(topic, type);
  if (registration_origin.starts_with("rpc ")) {
    key += "\n";
    key += registration_origin;
  }
  return key;
}

std::string ReaderKey(std::string_view topic, std::string_view type,
                      std::string_view registration_origin) {
  auto key = EndpointKey(topic, type);
  if (registration_origin.starts_with("rpc ")) {
    key += "\n";
    key += registration_origin;
  }
  return key;
}

std::string ReliabilityName(ReliabilityQosPolicyKind kind) {
  return kind == RELIABLE_RELIABILITY_QOS ? "reliable" : "best_effort";
}

std::string DurabilityName(DurabilityQosPolicyKind kind) {
  switch (kind) {
    case VOLATILE_DURABILITY_QOS:
      return "volatile";
    case TRANSIENT_LOCAL_DURABILITY_QOS:
      return "transient_local";
    case TRANSIENT_DURABILITY_QOS:
      return "transient";
    case PERSISTENT_DURABILITY_QOS:
      return "persistent";
  }
  return "unknown";
}

std::string HistoryName(const HistoryQosPolicy& history) {
  if (history.kind == KEEP_ALL_HISTORY_QOS) return "keep_all";
  return "keep_last(depth=" + std::to_string(history.depth) + ")";
}

template <typename EndpointQos>
std::string EffectiveQosSummary(const EndpointQos& qos) {
  return "{reliability=" + ReliabilityName(qos.reliability().kind) +
         ",durability=" + DurabilityName(qos.durability().kind) +
         ",history=" + HistoryName(qos.history()) + "}";
}

template <typename Policy>
void AddChangedPolicy(std::vector<std::string_view>& changed, std::string_view name,
                      const Policy& existing, const Policy& requested) {
  if (!(existing == requested)) changed.emplace_back(name);
}

std::string JoinChangedPolicies(const std::vector<std::string_view>& changed) {
  std::string result;
  for (const auto policy : changed) {
    if (!result.empty()) result += ',';
    result += policy;
  }
  return result;
}

std::string ChangedWriterQosPolicies(const DataWriterQos& existing,
                                     const DataWriterQos& requested) {
  std::vector<std::string_view> changed;
  AddChangedPolicy(changed, "durability", existing.durability(), requested.durability());
  AddChangedPolicy(changed, "durability_service", existing.durability_service(),
                   requested.durability_service());
  AddChangedPolicy(changed, "deadline", existing.deadline(), requested.deadline());
  AddChangedPolicy(changed, "latency_budget", existing.latency_budget(),
                   requested.latency_budget());
  AddChangedPolicy(changed, "liveliness", existing.liveliness(), requested.liveliness());
  AddChangedPolicy(changed, "reliability", existing.reliability(), requested.reliability());
  AddChangedPolicy(changed, "destination_order", existing.destination_order(),
                   requested.destination_order());
  AddChangedPolicy(changed, "history", existing.history(), requested.history());
  AddChangedPolicy(changed, "resource_limits", existing.resource_limits(),
                   requested.resource_limits());
  AddChangedPolicy(changed, "transport_priority", existing.transport_priority(),
                   requested.transport_priority());
  AddChangedPolicy(changed, "lifespan", existing.lifespan(), requested.lifespan());
  AddChangedPolicy(changed, "user_data", existing.user_data(), requested.user_data());
  AddChangedPolicy(changed, "ownership", existing.ownership(), requested.ownership());
  AddChangedPolicy(changed, "ownership_strength", existing.ownership_strength(),
                   requested.ownership_strength());
  AddChangedPolicy(changed, "writer_data_lifecycle", existing.writer_data_lifecycle(),
                   requested.writer_data_lifecycle());
  AddChangedPolicy(changed, "publish_mode", existing.publish_mode(), requested.publish_mode());
  AddChangedPolicy(changed, "representation", existing.representation(),
                   requested.representation());
  AddChangedPolicy(changed, "properties", existing.properties(), requested.properties());
  AddChangedPolicy(changed, "reliable_writer_qos", existing.reliable_writer_qos(),
                   requested.reliable_writer_qos());
  AddChangedPolicy(changed, "endpoint", existing.endpoint(), requested.endpoint());
  AddChangedPolicy(changed, "writer_resource_limits", existing.writer_resource_limits(),
                   requested.writer_resource_limits());
  AddChangedPolicy(changed, "data_sharing", existing.data_sharing(), requested.data_sharing());
  return JoinChangedPolicies(changed);
}

std::string ChangedReaderQosPolicies(const DataReaderQos& existing,
                                     const DataReaderQos& requested) {
  std::vector<std::string_view> changed;
  AddChangedPolicy(changed, "durability", existing.durability(), requested.durability());
  AddChangedPolicy(changed, "deadline", existing.deadline(), requested.deadline());
  AddChangedPolicy(changed, "latency_budget", existing.latency_budget(),
                   requested.latency_budget());
  AddChangedPolicy(changed, "liveliness", existing.liveliness(), requested.liveliness());
  AddChangedPolicy(changed, "reliability", existing.reliability(), requested.reliability());
  AddChangedPolicy(changed, "destination_order", existing.destination_order(),
                   requested.destination_order());
  AddChangedPolicy(changed, "history", existing.history(), requested.history());
  AddChangedPolicy(changed, "resource_limits", existing.resource_limits(),
                   requested.resource_limits());
  AddChangedPolicy(changed, "user_data", existing.user_data(), requested.user_data());
  AddChangedPolicy(changed, "ownership", existing.ownership(), requested.ownership());
  AddChangedPolicy(changed, "time_based_filter", existing.time_based_filter(),
                   requested.time_based_filter());
  AddChangedPolicy(changed, "reader_data_lifecycle", existing.reader_data_lifecycle(),
                   requested.reader_data_lifecycle());
  AddChangedPolicy(changed, "lifespan", existing.lifespan(), requested.lifespan());
  AddChangedPolicy(changed, "durability_service", existing.durability_service(),
                   requested.durability_service());
  AddChangedPolicy(changed, "reliable_reader_qos", existing.reliable_reader_qos(),
                   requested.reliable_reader_qos());
  AddChangedPolicy(changed, "type_consistency", existing.type_consistency(),
                   requested.type_consistency());
  AddChangedPolicy(changed, "representation", existing.representation(),
                   requested.representation());
  AddChangedPolicy(changed, "expects_inline_qos", existing.expects_inline_qos(),
                   requested.expects_inline_qos());
  AddChangedPolicy(changed, "properties", existing.properties(), requested.properties());
  AddChangedPolicy(changed, "endpoint", existing.endpoint(), requested.endpoint());
  AddChangedPolicy(changed, "reader_resource_limits", existing.reader_resource_limits(),
                   requested.reader_resource_limits());
  AddChangedPolicy(changed, "data_sharing", existing.data_sharing(), requested.data_sharing());
  return JoinChangedPolicies(changed);
}

std::string RequireRegistrationOrigin(std::string_view registration_origin) {
  if (registration_origin.empty()) {
    throw std::invalid_argument("DDS endpoint registration origin must not be empty");
  }
  return std::string(registration_origin);
}

}  // namespace

DdsDiagnosticsSnapshot DdsDiagnostics::Snapshot() const noexcept {
  return DdsDiagnosticsSnapshot{
      .reader_match_events = reader_match_events_.load(),
      .writer_match_events = writer_match_events_.load(),
      .requested_incompatible_qos_total = requested_incompatible_qos_total_.load(),
      .offered_incompatible_qos_total = offered_incompatible_qos_total_.load(),
      .reader_take_failure_total = reader_take_failure_total_.load(),
      .channel_write_failure_total = channel_write_failure_total_.load(),
      .rpc_invalid_request_total = rpc_invalid_request_total_.load(),
      .rpc_invalid_response_total = rpc_invalid_response_total_.load(),
      .rpc_foreign_response_total = rpc_foreign_response_total_.load(),
      .rpc_late_response_total = rpc_late_response_total_.load(),
      .rpc_duplicate_response_total = rpc_duplicate_response_total_.load(),
      .rpc_unknown_correlation_total = rpc_unknown_correlation_total_.load(),
      .rpc_pending_rejected_total = rpc_pending_rejected_total_.load(),
      .rpc_terminal_history_evicted_total = rpc_terminal_history_evicted_total_.load(),
      .rpc_server_non_ok_completion_total = rpc_server_non_ok_completion_total_.load(),
      .rpc_late_server_completion_total = rpc_late_server_completion_total_.load(),
      .rpc_requester_unmatched_on_reply_total = rpc_requester_unmatched_on_reply_total_.load(),
      .rpc_reply_match_timeout_total = rpc_reply_match_timeout_total_.load(),
      .rpc_multiple_servers_total = rpc_multiple_servers_total_.load(),
      .sample_lost_total = sample_lost_total_.load(),
      .sample_rejected_total = sample_rejected_total_.load(),
      .requested_deadline_missed_total = requested_deadline_missed_total_.load(),
      .offered_deadline_missed_total = offered_deadline_missed_total_.load(),
      .reader_liveliness_change_events = reader_liveliness_change_events_.load(),
      .writer_liveliness_lost_total = writer_liveliness_lost_total_.load(),
      .last_sample_rejected_reason = last_sample_rejected_reason_.load(),
      .sample_lost_last_change = sample_lost_last_change_.load(),
      .sample_rejected_last_change = sample_rejected_last_change_.load(),
      .requested_deadline_last_change = requested_deadline_last_change_.load(),
      .offered_deadline_last_change = offered_deadline_last_change_.load(),
      .reader_alive_count = reader_alive_count_.load(),
      .reader_not_alive_count = reader_not_alive_count_.load(),
      .reader_alive_last_change = reader_alive_last_change_.load(),
      .reader_not_alive_last_change = reader_not_alive_last_change_.load(),
      .writer_liveliness_lost_last_change = writer_liveliness_lost_last_change_.load(),
      .reader_current_matches = reader_current_matches_.load(),
      .writer_current_matches = writer_current_matches_.load(),
      .rpc_matched_servers = rpc_matched_servers_.load(),
      .last_incompatible_qos_policy = last_incompatible_qos_policy_.load()};
}

uint64_t DdsDiagnostics::RecordReaderTakeFailure() noexcept {
  return reader_take_failure_total_.fetch_add(1) + 1;
}

uint64_t DdsDiagnostics::RecordChannelWriteFailure() noexcept {
  return channel_write_failure_total_.fetch_add(1) + 1;
}

#define AIMRT_DDS_DEFINE_RPC_COUNTER(Method, Field) \
  uint64_t DdsDiagnostics::Method() noexcept { return Field.fetch_add(1) + 1; }

AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcInvalidRequest, rpc_invalid_request_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcInvalidResponse, rpc_invalid_response_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcForeignResponse, rpc_foreign_response_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcLateResponse, rpc_late_response_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcDuplicateResponse, rpc_duplicate_response_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcUnknownCorrelation, rpc_unknown_correlation_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcPendingRejected, rpc_pending_rejected_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcTerminalHistoryEvicted,
                             rpc_terminal_history_evicted_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcServerNonOkCompletion,
                             rpc_server_non_ok_completion_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcLateServerCompletion,
                             rpc_late_server_completion_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcRequesterUnmatchedOnReply,
                             rpc_requester_unmatched_on_reply_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcReplyMatchTimeout, rpc_reply_match_timeout_total_)
AIMRT_DDS_DEFINE_RPC_COUNTER(RecordRpcMultipleServers, rpc_multiple_servers_total_)

#undef AIMRT_DDS_DEFINE_RPC_COUNTER

DdsAsioExecutor::DdsAsioExecutor(uint32_t thread_num)
    : thread_num_(thread_num), io_context_(thread_num) {
  if (thread_num == 0) throw std::invalid_argument("DDS asio executor thread count must be positive");
}

DdsAsioExecutor::~DdsAsioExecutor() {
  try {
    Shutdown();
  } catch (...) {
  }
}

void DdsAsioExecutor::Start() {
  std::lock_guard lock(mutex_);
  if (started_once_) throw std::logic_error("DDS asio executor cannot be restarted");
  if (state_.load() != DdsRuntimeState::kStopped) {
    throw std::logic_error("DDS asio executor Start requires Stopped state");
  }
  started_once_ = true;
  work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
      io_context_.get_executor());
  state_ = DdsRuntimeState::kRunning;
  const auto self = shared_from_this();
  for (uint32_t index = 0; index < thread_num_; ++index) {
    threads_.emplace_back([self] {
      try {
        self->io_context_.run();
      } catch (const std::exception& error) {
        AIMRT_ERROR("DDS asio worker exited after exception: {}", error.what());
      }
    });
  }
}

bool DdsAsioExecutor::Post(std::function<void()> task) {
  std::lock_guard lock(mutex_);
  if (state_.load() != DdsRuntimeState::kRunning) return false;
  asio::post(io_context_, [task = std::move(task)]() mutable {
    try {
      task();
    } catch (const std::exception& error) {
      AIMRT_ERROR("DDS asio task failed: {}", error.what());
    } catch (...) {
      AIMRT_ERROR("DDS asio task failed with a non-standard exception");
    }
  });
  return true;
}

bool DdsAsioExecutor::ScheduleAfter(std::chrono::steady_clock::duration delay,
                                    std::function<void()> task) {
  std::lock_guard lock(mutex_);
  if (state_.load() != DdsRuntimeState::kRunning) return false;
  auto timer = std::make_shared<asio::steady_timer>(io_context_, delay);
  std::erase_if(timers_, [](const auto& existing) { return existing.expired(); });
  timers_.emplace_back(timer);
  std::weak_ptr<DdsAsioExecutor> weak_self = shared_from_this();
  timer->async_wait([weak_self, timer, task = std::move(task)](const asio::error_code& error) mutable {
    const auto self = weak_self.lock();
    if (!self || error == asio::error::operation_aborted ||
        self->State() != DdsRuntimeState::kRunning) {
      return;
    }
    try {
      task();
    } catch (const std::exception& exception) {
      AIMRT_ERROR("DDS asio timer task failed: {}", exception.what());
    } catch (...) {
      AIMRT_ERROR("DDS asio timer task failed with a non-standard exception");
    }
  });
  return true;
}

void DdsAsioExecutor::Shutdown() {
  std::vector<std::thread> threads;
  {
    std::lock_guard lock(mutex_);
    if (!started_once_ || state_.load() == DdsRuntimeState::kStopped) return;
    if (state_.load() == DdsRuntimeState::kStopping) return;
    const auto current = std::this_thread::get_id();
    if (std::ranges::any_of(threads_, [current](const auto& thread) {
          return thread.get_id() == current;
        })) {
      throw std::logic_error("DDS asio executor cannot join itself");
    }
    state_ = DdsRuntimeState::kStopping;
    for (auto& timer_ref : timers_) {
      if (auto timer = timer_ref.lock()) {
        timer->cancel();
      }
    }
    timers_.clear();
    if (work_guard_) work_guard_->reset();
    threads.swap(threads_);
  }
  for (auto& thread : threads) {
    if (thread.joinable()) thread.join();
  }
  state_ = DdsRuntimeState::kStopped;
}

bool DdsAsioExecutor::IsWorkerThread() const {
  std::lock_guard lock(mutex_);
  const auto current = std::this_thread::get_id();
  return std::ranges::any_of(threads_, [current](const auto& thread) {
    return thread.get_id() == current;
  });
}

DdsReaderDrainState::DdsReaderDrainState(std::weak_ptr<DdsAsioExecutor> executor,
                                         DrainOne drain_one, HasUnread has_unread)
    : executor_(std::move(executor)),
      drain_one_(drain_one ? std::move(drain_one)
                           : [] { return DrainResult::kNoData; }),
      has_unread_(has_unread ? std::move(has_unread) : [] { return false; }) {}

void DdsReaderDrainState::Start() {
  retry_step_ = 0;
  accepting_work_ = true;
  if (has_unread_()) NotifyData();
}

void DdsReaderDrainState::Stop() noexcept {
  accepting_work_ = false;
  wake_pending_ = false;
  drain_scheduled_ = false;
}

void DdsReaderDrainState::NotifyData() noexcept {
  if (!accepting_work_.load()) return;
  wake_pending_ = true;
  if (!drain_scheduled_.exchange(true) && !ScheduleDrain()) drain_scheduled_ = false;
}

bool DdsReaderDrainState::ScheduleDrain() noexcept {
  const auto executor = executor_.lock();
  if (!executor || !accepting_work_.load()) return false;
  const auto self = shared_from_this();
  return executor->Post([self] { self->DrainOnce(); });
}

bool DdsReaderDrainState::ScheduleRetry(DrainResult reason) noexcept {
  static constexpr std::array<uint32_t, 8> kRetryDelayUs{
      1000, 2000, 4000, 8000, 16000, 32000, 64000, 100000};
  const auto executor = executor_.lock();
  if (!executor || !accepting_work_.load()) return false;
  const auto step = retry_step_.fetch_add(1);
  const auto delay_us = kRetryDelayUs[std::min(step, kRetryDelayUs.size() - 1)];
#if defined(BUILD_TESTING)
  retry_schedule_count_.fetch_add(1);
  last_retry_delay_us_ = delay_us;
  if (reason == DrainResult::kRetryTemporaryLoanFailure) {
    temporary_loan_failure_retry_count_.fetch_add(1);
  } else if (reason == DrainResult::kRetryRpcPublicationGateBusy) {
    rpc_publication_gate_busy_retry_count_.fetch_add(1);
  }
#endif
  const auto self = shared_from_this();
  return executor->ScheduleAfter(
      std::chrono::microseconds(delay_us), [self] { self->DrainOnce(); });
}

void DdsReaderDrainState::DrainOnce() noexcept {
#if defined(BUILD_TESTING)
  drain_execution_count_.fetch_add(1);
#endif
  if (!accepting_work_.load()) {
    drain_scheduled_ = false;
    return;
  }

  // This task is now responsible for every wake observed before this point.
  // A listener notification racing with DrainOne or the unread recheck sets
  // the flag again and is consumed only after the active task releases its
  // drain_scheduled ownership below.
  wake_pending_ = false;
  auto result = DrainResult::kRetryLater;
  try {
    result = drain_one_();
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS reader DrainOne failed: {}", error.what());
  } catch (...) {
    AIMRT_ERROR("DDS reader DrainOne failed with a non-standard exception");
  }

  if (!accepting_work_.load()) {
    drain_scheduled_ = false;
    return;
  }
  if (result == DrainResult::kConsumed) {
    retry_step_ = 0;
    if (!ScheduleDrain()) drain_scheduled_ = false;
    return;
  }
  if (result == DrainResult::kRetryLater ||
      result == DrainResult::kRetryTemporaryLoanFailure ||
      result == DrainResult::kRetryRpcPublicationGateBusy) {
    if (!ScheduleRetry(result)) drain_scheduled_ = false;
    return;
  }
  retry_step_ = 0;

  bool unread = false;
  try {
    unread = accepting_work_.load() && has_unread_();
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS reader unread recheck failed: {}", error.what());
  } catch (...) {
    AIMRT_ERROR("DDS reader unread recheck failed with a non-standard exception");
  }
  if (unread) wake_pending_ = true;
  drain_scheduled_ = false;
  if (accepting_work_.load() && wake_pending_.exchange(false) &&
      !drain_scheduled_.exchange(true) && !ScheduleDrain()) {
    drain_scheduled_ = false;
  }
}

#if defined(BUILD_TESTING)
void DdsListenerLifetimeTracker::Record(std::string event) {
  std::lock_guard lock(mutex);
  events.emplace_back(std::move(event));
}
#endif

DdsDataReaderListener::DdsDataReaderListener(
    DdsEndpointMetadata metadata, std::shared_ptr<DdsReaderDrainState> drain_state,
    std::shared_ptr<DdsDiagnostics> diagnostics
#if defined(BUILD_TESTING)
    ,
    std::shared_ptr<DdsListenerLifetimeTracker> lifetime)
#else
    )
#endif
    : metadata_(std::move(metadata)),
      drain_state_(std::move(drain_state)),
      diagnostics_(std::move(diagnostics))
#if defined(BUILD_TESTING)
      ,
      lifetime_(std::move(lifetime)) {
  if (lifetime_) lifetime_->Record("reader_listener_constructed:" + metadata_.topic);
#else
          {
#endif
}

DdsDataReaderListener::~DdsDataReaderListener() {
  diagnostics_->reader_current_matches_.fetch_sub(current_matches_.load());
#if defined(BUILD_TESTING)
  if (lifetime_) lifetime_->Record("reader_listener_destroyed:" + metadata_.topic);
#endif
}

void DdsDataReaderListener::on_data_available(DataReader*) { drain_state_->NotifyData(); }

void DdsDataReaderListener::on_subscription_matched(
    DataReader* reader, const SubscriptionMatchedStatus& status) {
  diagnostics_->reader_match_events_.fetch_add(1);
  const auto previous = current_matches_.exchange(status.current_count);
  diagnostics_->reader_current_matches_.fetch_add(status.current_count - previous);
  AIMRT_INFO(
      "aimrt_dds_listener_match endpoint=reader participant='{}' topic='{}' type='{}' "
      "local_guid='{}' remote_guid='{}' current_count={} current_count_change={} total_count={}",
      metadata_.participant, metadata_.topic, metadata_.type, GuidToString(reader->guid()),
      HandleToString(status.last_publication_handle), status.current_count,
      status.current_count_change, status.total_count);
}

void DdsDataReaderListener::on_requested_incompatible_qos(
    DataReader* reader, const RequestedIncompatibleQosStatus& status) {
  const auto count = diagnostics_->requested_incompatible_qos_total_.fetch_add(
                         std::max(1U, status.total_count_change)) +
                     std::max(1U, status.total_count_change);
  diagnostics_->last_incompatible_qos_policy_ = static_cast<int32_t>(status.last_policy_id);
  if (ShouldLogRateLimited(count)) {
    AIMRT_WARN(
        "aimrt_dds_listener_incompatible_qos endpoint=reader participant='{}' topic='{}' "
        "type='{}' local_guid='{}' policy_id={} total_count={} total_count_change={}",
        metadata_.participant, metadata_.topic, metadata_.type, GuidToString(reader->guid()),
        static_cast<int32_t>(status.last_policy_id), status.total_count, status.total_count_change);
  }
}

void DdsDataReaderListener::on_requested_deadline_missed(
    DataReader* reader, const RequestedDeadlineMissedStatus& status) {
  diagnostics_->requested_deadline_missed_total_.fetch_add(status.total_count_change);
  diagnostics_->requested_deadline_last_change_ = status.total_count_change;
  if (status.total_count_change != 0 && ShouldLogCallbackEvent(deadline_log_events_)) {
    AIMRT_WARN(
        "aimrt_dds_listener_deadline endpoint=reader participant='{}' topic='{}' type='{}' "
        "local_guid='{}' reason='requested_deadline_missed' instance='{}' total_count={} "
        "total_count_change={}",
        metadata_.participant, metadata_.topic, metadata_.type, GuidToString(reader->guid()),
        HandleToString(status.last_instance_handle), status.total_count, status.total_count_change);
  }
}

void DdsDataReaderListener::on_liveliness_changed(
    DataReader* reader, const LivelinessChangedStatus& status) {
  const auto event = diagnostics_->reader_liveliness_change_events_.fetch_add(1) + 1;
  diagnostics_->reader_alive_count_ = status.alive_count;
  diagnostics_->reader_not_alive_count_ = status.not_alive_count;
  diagnostics_->reader_alive_last_change_ = status.alive_count_change;
  diagnostics_->reader_not_alive_last_change_ = status.not_alive_count_change;
  if (ShouldLogCallbackEvent(liveliness_log_events_)) {
    AIMRT_WARN(
        "aimrt_dds_listener_liveliness endpoint=reader participant='{}' topic='{}' type='{}' "
        "local_guid='{}' reason='writer_liveliness_changed' remote_handle='{}' alive_count={} "
        "alive_count_change={} "
        "not_alive_count={} not_alive_count_change={} event_count={}",
        metadata_.participant, metadata_.topic, metadata_.type, GuidToString(reader->guid()),
        HandleToString(status.last_publication_handle), status.alive_count,
        status.alive_count_change, status.not_alive_count, status.not_alive_count_change, event);
  }
}

void DdsDataReaderListener::on_sample_rejected(
    DataReader* reader, const SampleRejectedStatus& status) {
  diagnostics_->sample_rejected_total_.fetch_add(status.total_count_change);
  diagnostics_->sample_rejected_last_change_ = status.total_count_change;
  diagnostics_->last_sample_rejected_reason_ = static_cast<int32_t>(status.last_reason);
  if (status.total_count_change != 0 && ShouldLogCallbackEvent(sample_rejected_log_events_)) {
    AIMRT_WARN(
        "aimrt_dds_listener_sample_rejected participant='{}' topic='{}' type='{}' "
        "local_guid='{}' reason='{}' reason_code={} instance='{}' total_count={} "
        "total_count_change={}",
        metadata_.participant, metadata_.topic, metadata_.type, GuidToString(reader->guid()),
        SampleRejectedReason(status.last_reason), static_cast<int32_t>(status.last_reason),
        HandleToString(status.last_instance_handle), status.total_count, status.total_count_change);
  }
}

void DdsDataReaderListener::on_sample_lost(
    DataReader* reader, const SampleLostStatus& status) {
  diagnostics_->sample_lost_total_.fetch_add(std::max(0, status.total_count_change));
  diagnostics_->sample_lost_last_change_ = status.total_count_change;
  if (status.total_count_change != 0 && ShouldLogCallbackEvent(sample_lost_log_events_)) {
    AIMRT_WARN(
        "aimrt_dds_listener_sample_lost participant='{}' topic='{}' type='{}' "
        "local_guid='{}' reason='sequence_gap' total_count={} total_count_change={}",
        metadata_.participant, metadata_.topic, metadata_.type, GuidToString(reader->guid()),
        status.total_count, status.total_count_change);
  }
}

DdsDataWriterListener::DdsDataWriterListener(
    DdsEndpointMetadata metadata, std::shared_ptr<DdsDiagnostics> diagnostics,
    std::function<void(int32_t)> match_callback
#if defined(BUILD_TESTING)
    ,
    std::shared_ptr<DdsListenerLifetimeTracker> lifetime)
#else
    )
#endif
    : metadata_(std::move(metadata)),
      diagnostics_(std::move(diagnostics)),
      match_callback_(std::move(match_callback))
#if defined(BUILD_TESTING)
      ,
      lifetime_(std::move(lifetime)) {
  if (lifetime_) lifetime_->Record("writer_listener_constructed:" + metadata_.topic);
#else
          {
#endif
}

DdsDataWriterListener::~DdsDataWriterListener() {
  diagnostics_->writer_current_matches_.fetch_sub(current_matches_.load());
  if (match_callback_) match_callback_(0);
#if defined(BUILD_TESTING)
  if (lifetime_) lifetime_->Record("writer_listener_destroyed:" + metadata_.topic);
#endif
}

void DdsDataWriterListener::on_publication_matched(
    DataWriter* writer, const PublicationMatchedStatus& status) {
  diagnostics_->writer_match_events_.fetch_add(1);
  const auto previous = current_matches_.exchange(status.current_count);
  diagnostics_->writer_current_matches_.fetch_add(status.current_count - previous);
  if (match_callback_) match_callback_(status.current_count);
  AIMRT_INFO(
      "aimrt_dds_listener_match endpoint=writer participant='{}' topic='{}' type='{}' "
      "local_guid='{}' remote_guid='{}' current_count={} current_count_change={} total_count={}",
      metadata_.participant, metadata_.topic, metadata_.type, GuidToString(writer->guid()),
      HandleToString(status.last_subscription_handle), status.current_count,
      status.current_count_change, status.total_count);
}

void DdsDataWriterListener::on_offered_incompatible_qos(
    DataWriter* writer, const OfferedIncompatibleQosStatus& status) {
  const auto count = diagnostics_->offered_incompatible_qos_total_.fetch_add(
                         std::max(1U, status.total_count_change)) +
                     std::max(1U, status.total_count_change);
  diagnostics_->last_incompatible_qos_policy_ = static_cast<int32_t>(status.last_policy_id);
  if (ShouldLogRateLimited(count)) {
    AIMRT_WARN(
        "aimrt_dds_listener_incompatible_qos endpoint=writer participant='{}' topic='{}' "
        "type='{}' local_guid='{}' policy_id={} total_count={} total_count_change={}",
        metadata_.participant, metadata_.topic, metadata_.type, GuidToString(writer->guid()),
        static_cast<int32_t>(status.last_policy_id), status.total_count, status.total_count_change);
  }
}

void DdsDataWriterListener::on_offered_deadline_missed(
    DataWriter* writer, const OfferedDeadlineMissedStatus& status) {
  diagnostics_->offered_deadline_missed_total_.fetch_add(status.total_count_change);
  diagnostics_->offered_deadline_last_change_ = status.total_count_change;
  if (status.total_count_change != 0 && ShouldLogCallbackEvent(deadline_log_events_)) {
    AIMRT_WARN(
        "aimrt_dds_listener_deadline endpoint=writer participant='{}' topic='{}' type='{}' "
        "local_guid='{}' reason='offered_deadline_missed' instance='{}' total_count={} "
        "total_count_change={}",
        metadata_.participant, metadata_.topic, metadata_.type, GuidToString(writer->guid()),
        HandleToString(status.last_instance_handle), status.total_count, status.total_count_change);
  }
}

void DdsDataWriterListener::on_liveliness_lost(
    DataWriter* writer, const LivelinessLostStatus& status) {
  diagnostics_->writer_liveliness_lost_total_.fetch_add(
      std::max(0, status.total_count_change));
  diagnostics_->writer_liveliness_lost_last_change_ = status.total_count_change;
  if (status.total_count_change != 0 && ShouldLogCallbackEvent(liveliness_log_events_)) {
    AIMRT_WARN(
        "aimrt_dds_listener_liveliness endpoint=writer participant='{}' topic='{}' type='{}' "
        "local_guid='{}' reason='lease_expired' total_count={} total_count_change={}",
        metadata_.participant, metadata_.topic, metadata_.type, GuidToString(writer->guid()),
        status.total_count, status.total_count_change);
  }
}

struct DdsEndpointManager::TopicEntry {
  Topic* topic = nullptr;
  std::string type;
  TypeSupport type_support;
};

struct DdsEndpointManager::WriterEntry {
  DataWriter* writer = nullptr;
  DataWriterQos qos;
  std::string registration_origin;
  std::unique_ptr<DdsDataWriterListener> listener;
  bool listener_detached = false;
};

struct DdsEndpointManager::ReaderEntry {
  DataReader* reader = nullptr;
  ContentFilteredTopic* filtered_topic = nullptr;
  DataReaderQos qos;
  std::string registration_origin;
  std::shared_ptr<DdsReaderDrainState> drain_state;
  std::unique_ptr<DdsDataReaderListener> listener;
  bool listener_detached = false;
};

DdsEndpointManager::DdsEndpointManager() = default;

DdsEndpointManager::WriterOperation::~WriterOperation() { Reset(); }

DdsEndpointManager::WriterOperation::WriterOperation(WriterOperation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

DdsEndpointManager::WriterOperation& DdsEndpointManager::WriterOperation::operator=(
    WriterOperation&& other) noexcept {
  if (this != &other) {
    Reset();
    owner_ = std::exchange(other.owner_, nullptr);
  }
  return *this;
}

void DdsEndpointManager::WriterOperation::Reset() noexcept {
  if (owner_ == nullptr) return;
  auto* owner = std::exchange(owner_, nullptr);
  owner->ReleaseWriterOperation();
}

bool DdsEndpointManager::WriterOperation::Stopping() const noexcept {
  return owner_ == nullptr || owner_->WriterOperationStopping();
}

DdsEndpointManager::~DdsEndpointManager() {
  try {
    BeginStopping();
    DeleteEntities();
  } catch (...) {
  }
}

void DdsEndpointManager::Initialize(DomainParticipant* participant, const DdsQosSnapshot& qos,
                                    std::shared_ptr<DdsAsioExecutor> executor,
                                    std::shared_ptr<DdsDiagnostics> diagnostics) {
  std::lock_guard lock(mutex_);
  if (participant_ != nullptr || participant == nullptr) {
    throw std::logic_error("DDS endpoint manager Initialize requires one valid participant");
  }
  participant_ = participant;
  executor_ = std::move(executor);
  diagnostics_ = std::move(diagnostics);
  topic_qos_ = qos.topic;
  auto publisher_qos = qos.publisher;
  publisher_qos.entity_factory().autoenable_created_entities = false;
  publisher_ = participant_->create_publisher(publisher_qos);
  if (publisher_ == nullptr) {
    participant_ = nullptr;
    throw std::runtime_error("Fast DDS create_publisher failed");
  }
  auto subscriber_qos = qos.subscriber;
  subscriber_qos.entity_factory().autoenable_created_entities = false;
  subscriber_ = participant_->create_subscriber(subscriber_qos);
  if (subscriber_ == nullptr) {
    RequireOk(participant_->delete_publisher(publisher_), "delete_publisher rollback");
    publisher_ = nullptr;
    participant_ = nullptr;
    throw std::runtime_error("Fast DDS create_subscriber failed");
  }
  rpc_filter_factory_ = std::make_unique<AimrtRpcContentFilterFactory>();
  const auto filter_result = participant_->register_content_filter_factory(
      "AimRT_DDS_RPC_RELATED_PREFIX", rpc_filter_factory_.get());
  if (filter_result != RETCODE_OK) {
    rpc_filter_factory_.reset();
    RequireOk(participant_->delete_subscriber(subscriber_), "delete_subscriber rollback");
    subscriber_ = nullptr;
    RequireOk(participant_->delete_publisher(publisher_), "delete_publisher rollback");
    publisher_ = nullptr;
    participant_ = nullptr;
    executor_.reset();
    diagnostics_.reset();
    throw std::runtime_error("Fast DDS register_content_filter_factory failed");
  }
}

DdsEndpointManager::TopicEntry& DdsEndpointManager::GetOrCreateTopicLocked(
    std::string_view topic, const TypeSupport& type_support) {
  if (!type_support) throw std::invalid_argument("DDS endpoint TypeSupport is null");
  const std::string type = type_support->get_name();
  const auto existing = topics_.find(std::string(topic));
  if (existing != topics_.end()) {
    if (existing->second->type != type) {
      throw std::invalid_argument(
          "DDS topic/type conflict: topic='" + std::string(topic) + "', incoming_type='" + type +
          "', existing_type='" + existing->second->type + "'");
    }
    return *existing->second;
  }

  RequireOk(type_support.register_type(participant_), "register_type");
  auto entry = std::make_unique<TopicEntry>();
  entry->type = type;
  entry->type_support = type_support;
  entry->topic = participant_->create_topic(std::string(topic), type, topic_qos_);
  if (entry->topic == nullptr) {
    throw std::runtime_error("Fast DDS create_topic failed: topic='" + std::string(topic) +
                             "', type='" + type + "'");
  }
  auto* result = entry.get();
  topics_.emplace(std::string(topic), std::move(entry));
  return *result;
}

void DdsEndpointManager::RollbackTopicIfUnusedLocked(std::string_view topic) {
  const auto key_prefix = std::string(topic) + "\n";
  const auto uses_topic = [&key_prefix](const auto& entries) {
    return std::ranges::any_of(entries, [&key_prefix](const auto& item) {
      return item.first.starts_with(key_prefix);
    });
  };
  if (uses_topic(writers_) || uses_topic(readers_)) return;
  const auto iterator = topics_.find(std::string(topic));
  if (iterator == topics_.end()) return;
  RequireOk(participant_->delete_topic(iterator->second->topic), "delete_topic rollback");
  topics_.erase(iterator);
}

DataWriter* DdsEndpointManager::GetOrCreateWriter(
    std::string_view topic, const TypeSupport& type_support, const DataWriterQos& writer_qos,
    std::string_view registration_origin,
    std::function<void(int32_t)> match_callback) {
  std::lock_guard lock(mutex_);
  if (stopping_ || participant_ == nullptr) {
    throw std::logic_error("DDS writer creation rejected while endpoint manager is stopping");
  }
  const auto requested_origin = RequireRegistrationOrigin(registration_origin);
  const auto key = WriterKey(topic, type_support ? type_support->get_name() : "<null>", requested_origin);
  if (const auto existing = writers_.find(key); existing != writers_.end()) {
    if (existing->second->qos != writer_qos) {
      throw std::invalid_argument("DDS shared writer QoS conflict: topic='" + std::string(topic) +
                                  "', type='" + type_support->get_name() +
                                  "', existing_origin='" + existing->second->registration_origin +
                                  "', requested_origin='" + requested_origin +
                                  "', conflicting_policies=[" +
                                  ChangedWriterQosPolicies(existing->second->qos, writer_qos) +
                                  "], existing_effective_qos=" +
                                  EffectiveQosSummary(existing->second->qos) +
                                  ", requested_effective_qos=" + EffectiveQosSummary(writer_qos));
    }
    return existing->second->writer;
  }

  const bool topic_existed = topics_.contains(std::string(topic));
  auto& topic_entry = GetOrCreateTopicLocked(topic, type_support);
  auto entry = std::make_unique<WriterEntry>();
  entry->qos = writer_qos;
  entry->registration_origin = requested_origin;
  entry->listener = std::make_unique<DdsDataWriterListener>(
      DdsEndpointMetadata{.participant = GuidToString(participant_->guid()),
                          .topic = std::string(topic),
                          .type = topic_entry.type},
      diagnostics_, std::move(match_callback)
#if defined(BUILD_TESTING)
                        ,
      lifetime_tracker_
#endif
  );
  entry->writer = publisher_->create_datawriter(topic_entry.topic, writer_qos, entry->listener.get());
  if (entry->writer == nullptr) {
    entry->listener.reset();
    if (!topic_existed) RollbackTopicIfUnusedLocked(topic);
    throw std::runtime_error("Fast DDS create_datawriter failed: topic='" + std::string(topic) + "'");
  }
  auto* result = entry->writer;
  if (!requested_origin.starts_with("rpc ")) RequireOk(result->enable(), "DataWriter::enable");
  writers_.emplace(key, std::move(entry));
  return result;
}

DdsEndpointManager::ReaderEndpoint DdsEndpointManager::GetOrCreateReader(
    std::string_view topic, const TypeSupport& type_support, const DataReaderQos& reader_qos,
    std::string_view registration_origin, DdsReaderDrainState::DrainOne drain_one,
    DdsReaderDrainState::HasUnread has_unread) {
  std::lock_guard lock(mutex_);
  if (stopping_ || participant_ == nullptr) {
    throw std::logic_error("DDS reader creation rejected while endpoint manager is stopping");
  }
  const auto requested_origin = RequireRegistrationOrigin(registration_origin);
  const auto key = ReaderKey(topic, type_support ? type_support->get_name() : "<null>", requested_origin);
  if (const auto existing = readers_.find(key); existing != readers_.end()) {
    if (existing->second->qos != reader_qos) {
      throw std::invalid_argument("DDS shared reader QoS conflict: topic='" + std::string(topic) +
                                  "', type='" + type_support->get_name() +
                                  "', existing_origin='" + existing->second->registration_origin +
                                  "', requested_origin='" + requested_origin +
                                  "', conflicting_policies=[" +
                                  ChangedReaderQosPolicies(existing->second->qos, reader_qos) +
                                  "], existing_effective_qos=" +
                                  EffectiveQosSummary(existing->second->qos) +
                                  ", requested_effective_qos=" + EffectiveQosSummary(reader_qos));
    }
    return {.reader = existing->second->reader, .drain_state = existing->second->drain_state};
  }

  const bool topic_existed = topics_.contains(std::string(topic));
  auto& topic_entry = GetOrCreateTopicLocked(topic, type_support);
  auto entry = std::make_unique<ReaderEntry>();
  entry->qos = reader_qos;
  entry->registration_origin = requested_origin;
  entry->drain_state = std::make_shared<DdsReaderDrainState>(
      executor_, std::move(drain_one), std::move(has_unread));
  entry->listener = std::make_unique<DdsDataReaderListener>(
      DdsEndpointMetadata{.participant = GuidToString(participant_->guid()),
                          .topic = std::string(topic),
                          .type = topic_entry.type},
      entry->drain_state, diagnostics_
#if defined(BUILD_TESTING)
      ,
      lifetime_tracker_
#endif
  );
  entry->reader = subscriber_->create_datareader(topic_entry.topic, reader_qos, entry->listener.get());
  if (entry->reader == nullptr) {
    entry->listener.reset();
    if (!topic_existed) RollbackTopicIfUnusedLocked(topic);
    throw std::runtime_error("Fast DDS create_datareader failed: topic='" + std::string(topic) + "'");
  }
  if (!requested_origin.starts_with("rpc ")) RequireOk(entry->reader->enable(), "DataReader::enable");
  if (running_) entry->drain_state->Start();
  ReaderEndpoint result{.reader = entry->reader, .drain_state = entry->drain_state};
  readers_.emplace(key, std::move(entry));
  return result;
}

DdsEndpointManager::ReaderEndpoint DdsEndpointManager::GetOrCreateFilteredReader(
    std::string_view topic, const TypeSupport& type_support, const DataReaderQos& reader_qos,
    std::string_view registration_origin, std::string filter_name,
    std::string filter_expression, std::vector<std::string> filter_parameters,
    DdsReaderDrainState::DrainOne drain_one, DdsReaderDrainState::HasUnread has_unread) {
  std::lock_guard lock(mutex_);
  if (stopping_ || participant_ == nullptr) {
    throw std::logic_error("DDS filtered reader creation rejected while stopping");
  }
  const auto requested_origin = RequireRegistrationOrigin(registration_origin);
  const auto key = EndpointKey(topic, type_support ? type_support->get_name() : "<null>") +
                   "\nfiltered:" + filter_name;
  if (readers_.contains(key)) {
    throw std::invalid_argument("DDS filtered reader key already exists");
  }
  const bool topic_existed = topics_.contains(std::string(topic));
  auto& topic_entry = GetOrCreateTopicLocked(topic, type_support);
#if defined(BUILD_TESTING)
  if (fail_next_filtered_reader_creation_.exchange(false)) {
    if (!topic_existed) RollbackTopicIfUnusedLocked(topic);
    throw std::runtime_error("Forced DDS filtered reader creation failure");
  }
#endif
  ReturnCode_t filter_result = RETCODE_OK;
  auto* filtered = participant_->create_contentfilteredtopic(
      filter_name, topic_entry.topic, filter_expression, filter_parameters,
      "AimRT_DDS_RPC_RELATED_PREFIX", filter_result);
  if (filtered == nullptr || filter_result != RETCODE_OK) {
    if (!topic_existed) RollbackTopicIfUnusedLocked(topic);
    throw std::runtime_error("Fast DDS create_contentfilteredtopic failed");
  }
  auto entry = std::make_unique<ReaderEntry>();
  entry->filtered_topic = filtered;
  entry->qos = reader_qos;
  entry->registration_origin = requested_origin;
  entry->drain_state = std::make_shared<DdsReaderDrainState>(
      executor_, std::move(drain_one), std::move(has_unread));
  entry->listener = std::make_unique<DdsDataReaderListener>(
      DdsEndpointMetadata{.participant = GuidToString(participant_->guid()),
                          .topic = std::string(topic),
                          .type = topic_entry.type},
      entry->drain_state, diagnostics_
#if defined(BUILD_TESTING)
      ,
      lifetime_tracker_
#endif
  );
  entry->reader = subscriber_->create_datareader(filtered, reader_qos, entry->listener.get());
  if (entry->reader == nullptr) {
    entry->listener.reset();
    RequireOk(participant_->delete_contentfilteredtopic(filtered),
              "delete_contentfilteredtopic rollback");
    if (!topic_existed) RollbackTopicIfUnusedLocked(topic);
    throw std::runtime_error("Fast DDS create filtered datareader failed");
  }
  if (!entry->registration_origin.starts_with("rpc ")) {
    RequireOk(entry->reader->enable(), "DataReader::enable");
  }
  if (running_) entry->drain_state->Start();
  ReaderEndpoint result{.reader = entry->reader,
                        .drain_state = entry->drain_state,
                        .filtered_topic = entry->filtered_topic};
  readers_.emplace(key, std::move(entry));
  return result;
}

DdsEndpointManager::WriterOperation DdsEndpointManager::AcquireWriterOperation() noexcept {
  std::lock_guard lock(mutex_);
  if (stopping_ || participant_ == nullptr) return {};
  ++writer_operations_in_flight_;
  return WriterOperation(this);
}

bool DdsEndpointManager::EnableWriter(DataWriter* writer) noexcept {
#if defined(BUILD_TESTING)
  if (fail_next_endpoint_enable_.exchange(false)) return false;
#endif
  return writer != nullptr && writer->enable() == RETCODE_OK;
}

bool DdsEndpointManager::EnableReader(DataReader* reader) noexcept {
#if defined(BUILD_TESTING)
  if (fail_next_endpoint_enable_.exchange(false)) return false;
#endif
  return reader != nullptr && reader->enable() == RETCODE_OK;
}

void DdsEndpointManager::DeleteWriter(DataWriter* writer) {
  std::lock_guard lock(mutex_);
  if (writer == nullptr) return;
  if (writer_operations_in_flight_ != 0) {
    throw std::logic_error("DDS writer rollback requires no active write operation");
  }
  const auto iterator = std::ranges::find_if(
      writers_, [writer](const auto& item) { return item.second->writer == writer; });
  if (iterator == writers_.end()) return;
  const auto separator = iterator->first.find('\n');
  const auto topic = iterator->first.substr(0, separator);
  auto& entry = *iterator->second;
  if (!entry.listener_detached) {
    RequireOk(entry.writer->set_listener(nullptr), "DataWriter::set_listener(nullptr) rollback");
    entry.listener_detached = true;
  }
  RequireOk(publisher_->delete_datawriter(entry.writer), "Publisher::delete_datawriter rollback");
  entry.writer = nullptr;
  entry.listener.reset();
  writers_.erase(iterator);
  RollbackTopicIfUnusedLocked(topic);
}

void DdsEndpointManager::DeleteReader(DataReader* reader) {
  std::lock_guard lock(mutex_);
  if (reader == nullptr) return;
  const auto iterator = std::ranges::find_if(
      readers_, [reader](const auto& item) { return item.second->reader == reader; });
  if (iterator == readers_.end()) return;
  const auto separator = iterator->first.find('\n');
  const auto topic = iterator->first.substr(0, separator);
  auto& entry = *iterator->second;
  entry.drain_state->Stop();
  if (!entry.listener_detached) {
    RequireOk(entry.reader->set_listener(nullptr), "DataReader::set_listener(nullptr) rollback");
    entry.listener_detached = true;
  }
  RequireOk(subscriber_->delete_datareader(entry.reader), "Subscriber::delete_datareader rollback");
  entry.reader = nullptr;
  entry.listener.reset();
  if (entry.filtered_topic != nullptr) {
    RequireOk(participant_->delete_contentfilteredtopic(entry.filtered_topic),
              "DomainParticipant::delete_contentfilteredtopic rollback");
    entry.filtered_topic = nullptr;
  }
  readers_.erase(iterator);
  RollbackTopicIfUnusedLocked(topic);
}

void DdsEndpointManager::ReleaseWriterOperation() noexcept {
  std::lock_guard lock(mutex_);
  if (writer_operations_in_flight_ == 0) return;
  --writer_operations_in_flight_;
  if (writer_operations_in_flight_ == 0) writer_operations_drained_.notify_all();
}

bool DdsEndpointManager::WriterOperationStopping() const noexcept {
  std::lock_guard lock(mutex_);
  return stopping_;
}

void DdsEndpointManager::Start() {
  std::lock_guard lock(mutex_);
  if (stopping_) throw std::logic_error("DDS endpoint manager cannot Start while stopping");
  if (running_) return;
  running_ = true;
  for (auto& [_, reader] : readers_) reader->drain_state->Start();
}

void DdsEndpointManager::BeginStopping() {
  std::unique_lock lock(mutex_);
  if (participant_ == nullptr) return;
  stopping_ = true;
  running_ = false;
  writer_operations_drained_.wait(lock, [this] { return writer_operations_in_flight_ == 0; });
  for (auto& [_, reader] : readers_) {
    reader->drain_state->Stop();
    if (!reader->listener_detached) {
      RequireOk(reader->reader->set_listener(nullptr), "DataReader::set_listener(nullptr)");
      reader->listener_detached = true;
    }
  }
  for (auto& [_, writer] : writers_) {
    if (!writer->listener_detached) {
      RequireOk(writer->writer->set_listener(nullptr), "DataWriter::set_listener(nullptr)");
      writer->listener_detached = true;
    }
  }
}

void DdsEndpointManager::DeleteEntities() {
  std::lock_guard lock(mutex_);
  if (participant_ == nullptr) return;
  if (!stopping_) throw std::logic_error("DDS entities can only be deleted after BeginStopping");

  for (auto iterator = writers_.begin(); iterator != writers_.end();) {
    auto& entry = *iterator->second;
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("writer_delete_begin:" + iterator->first);
#endif
    RequireOk(publisher_->delete_datawriter(entry.writer), "Publisher::delete_datawriter");
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("writer_delete_ok:" + iterator->first);
#endif
    entry.writer = nullptr;
    entry.listener.reset();
    iterator = writers_.erase(iterator);
  }
  for (auto& [key, reader] : readers_) {
    auto& entry = *reader;
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("reader_delete_begin:" + key);
#endif
    RequireOk(subscriber_->delete_datareader(entry.reader), "Subscriber::delete_datareader");
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("reader_delete_ok:" + key);
#endif
    entry.reader = nullptr;
    entry.listener.reset();
  }
  for (auto& [key, reader] : readers_) {
    auto& entry = *reader;
    if (entry.filtered_topic == nullptr) continue;
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("cft_delete_begin:" + key);
#endif
    RequireOk(participant_->delete_contentfilteredtopic(entry.filtered_topic),
              "DomainParticipant::delete_contentfilteredtopic");
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("cft_delete_ok:" + key);
#endif
    entry.filtered_topic = nullptr;
  }
  readers_.clear();
  for (auto iterator = topics_.begin(); iterator != topics_.end();) {
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("topic_delete_begin:" + iterator->first);
#endif
    RequireOk(participant_->delete_topic(iterator->second->topic), "DomainParticipant::delete_topic");
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("topic_delete_ok:" + iterator->first);
#endif
    iterator = topics_.erase(iterator);
  }
  if (publisher_ != nullptr) {
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("publisher_delete_begin");
#endif
    RequireOk(participant_->delete_publisher(publisher_), "DomainParticipant::delete_publisher");
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("publisher_delete_ok");
#endif
    publisher_ = nullptr;
  }
  if (rpc_filter_factory_ && participant_ != nullptr) {
    RequireOk(participant_->unregister_content_filter_factory(
                  "AimRT_DDS_RPC_RELATED_PREFIX"),
              "DomainParticipant::unregister_content_filter_factory");
    rpc_filter_factory_.reset();
  }
  if (subscriber_ != nullptr) {
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("subscriber_delete_begin");
#endif
    RequireOk(participant_->delete_subscriber(subscriber_), "DomainParticipant::delete_subscriber");
#if defined(BUILD_TESTING)
    if (lifetime_tracker_) lifetime_tracker_->Record("subscriber_delete_ok");
#endif
    subscriber_ = nullptr;
  }
  participant_ = nullptr;
  executor_.reset();
  diagnostics_.reset();
}

size_t DdsEndpointManager::WriterCount() const {
  std::lock_guard lock(mutex_);
  return writers_.size();
}

size_t DdsEndpointManager::ReaderCount() const {
  std::lock_guard lock(mutex_);
  return readers_.size();
}

size_t DdsEndpointManager::TopicCount() const {
  std::lock_guard lock(mutex_);
  return topics_.size();
}

size_t DdsEndpointManager::FilteredTopicCount() const {
  std::lock_guard lock(mutex_);
  return std::ranges::count_if(readers_, [](const auto& item) {
    return item.second->filtered_topic != nullptr;
  });
}

#if defined(BUILD_TESTING)
void DdsEndpointManager::SetListenerLifetimeTracker(
    std::shared_ptr<DdsListenerLifetimeTracker> tracker) {
  std::lock_guard lock(mutex_);
  if (!readers_.empty() || !writers_.empty()) {
    throw std::logic_error("DDS listener tracker must be installed before endpoint creation");
  }
  lifetime_tracker_ = std::move(tracker);
}
#endif

DdsRuntime::DdsRuntime() = default;

DdsRuntime::~DdsRuntime() {
  try {
    Shutdown();
  } catch (...) {
  }
}

void DdsRuntime::Initialize(DdsParticipantContext& participant_context) {
  std::lock_guard lock(mutex_);
  if (initialized_) throw std::logic_error("DDS runtime cannot be initialized twice");
  const auto thread_num = ResolveDdsExecutorThreadNum(
      participant_context.Options().executor.thread_num, std::thread::hardware_concurrency());
  executor_ = std::make_shared<DdsAsioExecutor>(thread_num);
  diagnostics_ = std::make_shared<DdsDiagnostics>();
  endpoints_.Initialize(participant_context.Participant(), participant_context.Qos(), executor_, diagnostics_);
  initialized_ = true;
  state_ = DdsRuntimeState::kStopped;
}

void DdsRuntime::Start() {
  std::lock_guard lock(mutex_);
  if (!initialized_) throw std::logic_error("DDS runtime must be initialized before Start");
  if (state_.load() == DdsRuntimeState::kRunning) return;
  if (state_.load() == DdsRuntimeState::kStopping) {
    throw std::logic_error("DDS runtime cannot Start while stopping");
  }
  executor_->Start();
  endpoints_.Start();
  state_ = DdsRuntimeState::kRunning;
}

void DdsRuntime::Shutdown() {
  std::lock_guard lock(mutex_);
  if (!initialized_) return;
  state_ = DdsRuntimeState::kStopping;
  endpoints_.BeginStopping();
  executor_->Shutdown();
  endpoints_.DeleteEntities();
  state_ = DdsRuntimeState::kStopped;
  initialized_ = false;
}

uint32_t DdsRuntime::ThreadNum() const noexcept {
  return executor_ ? executor_->ThreadNum() : 0;
}

}  // namespace aimrt::plugins::dds_plugin
