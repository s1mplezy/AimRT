// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "dds_plugin/dds_backend.h"

#include <cstring>
#include <deque>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fastdds/dds/core/LoanableSequence.hpp>
#include <fastdds/dds/core/ReturnCode.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/rtps/common/WriteParams.hpp>

#include "core/channel/channel_backend_tools.h"
#include "core/rpc/rpc_backend_tools.h"
#include "dds_plugin/global.h"
#include "dds_plugin/serialized_message_adapter.h"
#include "util/log_util.h"

namespace aimrt::plugins::dds_plugin {
namespace {

using eprosima::fastdds::dds::TypeSupport;

void ValidateBackendOptions(const YAML::Node& node, std::string_view backend) {
  if (node && !node.IsNull() && (!node.IsMap() || node.size() != 0)) {
    throw std::invalid_argument("DDS " + std::string(backend) + " backend options must be an empty map");
  }
}

TypeSupport CopyMessageTypeSupport(const aimrt::util::TypeSupportRef& type_support,
                                   std::string_view object, std::string_view role) {
  if (!type_support) {
    throw std::invalid_argument("DDS " + std::string(role) +
                                " message TypeSupport is null for '" +
                                std::string(object) + "'");
  }
  const auto type_name = type_support.TypeName();
  if (!type_name.starts_with("dds:") || type_name.size() == 4) {
    throw std::invalid_argument("DDS " + std::string(role) + " message type for '" +
                                std::string(object) + "' must use canonical dds: naming, got '" +
                                std::string(type_name) + "'");
  }
  const auto* source = static_cast<const TypeSupport*>(type_support.CustomTypeSupportPtr());
  if (source == nullptr || !*source) {
    throw std::invalid_argument("DDS " + std::string(role) +
                                " message-level TypeSupport handle is null for '" +
                                std::string(object) + "'");
  }
  if ((*source)->get_name() != type_name.substr(4)) {
    throw std::invalid_argument("DDS " + std::string(role) + " TypeSupport name mismatch for '" +
                                std::string(object) + "': aimrt='" + std::string(type_name) +
                                "', fastdds='" + (*source)->get_name() + "'");
  }
  // TypeSupport is a reference-counted handle. Copying it is the ownership
  // boundary; the backend never mutates or deletes the static source pointer.
  return *source;
}

struct ChannelWireType {
  TypeSupport type_support;
  std::string serialization_type;
  bool wrapper = false;
};

ChannelWireType ResolveChannelWireType(
    const runtime::core::channel::TopicInfo& info, std::string_view role) {
  const auto wrapper_serialization = WrapperSerializationType(info.msg_type);
  if (wrapper_serialization.empty()) {
    return {.type_support = CopyMessageTypeSupport(
                info.msg_type_support_ref, info.topic_name, role),
            .serialization_type = "dds_xcdr2",
            .wrapper = false};
  }
  if (!WrapperAdapterEnabled(info.msg_type)) {
    throw std::invalid_argument(
        "DDS " + std::string(role) + " optional adapter is disabled for '" +
        info.msg_type + "'");
  }
  if (!info.msg_type_support_ref ||
      info.msg_type_support_ref.TypeName() != info.msg_type) {
    throw std::invalid_argument(
        "DDS " + std::string(role) +
        " wrapper type support does not match registered type '" +
        info.msg_type + "'");
  }
  if (!info.msg_type_support_ref.CheckSerializationTypeSupported(
          wrapper_serialization)) {
    throw std::invalid_argument(
        "DDS " + std::string(role) + " type '" + info.msg_type +
        "' does not support required serialization_type='" +
        std::string(wrapper_serialization) + "'");
  }
  return {.type_support = GetSerializedMessageTypeSupport(),
          .serialization_type = std::string(wrapper_serialization),
          .wrapper = true};
}

bool ShouldLogRateLimited(uint64_t count) {
  return count != 0 && (count & (count - 1)) == 0;
}

struct BoundedSerializedPayload {
  BoundedBufferArrayAllocator allocator{kSerializedMessageDataMaxBytes};
  aimrt::util::BufferArray buffers{allocator.NativeHandle()};
};

std::shared_ptr<aimrt::util::BufferArrayView>
SerializeWrapperPayloadWithCache(
    runtime::core::channel::MsgWrapper& wrapper,
    std::string_view serialization_type) {
  if (const auto cached = wrapper.serialization_cache.find(serialization_type);
      cached != wrapper.serialization_cache.end()) {
    return cached->second;
  }

  runtime::core::channel::CheckMsg(wrapper);
  auto payload = std::make_unique<BoundedSerializedPayload>();
  const bool serialized = wrapper.info.msg_type_support_ref.Serialize(
      serialization_type, wrapper.msg_ptr, payload->allocator.NativeHandle(),
      payload->buffers.BufferArrayNativeHandle());
  if (!serialized) {
    if (payload->allocator.LimitExceeded())
      throw std::invalid_argument("data exceeds 16 MiB");
    throw std::invalid_argument("DDS wrapper payload serialization failed");
  }

  auto* payload_ptr = payload.get();
  auto payload_view = std::shared_ptr<aimrt::util::BufferArrayView>(
      new aimrt::util::BufferArrayView(payload_ptr->buffers),
      [payload{std::move(payload)}](const auto* view) { delete view; });
  wrapper.serialization_cache.emplace(std::string(serialization_type),
                                      payload_view);
  return payload_view;
}

}  // namespace

struct DdsRpcBackend::CallbackState {
  CallbackState(std::shared_ptr<DdsBackendState> backend_state,
                std::shared_ptr<std::atomic_bool> running)
      : backend_state(std::move(backend_state)), running(std::move(running)) {}

  std::shared_ptr<DdsBackendState> backend_state;
  std::shared_ptr<std::atomic_bool> running;
#if defined(BUILD_TESTING)
  std::atomic_bool fail_next_response_serialization = false;
  std::atomic_uint64_t server_drains_in_flight = 0;
  std::atomic_uint64_t response_write_calls = 0;
  std::mutex response_write_hook_mutex;
  std::function<void()> before_response_write_hook;
  std::mutex timeout_completion_hook_mutex;
  std::function<void()> before_timeout_completion_hook;
  std::mutex shutdown_hook_mutex;
  std::function<void()> after_shutdown_started_hook;
  std::shared_ptr<std::function<int()>> requester_match_override;
  std::shared_ptr<std::function<void(
      eprosima::fastdds::rtps::SampleIdentity&,
      const eprosima::fastdds::rtps::SampleIdentity&)>>
      request_identity_override;
#endif
};

DdsRpcBackend::DdsRpcBackend(std::shared_ptr<DdsBackendState> state)
    : state_(std::move(state)),
      callback_state_(std::make_shared<CallbackState>(state_, running_token_)) {}

struct DdsRpcBackend::ClientState {
  static constexpr size_t kPendingLimit = 1024;
  static constexpr size_t kTerminalHistoryLimit = 1024;
  runtime::core::rpc::FuncInfo info;
  TypeSupport request_type;
  TypeSupport response_type;
  bool wrapper = false;
  std::string serialization_type;
  eprosima::fastdds::dds::DataWriter* request_writer = nullptr;
  eprosima::fastdds::dds::DataReader* response_reader = nullptr;
  eprosima::fastdds::rtps::GUID_t response_reader_guid;
  std::string endpoint_id;
  std::string response_cft_name;
  std::string response_filter_expression;
  std::string response_filter_signature;
  std::shared_ptr<DdsReaderDrainState> drain;
  std::shared_ptr<DdsDiagnostics> diagnostics;
  std::mutex publication_mutex;
  std::mutex mutex;
  struct Pending {
    std::shared_ptr<runtime::core::rpc::InvokeWrapper> invoke;
    eprosima::fastdds::rtps::SampleIdentity request_identity;
    std::chrono::steady_clock::time_point deadline;
  };
  std::unordered_map<std::string, Pending> pending;
  struct Terminal {
    SampleIdentity request_identity;
    TerminalReason reason;
  };
  std::unordered_map<std::string, Terminal> terminal_history;
  std::deque<std::string> terminal_order;
  std::atomic_int32_t observed_matched_servers{0};
#if defined(BUILD_TESTING)
  std::atomic_int32_t forced_response_take_result{
      static_cast<int32_t>(eprosima::fastdds::dds::RETCODE_OK)};
#endif
  enum class Readiness : uint8_t { kUnmatched,
                                   kPartiallyMatched,
                                   kMatched };
  Readiness ReadinessState() const {
    eprosima::fastdds::dds::SubscriptionMatchedStatus reader_status;
    eprosima::fastdds::dds::PublicationMatchedStatus writer_status;
    const auto response_writer_matches =
        response_reader != nullptr &&
                response_reader->get_subscription_matched_status(reader_status) ==
                    eprosima::fastdds::dds::RETCODE_OK
            ? reader_status.current_count
            : 0;
    const auto request_reader_matches =
        request_writer != nullptr &&
                request_writer->get_publication_matched_status(writer_status) ==
                    eprosima::fastdds::dds::RETCODE_OK
            ? writer_status.current_count
            : 0;
    if (response_writer_matches == 0) return Readiness::kUnmatched;
    if (request_reader_matches == 0 ||
        response_writer_matches != request_reader_matches) {
      return Readiness::kPartiallyMatched;
    }
    return Readiness::kMatched;
  }
};

struct DdsRpcBackend::ServerState {
  runtime::core::rpc::FuncInfo info;
  runtime::core::rpc::ServiceFunc service_func;
  TypeSupport request_type;
  TypeSupport response_type;
  bool wrapper = false;
  std::string serialization_type;
  eprosima::fastdds::dds::DataReader* request_reader = nullptr;
  eprosima::fastdds::dds::DataWriter* response_writer = nullptr;
  std::shared_ptr<DdsReaderDrainState> drain;
#if defined(BUILD_TESTING)
  std::atomic_int32_t forced_request_take_result{
      static_cast<int32_t>(eprosima::fastdds::dds::RETCODE_OK)};
#endif
};

namespace {
std::string IdentityKey(const eprosima::fastdds::rtps::SampleIdentity& id) {
  std::ostringstream out;
  out << id.writer_guid() << ':' << id.sequence_number();
  return out.str();
}

std::string LogicalKey(const runtime::core::rpc::FuncInfo& info) {
  return info.func_name + "\n" + info.pkg_path + "\n" + info.module_name + "\n" +
         std::to_string(info.index);
}

bool IdentityValid(const eprosima::fastdds::rtps::SampleIdentity& id) {
  return id.writer_guid() != eprosima::fastdds::rtps::GUID_t{} &&
         id.sequence_number() != eprosima::fastdds::rtps::c_SequenceNumber_Unknown;
}

bool FillCorrelationToken(
    const eprosima::fastdds::rtps::SampleIdentity& request_identity,
    const eprosima::fastdds::rtps::SampleIdentity& related_identity,
    eprosima::fastdds::rtps::SampleIdentity& correlation_token) noexcept {
  if (!IdentityValid(request_identity) ||
      related_identity.writer_guid() == eprosima::fastdds::rtps::GUID_t{} ||
      !related_identity.writer_guid().entityId.is_reader() ||
      related_identity.writer_guid() == request_identity.writer_guid()) {
    return false;
  }
  correlation_token = related_identity;
  if (correlation_token.sequence_number() ==
      eprosima::fastdds::rtps::c_SequenceNumber_Unknown) {
    correlation_token.sequence_number(request_identity.sequence_number());
    return true;
  }
  return correlation_token.sequence_number() == request_identity.sequence_number();
}

std::string StableEndpointId(std::string_view value) {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char character : value) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << hash;
  return output.str();
}

enum class RequesterMatch { kUnmatched,
                            kPartial,
                            kMatched };

RequesterMatch RequesterMatchState(
    eprosima::fastdds::dds::DataReader* request_reader,
    eprosima::fastdds::dds::DataWriter* response_writer,
    const eprosima::fastdds::rtps::SampleIdentity& request_identity,
    const eprosima::fastdds::rtps::GUID_t& response_reader_guid) {
  eprosima::fastdds::dds::PublicationBuiltinTopicData publication;
  if (request_reader == nullptr || response_writer == nullptr ||
      request_reader->get_matched_publication_data(
          publication, request_identity.writer_guid()) !=
          eprosima::fastdds::dds::RETCODE_OK) {
    return RequesterMatch::kUnmatched;
  }
  eprosima::fastdds::dds::SubscriptionBuiltinTopicData subscription;
  return response_writer->get_matched_subscription_data(
             subscription, response_reader_guid) ==
                 eprosima::fastdds::dds::RETCODE_OK
             ? RequesterMatch::kMatched
             : RequesterMatch::kPartial;
}
}  // namespace

struct DdsChannelSubscriptionState {
  std::string topic;
  std::string msg_type;
  runtime::core::channel::SubscribeTool subscribe_tool;
  runtime::core::channel::LoanedSubscribeTool loaned_subscribe_tool;
  const runtime::core::channel::SubscribeWrapper* ordinary_subscriber = nullptr;
  bool has_loaned_subscriber = false;
  bool wrapper = false;
  std::string serialization_type = "dds_xcdr2";
  std::atomic<eprosima::fastdds::dds::DataReader*> reader = nullptr;
  std::shared_ptr<DdsReaderDrainState> drain_state;
  std::weak_ptr<std::atomic_bool> backend_running;
  std::shared_ptr<DdsDiagnostics> diagnostics;
#if defined(BUILD_TESTING)
  std::atomic_uint64_t returned_loans = 0;
  std::atomic_int32_t* forced_reader_take_result = nullptr;
#endif

  DdsReaderDrainState::DrainResult DrainOne() {
    using namespace eprosima::fastdds::dds;
    using DrainResult = DdsReaderDrainState::DrainResult;
    const auto running = backend_running.lock();
    if (!running || !running->load()) return DrainResult::kNoData;
    auto* current_reader = reader.load();
    if (current_reader == nullptr) return DrainResult::kNoData;
    const auto make_context = [this] {
      auto context = std::make_shared<aimrt::channel::Context>(
          aimrt_channel_context_type_t::AIMRT_CHANNEL_SUBSCRIBER_CONTEXT);
      context->SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_BACKEND, "dds");
      context->SetSerializationType(serialization_type);
      return context;
    };

    if (!has_loaned_subscriber) {
      if (ordinary_subscriber == nullptr) return DrainResult::kNoData;
      if (wrapper) {
        aimrt::dds::SerializedMessage wire_message;
        SampleInfo sample_info;
        const auto result =
            current_reader->take_next_sample(&wire_message, &sample_info);
        if (result == RETCODE_NO_DATA) return DrainResult::kNoData;
        if (result != RETCODE_OK) {
          const auto count = diagnostics->RecordReaderTakeFailure();
          if (ShouldLogRateLimited(count)) {
            AIMRT_ERROR(
                "aimrt_dds_reader_take_failure_total={} topic='{}' type='{}' return_code={}",
                count, topic, msg_type, static_cast<int32_t>(result));
          }
          return DrainResult::kRetryLater;
        }
        if (!sample_info.valid_data) return DrainResult::kConsumed;

        const auto validation = ValidateSerializedMessage(
            wire_message,
            SerializedMessageValidationOptions{
                .usage = SerializedMessageUsage::kChannel,
                .expected_type_name = msg_type,
                .expected_serialization_type = serialization_type});
        if (!validation.ok) {
          const auto count = diagnostics->RecordReaderTakeFailure();
          if (ShouldLogRateLimited(count)) {
            AIMRT_ERROR(
                "aimrt_dds_wrapper_invalid_total={} topic='{}' type='{}' reason='{}'",
                count, topic, msg_type, validation.reason);
          }
          return DrainResult::kConsumed;
        }

        auto message = ordinary_subscriber->info.msg_type_support_ref.CreateSharedPtr();
        if (!message) {
          const auto count = diagnostics->RecordReaderTakeFailure();
          if (ShouldLogRateLimited(count)) {
            AIMRT_ERROR(
                "aimrt_dds_wrapper_invalid_total={} topic='{}' type='{}' reason='create_failed'",
                count, topic, msg_type);
          }
          return DrainResult::kConsumed;
        }
        const aimrt::util::BufferArrayView payload(
            wire_message.data().data(), wire_message.data().size());
        if (!ordinary_subscriber->info.msg_type_support_ref.Deserialize(
                serialization_type, *payload.NativeHandle(), message.get())) {
          const auto count = diagnostics->RecordReaderTakeFailure();
          if (ShouldLogRateLimited(count)) {
            AIMRT_ERROR(
                "aimrt_dds_wrapper_invalid_total={} topic='{}' type='{}' reason='payload_deserialize_failed'",
                count, topic, msg_type);
          }
          return DrainResult::kConsumed;
        }

        auto context = make_context();
        for (const auto& entry : wire_message.metadata()) {
          context->SetMetaValue(
              std::string_view(entry.key().c_str(), entry.key().size()),
              std::string_view(entry.value().c_str(), entry.value().size()));
        }
        context->SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_BACKEND, "dds");
        context->SetSerializationType(serialization_type);
        subscribe_tool.DoSubscribeCallback(context, *ordinary_subscriber,
                                           message);
        return DrainResult::kConsumed;
      }
      auto message = ordinary_subscriber->info.msg_type_support_ref.CreateSharedPtr();
      if (!message) {
        const auto count = diagnostics->RecordReaderTakeFailure();
        if (ShouldLogRateLimited(count)) {
          AIMRT_ERROR(
              "aimrt_dds_reader_take_failure_total={} topic='{}' type='{}' reason='create_failed'",
              count, topic, msg_type);
        }
        return DrainResult::kRetryLater;
      }
      SampleInfo sample_info;
      const auto result = current_reader->take_next_sample(message.get(), &sample_info);
      if (result == RETCODE_NO_DATA) return DrainResult::kNoData;
      if (result != RETCODE_OK) {
        const auto count = diagnostics->RecordReaderTakeFailure();
        if (ShouldLogRateLimited(count)) {
          AIMRT_ERROR(
              "aimrt_dds_reader_take_failure_total={} topic='{}' type='{}' return_code={}",
              count, topic, msg_type, static_cast<int32_t>(result));
        }
        return DrainResult::kRetryLater;
      }
      if (!sample_info.valid_data) return DrainResult::kConsumed;
      subscribe_tool.DoSubscribeCallback(make_context(), *ordinary_subscriber, message);
      return DrainResult::kConsumed;
    }

    LoanableSequence<void*> samples;
    SampleInfoSeq sample_infos;
    auto result = RETCODE_OK;
#if defined(BUILD_TESTING)
    result = ReturnCode_t(forced_reader_take_result->load());
    if (result == RETCODE_OK)
#endif
      result = current_reader->take(samples, sample_infos, 1);
    if (result == RETCODE_NO_DATA) return DrainResult::kNoData;
    if (result != RETCODE_OK || samples.length() == 0 || sample_infos.length() == 0) {
      const auto count = diagnostics->RecordReaderTakeFailure();
      if (ShouldLogRateLimited(count)) {
        AIMRT_ERROR(
            "aimrt_dds_reader_take_failure_total={} topic='{}' type='{}' return_code={}",
            count, topic, msg_type, static_cast<int32_t>(result));
      }
      return DrainResult::kRetryTemporaryLoanFailure;
    }

    struct LoanGuard {
      DataReader* reader;
      LoanableSequence<void*>& samples;
      SampleInfoSeq& infos;
      DdsChannelSubscriptionState* owner;
      ~LoanGuard() {
        const auto result = reader->return_loan(samples, infos);
        if (result != RETCODE_OK) {
          const auto count = owner->diagnostics->RecordReaderTakeFailure();
          AIMRT_ERROR(
              "aimrt_dds_reader_take_failure_total={} topic='{}' type='{}' "
              "reason='return_loan' return_code={}",
              count, owner->topic, owner->msg_type, static_cast<int32_t>(result));
          return;
        }
#if defined(BUILD_TESTING)
        ++owner->returned_loans;
#endif
      }
    } loan_guard{current_reader, samples, sample_infos, this};

    const auto* sample = samples.buffer()[0];
    const auto& sample_info = sample_infos[0];
    if (!sample_info.valid_data || sample == nullptr) return DrainResult::kConsumed;
    auto context = make_context();
    loaned_subscribe_tool.DoSubscribeCallback(context, sample);

    if (ordinary_subscriber != nullptr) {
      auto message = ordinary_subscriber->info.msg_type_support_ref.CreateSharedPtr();
      if (!message) return DrainResult::kConsumed;
      ordinary_subscriber->info.msg_type_support_ref.Copy(sample, message.get());
      subscribe_tool.DoSubscribeCallback(context, *ordinary_subscriber, message);
    }
    return DrainResult::kConsumed;
  }

  bool HasUnread() const {
    const auto running = backend_running.lock();
    if (!running || !running->load()) return false;
    const auto* current_reader = reader.load();
    return current_reader != nullptr && current_reader->get_unread_count() != 0;
  }
};

struct DdsLoanedPublisherState {
  eprosima::fastdds::dds::DataWriter* writer = nullptr;
  DdsEndpointManager* endpoints = nullptr;
  std::weak_ptr<std::atomic_bool> backend_running;
  std::string topic;
  std::string msg_type;
#if defined(BUILD_TESTING)
  std::atomic_int32_t* forced_write_result = nullptr;
  std::atomic_int32_t* forced_loan_sample_result = nullptr;
  std::atomic_int32_t* forced_discard_loan_result = nullptr;
  std::atomic_uint64_t* loan_sample_calls = nullptr;
  std::atomic_uint64_t* discard_loan_calls = nullptr;
  std::mutex* hook_mutex = nullptr;
  std::function<void()>* before_loan_sample_hook = nullptr;
  std::function<void()>* before_loaned_write_hook = nullptr;
  std::function<void()>* before_discard_loan_hook = nullptr;
#endif
};

#if defined(BUILD_TESTING)
void RunLoanHook(std::mutex* mutex, std::function<void()>* hook) {
  std::function<void()> copy;
  {
    std::lock_guard lock(*mutex);
    copy = *hook;
  }
  if (copy) copy();
}
#endif

DdsChannelBackend::DdsChannelBackend(std::shared_ptr<DdsBackendState> state)
    : state_(std::move(state)) {}

DdsChannelBackend::~DdsChannelBackend() = default;

std::string DdsChannelBackend::EndpointKey(std::string_view topic, std::string_view msg_type) {
  return std::string(topic) + "\n" + std::string(msg_type);
}

void DdsChannelBackend::Initialize(YAML::Node options_node) {
  ValidateBackendOptions(options_node, "Channel");
  if (state_->participant_context.Participant() == nullptr) {
    throw std::logic_error("DDS Channel backend requires the process participant to be initialized");
  }
  if (state_->runtime.State() == DdsRuntimeState::kStopping) {
    throw std::logic_error("DDS Channel backend cannot initialize while runtime is stopping");
  }
}

void DdsChannelBackend::Start() {
  running_->store(true);
  std::lock_guard lock(mutex_);
  for (const auto& [_, subscription] : subscriptions_) {
    if (subscription->drain_state) subscription->drain_state->NotifyData();
  }
}
void DdsChannelBackend::Shutdown() { running_->store(false); }

std::list<std::pair<std::string, std::string>> DdsChannelBackend::GenInitializationReport() const noexcept {
  std::scoped_lock lock(state_->mutex, mutex_);
  std::list<std::pair<std::string, std::string>> report{
      {"DDS Channel Backend", "registered=true, writers=" +
                                  std::to_string(writers_.size()) + ", readers=" +
                                  std::to_string(subscriptions_.size()) + ", registrations=" +
                                  std::to_string(state_->channel_registrations)},
      {"DDS SerializedMessage Bounds",
       "type_name=256B, serialization_type=32B, metadata=64, key=256B, "
       "value=4096B, data=16777216B"}};
  for (const auto& [key, capability] : writer_loan_capabilities_) {
    const auto separator = key.find('\n');
    const auto topic = key.substr(0, separator);
    const auto type = separator == std::string::npos ? std::string() : key.substr(separator + 1);
    report.emplace_back(
        "DDS Channel Loan Endpoint",
        "topic=" + topic + ", type=" + type +
            ", representation=" + capability.representation +
            ", capable=" + (capability.can_loan ? "true" : "false") +
            ", reason=" + capability.reason);
  }
  return report;
}

bool DdsChannelBackend::RegisterPublishType(
    const runtime::core::channel::PublishTypeWrapper& wrapper) noexcept {
  try {
    if (running_->load()) throw std::logic_error("DDS Channel publish registration requires Init state");
    const auto& info = wrapper.info;
    auto wire_type = ResolveChannelWireType(info, "Channel publisher");
    auto type_support = wire_type.type_support;
    const auto key = EndpointKey(info.topic_name, info.msg_type);
    std::scoped_lock lock(state_->mutex, mutex_);
    state_->names.RegisterChannel(
        info.topic_name,
        "Channel topic '" + info.topic_name + "' type '" + info.msg_type + "'");
    auto* writer = state_->runtime.Endpoints().GetOrCreateWriter(
        info.topic_name, type_support, state_->participant_context.Qos().channel_writer,
        "Channel publisher module='" + info.module_name + "' pkg='" + info.pkg_path + "'");
    if (const auto existing = writers_.find(key);
        existing != writers_.end() && existing->second != writer) {
      throw std::logic_error("DDS Channel writer aggregation returned inconsistent endpoint");
    }
    writers_[key] = writer;
    eprosima::fastdds::dds::DataWriterQos effective_qos;
    const auto qos_result = writer->get_qos(effective_qos);
    if (qos_result != eprosima::fastdds::dds::RETCODE_OK) {
      throw std::runtime_error("DDS Channel cannot read effective writer QoS for loan capability");
    }
    const auto& representations = effective_qos.representation().m_value;
    const auto representation =
        representations.empty() ||
                representations.front() ==
                    eprosima::fastdds::dds::XCDR_DATA_REPRESENTATION
            ? eprosima::fastdds::dds::XCDR_DATA_REPRESENTATION
            : eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION;
    const bool can_loan =
        !wire_type.wrapper && type_support->is_plain(representation);
    const WriterLoanCapability capability{
        .can_loan = can_loan,
        .representation =
            representation == eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION
                ? "xcdr2"
                : "xcdr1",
        .reason = can_loan
                      ? "plain_type_and_effective_endpoint_configuration"
                      : (wire_type.wrapper
                             ? "serialized_message_wrapper_never_supports_loan"
                             : "type_not_plain_for_effective_representation")};
    if (const auto existing_capability = writer_loan_capabilities_.find(key);
        existing_capability != writer_loan_capabilities_.end() &&
        (existing_capability->second.can_loan != capability.can_loan ||
         existing_capability->second.representation != capability.representation)) {
      throw std::logic_error("DDS Channel writer loan capability changed for shared endpoint");
    }
    writer_loan_capabilities_[key] = capability;
    state_->type_support_handles.emplace_back(std::move(type_support));
    ++state_->channel_registrations;
    return true;
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS Channel publish registration rejected: {}", error.what());
    return false;
  }
}

bool DdsChannelBackend::Subscribe(const runtime::core::channel::SubscribeWrapper& wrapper) noexcept {
  try {
    if (running_->load()) throw std::logic_error("DDS Channel subscription requires Init state");
    const auto& info = wrapper.info;
    auto wire_type = ResolveChannelWireType(info, "Channel subscriber");
    auto type_support = wire_type.type_support;
    const auto key = EndpointKey(info.topic_name, info.msg_type);
    std::scoped_lock lock(state_->mutex, mutex_);
    state_->names.RegisterChannel(
        info.topic_name,
        "Channel topic '" + info.topic_name + "' type '" + info.msg_type + "'");
    if (const auto existing = subscriptions_.find(key); existing != subscriptions_.end()) {
      existing->second->subscribe_tool.AddSubscribeWrapper(&wrapper);
      if (existing->second->ordinary_subscriber == nullptr)
        existing->second->ordinary_subscriber = &wrapper;
      ++state_->channel_registrations;
      return true;
    }

    auto subscription = std::make_shared<DdsChannelSubscriptionState>();
    subscription->topic = info.topic_name;
    subscription->msg_type = info.msg_type;
    subscription->wrapper = wire_type.wrapper;
    subscription->serialization_type = wire_type.serialization_type;
    subscription->diagnostics = state_->runtime.Diagnostics();
    subscription->backend_running = running_;
#if defined(BUILD_TESTING)
    subscription->forced_reader_take_result = &forced_reader_take_result_;
#endif
    subscription->subscribe_tool.AddSubscribeWrapper(&wrapper);
    subscription->ordinary_subscriber = &wrapper;
    std::weak_ptr<DdsChannelSubscriptionState> weak_subscription = subscription;
    auto endpoint = state_->runtime.Endpoints().GetOrCreateReader(
        info.topic_name, type_support, state_->participant_context.Qos().channel_reader,
        "Channel subscriber module='" + info.module_name + "' pkg='" + info.pkg_path + "'",
        [weak_subscription] {
          const auto locked = weak_subscription.lock();
          return locked ? locked->DrainOne()
                        : DdsReaderDrainState::DrainResult::kNoData;
        },
        [weak_subscription] {
          const auto locked = weak_subscription.lock();
          return locked && locked->HasUnread();
        });
    subscription->reader = endpoint.reader;
    subscription->drain_state = endpoint.drain_state;
    subscriptions_.emplace(key, std::move(subscription));
    state_->type_support_handles.emplace_back(std::move(type_support));
    ++state_->channel_registrations;
    return true;
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS Channel subscription rejected: {}", error.what());
    return false;
  }
}

aimrt_channel_loan_status_t DdsChannelBackend::PrepareLoanedPublisher(
    const runtime::core::channel::PublishTypeWrapper& wrapper,
    runtime::core::channel::BackendLoanedPublisher& loaned_publisher) noexcept {
  loaned_publisher = {};
  try {
    if (!running_->load()) return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;
    if (!wrapper.info.msg_type.starts_with("dds:"))
      return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE;
    const auto key = EndpointKey(wrapper.info.topic_name, wrapper.info.msg_type);
    std::lock_guard lock(mutex_);
    const auto writer_itr = writers_.find(key);
    const auto capability_itr = writer_loan_capabilities_.find(key);
    if (writer_itr == writers_.end() || capability_itr == writer_loan_capabilities_.end())
      return AIMRT_CHANNEL_LOAN_STATUS_UNREGISTERED_MESSAGE_TYPE;
    if (!capability_itr->second.can_loan)
      return AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN;
    if (const auto route_itr = loan_routes_.find(key); route_itr != loan_routes_.end()) {
      loaned_publisher.impl = route_itr->second;
      loaned_publisher.borrow = &BorrowLoanedMessage;
      loaned_publisher.publish = &PublishLoanedMessage;
      return AIMRT_CHANNEL_LOAN_STATUS_OK;
    }
    auto route = std::make_unique<DdsLoanedPublisherState>();
    route->writer = writer_itr->second;
    route->endpoints = &state_->runtime.Endpoints();
    route->backend_running = running_;
    route->topic = wrapper.info.topic_name;
    route->msg_type = wrapper.info.msg_type;
#if defined(BUILD_TESTING)
    route->forced_write_result = &forced_write_result_;
    route->forced_loan_sample_result = &forced_loan_sample_result_;
    route->forced_discard_loan_result = &forced_discard_loan_result_;
    route->loan_sample_calls = &loan_sample_calls_;
    route->discard_loan_calls = &discard_loan_calls_;
    route->hook_mutex = &loan_hook_mutex_;
    route->before_loan_sample_hook = &before_loan_sample_hook_;
    route->before_loaned_write_hook = &before_loaned_write_hook_;
    route->before_discard_loan_hook = &before_discard_loan_hook_;
#endif
    auto* route_ptr = route.get();
    loan_route_storage_.emplace_back(std::move(route));
    loan_routes_.emplace(key, route_ptr);
    loaned_publisher.impl = route_ptr;
    loaned_publisher.borrow = &BorrowLoanedMessage;
    loaned_publisher.publish = &PublishLoanedMessage;
    return AIMRT_CHANNEL_LOAN_STATUS_OK;
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS Channel loan publisher preparation failed: {}", error.what());
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  } catch (...) {
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  }
}

aimrt_channel_loan_status_t DdsChannelBackend::BorrowLoanedMessage(
    void* impl, aimrt_channel_loaned_message_base_t& output) noexcept {
  output = {};
  auto* route = static_cast<DdsLoanedPublisherState*>(impl);
  if (route == nullptr || route->writer == nullptr || route->endpoints == nullptr)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  auto writer_operation = route->endpoints->AcquireWriterOperation();
  if (!writer_operation) return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;
  const auto running = route->backend_running.lock();
  if (!running || !running->load()) return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;
  void* sample = nullptr;
  auto result = eprosima::fastdds::dds::RETCODE_OK;
#if defined(BUILD_TESTING)
  RunLoanHook(route->hook_mutex, route->before_loan_sample_hook);
  ++(*route->loan_sample_calls);
  result = eprosima::fastdds::dds::ReturnCode_t(route->forced_loan_sample_result->load());
  if (result == eprosima::fastdds::dds::RETCODE_OK)
#endif
    result = route->writer->loan_sample(sample);
  if (result == eprosima::fastdds::dds::RETCODE_OK && sample != nullptr) {
    if (writer_operation.Stopping() || !running->load()) {
      const auto discard_result = route->writer->discard_loan(sample);
      if (discard_result != eprosima::fastdds::dds::RETCODE_OK &&
          discard_result != eprosima::fastdds::dds::RETCODE_BAD_PARAMETER) {
        AIMRT_FATAL(
            "DDS Channel could not return a newly borrowed sample while stopping "
            "topic='{}' type='{}' return_code={}",
            route->topic, route->msg_type,
            static_cast<int32_t>(discard_result));
        std::terminate();
      }
      return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;
    }
    output.msg_ptr = sample;
    output.impl = route;
    output.release = &ReleaseLoanedMessage;
    return AIMRT_CHANNEL_LOAN_STATUS_OK;
  }
  if (result == eprosima::fastdds::dds::RETCODE_OUT_OF_RESOURCES)
    return AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE;
  if (result == eprosima::fastdds::dds::RETCODE_ILLEGAL_OPERATION) {
    AIMRT_ERROR("DDS Channel writer loan capability inconsistent at runtime topic='{}' type='{}'",
                route->topic, route->msg_type);
    return AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN;
  }
  return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
}

aimrt_channel_loan_status_t DdsChannelBackend::ReleaseLoanedMessage(
    void* impl, void* msg_ptr) noexcept {
  auto* route = static_cast<DdsLoanedPublisherState*>(impl);
  if (route == nullptr || route->writer == nullptr || route->endpoints == nullptr ||
      msg_ptr == nullptr)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  auto writer_operation = route->endpoints->AcquireWriterOperation();
  if (!writer_operation) return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;
  void* sample = msg_ptr;
  auto result = eprosima::fastdds::dds::RETCODE_OK;
#if defined(BUILD_TESTING)
  RunLoanHook(route->hook_mutex, route->before_discard_loan_hook);
  ++(*route->discard_loan_calls);
  result = eprosima::fastdds::dds::ReturnCode_t(route->forced_discard_loan_result->load());
  if (result == eprosima::fastdds::dds::RETCODE_OK)
#endif
    result = route->writer->discard_loan(sample);
  if (result == eprosima::fastdds::dds::RETCODE_OK)
    return AIMRT_CHANNEL_LOAN_STATUS_OK;
  if (result == eprosima::fastdds::dds::RETCODE_BAD_PARAMETER) {
    AIMRT_ERROR("DDS Channel discard_loan failed topic='{}' type='{}' return_code={}",
                route->topic, route->msg_type, static_cast<int32_t>(result));
    return AIMRT_CHANNEL_LOAN_STATUS_OK;
  }
  AIMRT_ERROR("DDS Channel discard_loan retained ownership topic='{}' type='{}' return_code={}",
              route->topic, route->msg_type, static_cast<int32_t>(result));
  return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
}

aimrt_channel_loan_status_t DdsChannelBackend::PublishLoanedMessage(
    void* impl, aimrt::channel::ContextRef, aimrt_channel_loaned_message_base_t& loaned_msg) noexcept {
  auto* route = static_cast<DdsLoanedPublisherState*>(impl);
  if (route == nullptr || route->writer == nullptr || route->endpoints == nullptr ||
      loaned_msg.msg_ptr == nullptr || loaned_msg.impl != route ||
      loaned_msg.release != &ReleaseLoanedMessage)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  auto writer_operation = route->endpoints->AcquireWriterOperation();
  if (!writer_operation) return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;
  const auto running = route->backend_running.lock();
  if (!running || !running->load()) return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;
  void* sample = loaned_msg.msg_ptr;
  auto result = eprosima::fastdds::dds::RETCODE_OK;
#if defined(BUILD_TESTING)
  RunLoanHook(route->hook_mutex, route->before_loaned_write_hook);
  result = eprosima::fastdds::dds::ReturnCode_t(route->forced_write_result->load());
  if (result == eprosima::fastdds::dds::RETCODE_OK)
#endif
    result = route->writer->write(sample);
  if (result == eprosima::fastdds::dds::RETCODE_OK) {
    loaned_msg = {};
    return AIMRT_CHANNEL_LOAN_STATUS_OK;
  }
  if (result == eprosima::fastdds::dds::RETCODE_OUT_OF_RESOURCES) {
    loaned_msg = {};
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  }
  if (result == eprosima::fastdds::dds::RETCODE_ILLEGAL_OPERATION) {
    AIMRT_ERROR("DDS Channel writer loan publish capability inconsistent at runtime topic='{}' type='{}'",
                route->topic, route->msg_type);
    return AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN;
  }
  return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
}

aimrt_channel_loan_status_t DdsChannelBackend::SubscribeLoaned(
    const runtime::core::channel::LoanedSubscribeWrapper& wrapper) noexcept {
  try {
    if (running_->load()) throw std::logic_error("DDS Channel loan subscription requires Init state");
    const auto& info = wrapper.info;
    if (!info.msg_type.starts_with("dds:"))
      return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE;
    auto type_support = CopyMessageTypeSupport(
        info.msg_type_support_ref, info.topic_name, "Channel loan subscriber");
    const auto key = EndpointKey(info.topic_name, info.msg_type);
    std::scoped_lock lock(state_->mutex, mutex_);
    state_->names.RegisterChannel(
        info.topic_name,
        "Channel topic '" + info.topic_name + "' type '" + info.msg_type + "'");
    if (const auto existing = subscriptions_.find(key); existing != subscriptions_.end()) {
      existing->second->loaned_subscribe_tool.AddSubscribeWrapper(&wrapper);
      existing->second->has_loaned_subscriber = true;
      ++state_->channel_registrations;
      return AIMRT_CHANNEL_LOAN_STATUS_OK;
    }

    auto subscription = std::make_shared<DdsChannelSubscriptionState>();
    subscription->topic = info.topic_name;
    subscription->msg_type = info.msg_type;
    subscription->diagnostics = state_->runtime.Diagnostics();
    subscription->backend_running = running_;
#if defined(BUILD_TESTING)
    subscription->forced_reader_take_result = &forced_reader_take_result_;
#endif
    subscription->loaned_subscribe_tool.AddSubscribeWrapper(&wrapper);
    subscription->has_loaned_subscriber = true;
    std::weak_ptr<DdsChannelSubscriptionState> weak_subscription = subscription;
    auto endpoint = state_->runtime.Endpoints().GetOrCreateReader(
        info.topic_name, type_support, state_->participant_context.Qos().channel_reader,
        "Channel loan subscriber module='" + info.module_name + "' pkg='" + info.pkg_path + "'",
        [weak_subscription] {
          const auto locked = weak_subscription.lock();
          return locked ? locked->DrainOne()
                        : DdsReaderDrainState::DrainResult::kNoData;
        },
        [weak_subscription] {
          const auto locked = weak_subscription.lock();
          return locked && locked->HasUnread();
        });
    subscription->reader = endpoint.reader;
    subscription->drain_state = endpoint.drain_state;
    subscriptions_.emplace(key, std::move(subscription));
    state_->type_support_handles.emplace_back(std::move(type_support));
    ++state_->channel_registrations;
    return AIMRT_CHANNEL_LOAN_STATUS_OK;
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS Channel loan subscription rejected: {}", error.what());
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  } catch (...) {
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  }
}

void DdsChannelBackend::Publish(runtime::core::channel::MsgWrapper& wrapper) noexcept {
  try {
    if (!running_->load()) {
      const auto count = state_->runtime.Diagnostics()->RecordChannelWriteFailure();
      if (ShouldLogRateLimited(count)) {
        AIMRT_ERROR("aimrt_dds_channel_write_failure_total={} reason='not_running'", count);
      }
      return;
    }
    const auto& info = wrapper.info;
    const auto wrapper_serialization = WrapperSerializationType(info.msg_type);
    const bool is_wrapper = !wrapper_serialization.empty();
    auto serialization = wrapper.ctx_ref
                             ? wrapper.ctx_ref.GetSerializationType()
                             : std::string_view{};
    if (serialization.empty())
      serialization = is_wrapper ? wrapper_serialization : "dds_xcdr2";
    if ((!is_wrapper && serialization != "dds_xcdr2") ||
        (is_wrapper && serialization != wrapper_serialization)) {
      throw std::invalid_argument(
          "DDS Channel serialization_type does not match the registered message adapter");
    }
    if (!is_wrapper && !runtime::core::channel::TryCheckMsg(wrapper)) {
      throw std::invalid_argument(
          "DDS native Channel message is null or cannot be deserialized");
    }
    auto writer_operation = state_->runtime.Endpoints().AcquireWriterOperation();
    if (!writer_operation) {
      throw std::logic_error("DDS Channel publish rejected while runtime is stopping");
    }
    eprosima::fastdds::dds::DataWriter* writer = nullptr;
    {
      std::lock_guard lock(mutex_);
      const auto iterator = writers_.find(EndpointKey(info.topic_name, info.msg_type));
      if (iterator == writers_.end()) {
        throw std::invalid_argument("DDS Channel publisher endpoint is not registered");
      }
      writer = iterator->second;
    }
    aimrt::dds::SerializedMessage wire_message;
    const void* write_message = wrapper.msg_ptr;
    if (is_wrapper) {
      if (!WrapperAdapterEnabled(info.msg_type)) {
        throw std::invalid_argument("DDS optional wrapper adapter is disabled");
      }
      std::vector<SerializedMetadataView> metadata;
      if (wrapper.ctx_ref) {
        const auto [values, size] = wrapper.ctx_ref.GetMetaKeyValsArray();
        if (size % 2 != 0)
          throw std::invalid_argument("DDS Channel context metadata is malformed");
        const size_t entry_count = size / 2;
        if (entry_count > kSerializedMessageMetadataMaxEntries)
          throw std::invalid_argument("metadata exceeds 64 entries");
        for (size_t index = 0; index < size; index += 2) {
          const auto key = aimrt::util::ToStdStringView(values[index]);
          const auto value = aimrt::util::ToStdStringView(values[index + 1]);
          if (key.size() > kSerializedMessageKeyMaxBytes)
            throw std::invalid_argument("metadata key exceeds 256 UTF-8 bytes");
          if (value.size() > kSerializedMessageValueMaxBytes)
            throw std::invalid_argument("metadata value exceeds 4096 UTF-8 bytes");
        }
        metadata.reserve(entry_count);
        for (size_t index = 0; index < size; index += 2) {
          metadata.emplace_back(
              aimrt::util::ToStdStringView(values[index]),
              aimrt::util::ToStdStringView(values[index + 1]));
        }
      }
      auto validation = ValidateSerializedMessageSemantics(
          info.msg_type, serialization, metadata, 0,
          SerializedMessageValidationOptions{
              .usage = SerializedMessageUsage::kChannel,
              .expected_type_name = info.msg_type,
              .expected_serialization_type = wrapper_serialization});
      if (!validation.ok) throw std::invalid_argument(validation.reason);

      auto payload = SerializeWrapperPayloadWithCache(wrapper, serialization);
      validation = ValidateSerializedMessageSemantics(
          info.msg_type, serialization, metadata, payload->BufferSize(),
          SerializedMessageValidationOptions{
              .usage = SerializedMessageUsage::kChannel,
              .expected_type_name = info.msg_type,
              .expected_serialization_type = wrapper_serialization});
      if (!validation.ok) throw std::invalid_argument(validation.reason);

      wire_message.type_name(std::string(info.msg_type));
      wire_message.serialization_type(std::string(serialization));
      wire_message.metadata().reserve(metadata.size());
      for (const auto& item : metadata) {
        aimrt::dds::MetadataEntry entry;
        entry.key(std::string(item.key));
        entry.value(std::string(item.value));
        wire_message.metadata().emplace_back(std::move(entry));
      }
      wire_message.data().resize(payload->BufferSize());
      auto* output = wire_message.data().data();
      for (const auto& item :
           std::span(payload->Data(), payload->Size())) {
        std::memcpy(output, item.data, item.len);
        output += item.len;
      }
      write_message = &wire_message;
    }

#if defined(BUILD_TESTING)
    std::function<void()> before_write;
    {
      std::lock_guard lock(write_hook_mutex_);
      before_write = before_write_hook_;
    }
    if (before_write) before_write();
#endif
    auto result = eprosima::fastdds::dds::RETCODE_OK;
#if defined(BUILD_TESTING)
    result = eprosima::fastdds::dds::ReturnCode_t(forced_write_result_.load());
    if (result == eprosima::fastdds::dds::RETCODE_OK)
#endif
      result = writer->write(const_cast<void*>(write_message));
    if (result != eprosima::fastdds::dds::RETCODE_OK) {
      const auto count = state_->runtime.Diagnostics()->RecordChannelWriteFailure();
      if (ShouldLogRateLimited(count)) {
        AIMRT_ERROR(
            "aimrt_dds_channel_write_failure_total={} topic='{}' type='{}' return_code={}",
            count, info.topic_name, info.msg_type, static_cast<int32_t>(result));
      }
    }
  } catch (const std::exception& error) {
    const auto count = state_->runtime.Diagnostics()->RecordChannelWriteFailure();
    if (ShouldLogRateLimited(count)) {
      AIMRT_ERROR("aimrt_dds_channel_write_failure_total={} reason='{}'", count, error.what());
    }
  } catch (...) {
    const auto count = state_->runtime.Diagnostics()->RecordChannelWriteFailure();
    if (ShouldLogRateLimited(count)) {
      AIMRT_ERROR("aimrt_dds_channel_write_failure_total={} reason='unknown'", count);
    }
  }
}

#if defined(BUILD_TESTING)
void DdsChannelBackend::SetBeforeWriteHookForTesting(std::function<void()> hook) {
  std::lock_guard lock(write_hook_mutex_);
  before_write_hook_ = std::move(hook);
}

void DdsChannelBackend::SetBeforeLoanSampleHookForTesting(
    std::function<void()> hook) {
  std::lock_guard lock(loan_hook_mutex_);
  before_loan_sample_hook_ = std::move(hook);
}

void DdsChannelBackend::SetBeforeLoanedWriteHookForTesting(
    std::function<void()> hook) {
  std::lock_guard lock(loan_hook_mutex_);
  before_loaned_write_hook_ = std::move(hook);
}

void DdsChannelBackend::SetBeforeDiscardLoanHookForTesting(
    std::function<void()> hook) {
  std::lock_guard lock(loan_hook_mutex_);
  before_discard_loan_hook_ = std::move(hook);
}

uint64_t DdsChannelBackend::LoanReturnCountForTesting(
    std::string_view topic, std::string_view msg_type) const noexcept {
  std::lock_guard lock(mutex_);
  const auto iterator = subscriptions_.find(EndpointKey(topic, msg_type));
  return iterator == subscriptions_.end() ? 0 : iterator->second->returned_loans.load();
}

uint64_t DdsChannelBackend::ReaderRetryScheduleCountForTesting(
    std::string_view topic, std::string_view msg_type) const noexcept {
  std::lock_guard lock(mutex_);
  const auto iterator = subscriptions_.find(EndpointKey(topic, msg_type));
  return iterator == subscriptions_.end() || !iterator->second->drain_state
             ? 0
             : iterator->second->drain_state->RetryScheduleCount();
}

uint32_t DdsChannelBackend::ReaderLastRetryDelayMsForTesting(
    std::string_view topic, std::string_view msg_type) const noexcept {
  std::lock_guard lock(mutex_);
  const auto iterator = subscriptions_.find(EndpointKey(topic, msg_type));
  return iterator == subscriptions_.end() || !iterator->second->drain_state
             ? 0
             : iterator->second->drain_state->LastRetryDelayMs();
}

size_t DdsChannelBackend::ReaderRetryStepForTesting(
    std::string_view topic, std::string_view msg_type) const noexcept {
  std::lock_guard lock(mutex_);
  const auto iterator = subscriptions_.find(EndpointKey(topic, msg_type));
  return iterator == subscriptions_.end() || !iterator->second->drain_state
             ? 0
             : iterator->second->drain_state->RetryStep();
}

int64_t DdsChannelBackend::ReaderUnreadCountForTesting(
    std::string_view topic, std::string_view msg_type) const noexcept {
  std::lock_guard lock(mutex_);
  const auto iterator = subscriptions_.find(EndpointKey(topic, msg_type));
  if (iterator == subscriptions_.end()) return 0;
  const auto* reader = iterator->second->reader.load();
  return reader == nullptr ? 0 : reader->get_unread_count();
}
#endif

void DdsRpcBackend::Initialize(YAML::Node options_node) {
  ValidateBackendOptions(options_node, "RPC");
  if (state_->participant_context.Participant() == nullptr) {
    throw std::logic_error("DDS RPC backend requires the process participant to be initialized");
  }
  if (state_->runtime.State() == DdsRuntimeState::kStopping) {
    throw std::logic_error("DDS RPC backend cannot initialize while runtime is stopping");
  }
  running_token_->store(false);
}

void DdsRpcBackend::Start() {
  running_ = true;
  running_token_->store(true);
  state_->runtime.Endpoints().Start();
}

void DdsRpcBackend::Shutdown() {
  if (!running_.exchange(false)) return;
  running_token_->store(false);
#if defined(BUILD_TESTING)
  std::function<void()> after_shutdown_started;
  {
    std::lock_guard lock(callback_state_->shutdown_hook_mutex);
    after_shutdown_started = callback_state_->after_shutdown_started_hook;
  }
  if (after_shutdown_started) after_shutdown_started();
#endif
  std::vector<std::shared_ptr<ClientState>> clients;
  std::vector<std::shared_ptr<ServerState>> servers;
  {
    std::lock_guard lock(mutex_);
    for (auto& [_, client] : clients_) clients.emplace_back(client);
    for (auto& [_, server] : servers_) servers.emplace_back(server);
  }
  for (const auto& client : clients)
    if (client->drain) client->drain->Stop();
  for (const auto& server : servers)
    if (server->drain) server->drain->Stop();
  for (const auto& client : clients) {
    std::vector<std::shared_ptr<runtime::core::rpc::InvokeWrapper>> pending;
    {
      std::lock_guard publication_lock(client->publication_mutex);
      std::lock_guard lock(client->mutex);
      for (auto& [key, record] : client->pending) {
        pending.emplace_back(std::move(record.invoke));
        AddTerminalLocked(*client, key, record.request_identity,
                          TerminalReason::kShutdown);
      }
      client->pending.clear();
    }
    for (auto& invoke : pending)
      if (invoke && invoke->callback)
        runtime::core::rpc::InvokeCallBack(
            *invoke, aimrt::rpc::Status(AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR));
  }
}

std::list<std::pair<std::string, std::string>>
DdsRpcBackend::GenInitializationReport() const noexcept {
  std::list<std::pair<std::string, std::string>> report;
  {
    std::lock_guard lock(state_->mutex);
    report.emplace_back(
        "DDS RPC Backend",
        "registered=true, endpoints=rpc, registrations=" +
            std::to_string(state_->rpc_registrations));
    report.emplace_back("dds.rpc.request_prefix", "req/");
    report.emplace_back("dds.rpc.response_prefix", "rsp/");
    report.emplace_back("dds.rpc.response_filter",
                        "related_sample_identity.writer_guid.guidPrefix");
    report.emplace_back("dds.rpc.response_cft_ownership", "per_logical_client");
    report.emplace_back("dds.rpc.reply_target_mode",
                        "explicit_response_reader_guid_in_related_sample_identity");
    report.emplace_back("dds.rpc.related_entity_setters", "unsupported_not_called");
    report.emplace_back("dds.rpc.fastdds_compatibility",
                        "fastdds_3.6.2_dd66ef2a_explicit_identity");
    report.emplace_back("dds.rpc.default_qos", "RELIABLE/VOLATILE/KEEP_ALL");
    report.emplace_back("dds.rpc.behavior_reference_commit",
                        "ecf205b06387b138d41826cad444515f3981cc78");
    report.emplace_back("dds.rpc.reply_match_wait_seconds", "3");
    report.emplace_back("dds.rpc.pending_limit_per_client", "1024");
    report.emplace_back("dds.rpc.terminal_history_capacity_per_client", "1024");
    report.emplace_back("dds.rpc.multiserver_policy",
                        "broadcast_first_valid_response_wins");
    report.emplace_back("dds.wrapper.type_name_max_bytes",
                        std::to_string(kSerializedMessageTypeNameMaxBytes));
    report.emplace_back(
        "dds.wrapper.serialization_type_max_bytes",
        std::to_string(kSerializedMessageSerializationTypeMaxBytes));
    report.emplace_back("dds.wrapper.metadata_max_entries",
                        std::to_string(kSerializedMessageMetadataMaxEntries));
    report.emplace_back("dds.wrapper.metadata_key_max_bytes",
                        std::to_string(kSerializedMessageKeyMaxBytes));
    report.emplace_back("dds.wrapper.metadata_value_max_bytes",
                        std::to_string(kSerializedMessageValueMaxBytes));
    report.emplace_back("dds.wrapper.data_max_bytes",
                        std::to_string(kSerializedMessageDataMaxBytes));
    report.emplace_back("dds.rpc.registrations", std::to_string(state_->rpc_registrations));
  }
  {
    std::lock_guard lock(mutex_);
    report.emplace_back("dds.rpc.client_count", std::to_string(clients_.size()));
    for (const auto& [_, client] : clients_) {
      report.emplace_back("dds.rpc.client." + client->endpoint_id + ".response_cft_name",
                          client->response_cft_name);
      report.emplace_back(
          "dds.rpc.client." + client->endpoint_id + ".response_filter_expression",
          client->response_filter_expression);
      report.emplace_back(
          "dds.rpc.client." + client->endpoint_id + ".response_filter_signature",
          client->response_filter_signature);
      std::ostringstream guid;
      guid << client->response_reader_guid;
      report.emplace_back("dds.rpc.client." + client->endpoint_id +
                              ".response_reader_guid",
                          guid.str());
      report.emplace_back("dds.rpc.client." + client->endpoint_id +
                              ".request_writer_max_blocking_time_us",
                          "0");
      report.emplace_back("dds.rpc.client." + client->endpoint_id +
                              ".request_writer_max_blocking_time_source",
                          "fixed_rpc_zero");
    }
  }
  return report;
}

bool DdsRpcBackend::RegisterFunction(const runtime::core::rpc::FuncInfo& info) noexcept {
  try {
    if (info.custom_type_support_ptr != nullptr) {
      throw std::invalid_argument(
          "DDS RPC function-level custom TypeSupport must be null: function='" + info.func_name + "'");
    }
    ValidateDdsRpcQos(state_->participant_context.Qos(), info.func_name);
    const auto req_wrapper = WrapperSerializationType(info.req_type_support_ref.TypeName());
    const auto rsp_wrapper = WrapperSerializationType(info.rsp_type_support_ref.TypeName());
    if (req_wrapper != rsp_wrapper) {
      throw std::invalid_argument("DDS RPC request/response wrapper mismatch");
    }
    if (!req_wrapper.empty() &&
        (!WrapperAdapterEnabled(info.req_type_support_ref.TypeName()) ||
         !WrapperAdapterEnabled(info.rsp_type_support_ref.TypeName()))) {
      throw std::invalid_argument("DDS RPC optional wrapper adapter is disabled");
    }
    auto request = req_wrapper.empty()
                       ? CopyMessageTypeSupport(info.req_type_support_ref, info.func_name,
                                                "request")
                       : GetSerializedMessageTypeSupport();
    auto response = rsp_wrapper.empty()
                        ? CopyMessageTypeSupport(info.rsp_type_support_ref, info.func_name,
                                                 "response")
                        : GetSerializedMessageTypeSupport();
    {
      std::lock_guard lock(mutex_);
      const auto key = LogicalKey(info);
      if (!registered_functions_.insert(key).second) return true;
      std::lock_guard state_lock(state_->mutex);
      state_->names.RegisterRpc(info.func_name);
      state_->type_support_handles.emplace_back(request);
      state_->type_support_handles.emplace_back(response);
      ++state_->rpc_registrations;
    }
    return true;
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS RPC registration rejected: {}", error.what());
    return false;
  }
}

bool DdsRpcBackend::RegisterServiceFunc(
    const runtime::core::rpc::ServiceFuncWrapper& wrapper) noexcept {
  if (!RegisterFunction(wrapper.info)) return false;
  try {
    const auto key = LogicalKey(wrapper.info);
    {
      std::lock_guard lock(mutex_);
      if (servers_.contains(key)) {
        throw std::invalid_argument("DDS RPC server logical endpoint already exists");
      }
    }
    auto state = std::make_shared<ServerState>();
    state->info = wrapper.info;
    state->service_func = wrapper.service_func;
    const auto req_serialization =
        WrapperSerializationType(wrapper.info.req_type_support_ref.TypeName());
    const auto rsp_serialization =
        WrapperSerializationType(wrapper.info.rsp_type_support_ref.TypeName());
    state->request_type = req_serialization.empty()
                              ? CopyMessageTypeSupport(wrapper.info.req_type_support_ref,
                                                       wrapper.info.func_name, "request")
                              : GetSerializedMessageTypeSupport();
    state->response_type = rsp_serialization.empty()
                               ? CopyMessageTypeSupport(wrapper.info.rsp_type_support_ref,
                                                        wrapper.info.func_name, "response")
                               : GetSerializedMessageTypeSupport();
    state->wrapper = !req_serialization.empty();
    state->serialization_type = std::string(req_serialization);
    RegisterServerEndpoint(state);
    try {
      std::lock_guard lock(mutex_);
      servers_.emplace(key, state);
    } catch (...) {
      state_->runtime.Endpoints().DeleteWriter(state->response_writer);
      state_->runtime.Endpoints().DeleteReader(state->request_reader);
      throw;
    }
    return true;
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS RPC service endpoint registration failed: {}", error.what());
    return false;
  }
}

bool DdsRpcBackend::RegisterClientFunc(
    const runtime::core::rpc::ClientFuncWrapper& wrapper) noexcept {
  if (!RegisterFunction(wrapper.info)) return false;
  try {
    const auto key = LogicalKey(wrapper.info);
    {
      std::lock_guard lock(mutex_);
      if (clients_.contains(key)) {
        throw std::invalid_argument("DDS RPC client logical endpoint already exists");
      }
    }
    auto state = std::make_shared<ClientState>();
    state->info = wrapper.info;
    state->diagnostics = state_->runtime.Diagnostics();
    const auto req_serialization =
        WrapperSerializationType(wrapper.info.req_type_support_ref.TypeName());
    const auto rsp_serialization =
        WrapperSerializationType(wrapper.info.rsp_type_support_ref.TypeName());
    state->request_type = req_serialization.empty()
                              ? CopyMessageTypeSupport(wrapper.info.req_type_support_ref,
                                                       wrapper.info.func_name, "request")
                              : GetSerializedMessageTypeSupport();
    state->response_type = rsp_serialization.empty()
                               ? CopyMessageTypeSupport(wrapper.info.rsp_type_support_ref,
                                                        wrapper.info.func_name, "response")
                               : GetSerializedMessageTypeSupport();
    state->wrapper = !req_serialization.empty();
    state->serialization_type = std::string(req_serialization);
    RegisterClientEndpoint(state);
    try {
      std::lock_guard lock(mutex_);
      clients_.emplace(key, state);
    } catch (...) {
      state_->runtime.Endpoints().DeleteWriter(state->request_writer);
      state_->runtime.Endpoints().DeleteReader(state->response_reader);
      throw;
    }
    return true;
  } catch (const std::exception& error) {
    AIMRT_ERROR("DDS RPC client endpoint registration failed: {}", error.what());
    return false;
  }
}

void DdsRpcBackend::RegisterClientEndpoint(const std::shared_ptr<ClientState>& client) {
  const auto logical_key = LogicalKey(client->info);
  const auto names = DeriveDdsRpcTopicNames(client->info.func_name);
  client->endpoint_id = StableEndpointId(logical_key);
  client->response_cft_name = names.response + "/cft/" + client->endpoint_id;
  client->response_filter_signature = "aimrt_rpc_client_" + client->endpoint_id;
  client->response_filter_expression =
      "related_sample_identity.writer_guid().guidPrefix = %0 AND '" +
      client->response_filter_signature + "' = '" + client->response_filter_signature + "'";
  std::weak_ptr<ClientState> weak_client = client;
  try {
    auto endpoint = state_->runtime.Endpoints().GetOrCreateFilteredReader(
        names.response, client->response_type,
        state_->participant_context.Qos().rpc_reader,
        std::string("rpc client response reader ") + logical_key,
        client->response_cft_name, client->response_filter_expression, {"pending"},
        [callback_state = callback_state_, weak_client] {
          auto current = weak_client.lock();
          if (!current) return DdsReaderDrainState::DrainResult::kNoData;
          return DrainClient(callback_state, current);
        },
        [callback_state = callback_state_, weak_client] {
          if (!callback_state->running->load()) return false;
          auto current = weak_client.lock();
          return current && current->response_reader &&
                 current->response_reader->get_unread_count() != 0;
        });
    client->response_reader = endpoint.reader;
    client->drain = endpoint.drain_state;
    client->response_reader_guid = endpoint.reader->guid();
    std::ostringstream response_prefix;
    response_prefix << client->response_reader_guid.guidPrefix;
    const auto result = endpoint.filtered_topic->set_expression_parameters({response_prefix.str()});
    if (result != eprosima::fastdds::dds::RETCODE_OK) {
      throw std::runtime_error("Fast DDS response CFT parameter update failed");
    }
    auto request_writer_qos = state_->participant_context.Qos().rpc_writer;
    request_writer_qos.reliability().max_blocking_time =
        eprosima::fastdds::dds::Duration_t(0, 0);
    client->request_writer = state_->runtime.Endpoints().GetOrCreateWriter(
        names.request, client->request_type, request_writer_qos,
        std::string("rpc client request writer ") + logical_key,
        [weak_client](int32_t matched_servers) {
          if (auto current = weak_client.lock()) {
            ObserveMatchedServers(current, matched_servers);
          }
        });
    if (!state_->runtime.Endpoints().EnableReader(client->response_reader) ||
        !state_->runtime.Endpoints().EnableWriter(client->request_writer)) {
      throw std::runtime_error("DDS RPC client endpoint enable failed");
    }
  } catch (...) {
    state_->runtime.Endpoints().DeleteWriter(client->request_writer);
    client->request_writer = nullptr;
    state_->runtime.Endpoints().DeleteReader(client->response_reader);
    client->response_reader = nullptr;
    client->drain.reset();
    throw;
  }
}

void DdsRpcBackend::RegisterServerEndpoint(const std::shared_ptr<ServerState>& server) {
  const auto logical_key = LogicalKey(server->info);
  const auto names = DeriveDdsRpcTopicNames(server->info.func_name);
  std::weak_ptr<ServerState> weak_server = server;
  try {
    auto endpoint = state_->runtime.Endpoints().GetOrCreateReader(
        names.request, server->request_type,
        state_->participant_context.Qos().rpc_reader,
        std::string("rpc server request reader ") + logical_key,
        [callback_state = callback_state_, weak_server] {
          auto current = weak_server.lock();
          if (!current) return DdsReaderDrainState::DrainResult::kNoData;
          return DrainServer(callback_state, current);
        },
        [callback_state = callback_state_, weak_server] {
          if (!callback_state->running->load()) return false;
          auto current = weak_server.lock();
          return current && current->request_reader &&
                 current->request_reader->get_unread_count() != 0;
        });
    server->request_reader = endpoint.reader;
    server->drain = endpoint.drain_state;
    server->response_writer = state_->runtime.Endpoints().GetOrCreateWriter(
        names.response, server->response_type,
        state_->participant_context.Qos().rpc_writer,
        std::string("rpc server response writer ") + logical_key);
    if (!state_->runtime.Endpoints().EnableReader(server->request_reader) ||
        !state_->runtime.Endpoints().EnableWriter(server->response_writer)) {
      throw std::runtime_error("DDS RPC server endpoint enable failed");
    }
  } catch (...) {
    state_->runtime.Endpoints().DeleteWriter(server->response_writer);
    server->response_writer = nullptr;
    state_->runtime.Endpoints().DeleteReader(server->request_reader);
    server->request_reader = nullptr;
    server->drain.reset();
    throw;
  }
}

void DdsRpcBackend::AddTerminalLocked(ClientState& client, const std::string& key,
                                      const SampleIdentity& request_identity,
                                      TerminalReason reason) {
  if (client.terminal_history.contains(key)) return;
  if (client.terminal_history.size() >= ClientState::kTerminalHistoryLimit) {
    const auto evicted = std::move(client.terminal_order.front());
    client.terminal_order.pop_front();
    client.terminal_history.erase(evicted);
    const auto count = client.diagnostics->RecordRpcTerminalHistoryEvicted();
    if (ShouldLogRateLimited(count)) {
      AIMRT_WARN(
          "aimrt_dds_rpc_terminal_history_evicted_total={} function='{}' "
          "capacity={} policy=fifo",
          count, client.info.func_name, ClientState::kTerminalHistoryLimit);
    }
  }
  client.terminal_order.emplace_back(key);
  client.terminal_history.emplace(
      key, ClientState::Terminal{.request_identity = request_identity,
                                 .reason = reason});
}

void DdsRpcBackend::ObserveMatchedServers(
    const std::shared_ptr<ClientState>& client, int32_t matched_servers) {
  const auto previous = client->observed_matched_servers.exchange(matched_servers);
  if (previous == matched_servers) return;
  client->diagnostics->AdjustRpcMatchedServers(matched_servers - previous);
  const bool entered_multiple = previous < 2 && matched_servers >= 2;
  const bool changed_while_multiple = previous >= 2 && matched_servers >= 2;
  if (!entered_multiple && !changed_while_multiple) return;
  const auto count = entered_multiple
                         ? client->diagnostics->RecordRpcMultipleServers()
                         : client->diagnostics->Snapshot().rpc_multiple_servers_total;
  if (ShouldLogRateLimited(std::max<uint64_t>(1, count))) {
    const auto names = DeriveDdsRpcTopicNames(client->info.func_name);
    AIMRT_WARN(
        "event=dds_rpc_multiple_servers function='{}' request_topic='{}' "
        "response_topic='{}' matched_servers={} "
        "policy=broadcast_first_valid_response_wins "
        "risk=business_handler_may_execute_multiple_times",
        client->info.func_name, names.request, names.response, matched_servers);
  }
}

DdsReaderDrainState::DrainResult DdsRpcBackend::DrainClient(
    const std::shared_ptr<CallbackState>& callback_state,
    const std::shared_ptr<ClientState>& client) {
  using DrainResult = DdsReaderDrainState::DrainResult;
  if (!callback_state->running->load() || client->response_reader == nullptr) {
    return DrainResult::kNoData;
  }
  const auto record_invalid_response = [&](std::string_view reason) {
    const auto count = client->diagnostics->RecordRpcInvalidResponse();
    if (ShouldLogRateLimited(count)) {
      const auto names = DeriveDdsRpcTopicNames(client->info.func_name);
      AIMRT_WARN(
          "aimrt_dds_rpc_invalid_response_total={} role=rpc_response "
          "function='{}' topic='{}' type='{}' reason='{}' "
          "action=drop_keep_pending",
          count, client->info.func_name, names.response,
          client->info.rsp_type_support_ref.TypeName(), reason);
    }
  };
  std::shared_ptr<void> response =
      client->wrapper
          ? std::shared_ptr<void>(new aimrt::dds::SerializedMessage(), [](void* pointer) {
              delete static_cast<aimrt::dds::SerializedMessage*>(pointer);
            })
          : client->info.rsp_type_support_ref.CreateSharedPtr();
  if (!response) return DrainResult::kNoData;
  std::unique_lock publication_lock(client->publication_mutex, std::try_to_lock);
  if (!publication_lock.owns_lock()) {
    return DrainResult::kRetryRpcPublicationGateBusy;
  }
  if (!callback_state->running->load() || client->response_reader == nullptr) {
    return DrainResult::kNoData;
  }
  eprosima::fastdds::dds::SampleInfo sample_info;
  eprosima::fastdds::dds::ReturnCode_t result;
#if defined(BUILD_TESTING)
  const auto forced_result = eprosima::fastdds::dds::ReturnCode_t(
      client->forced_response_take_result.exchange(
          static_cast<int32_t>(eprosima::fastdds::dds::RETCODE_OK)));
  if (forced_result != eprosima::fastdds::dds::RETCODE_OK) {
    result = forced_result;
  } else
#endif
  {
    result = client->response_reader->take_next_sample(response.get(), &sample_info);
  }
  publication_lock.unlock();
  if (result == eprosima::fastdds::dds::RETCODE_NO_DATA) {
    return DrainResult::kNoData;
  }
  if (result != eprosima::fastdds::dds::RETCODE_OK) {
    const auto count = client->diagnostics->RecordReaderTakeFailure();
    if (ShouldLogRateLimited(count)) {
      const auto names = DeriveDdsRpcTopicNames(client->info.func_name);
      AIMRT_WARN(
          "aimrt_dds_reader_take_failure_total={} role=rpc_response "
          "function='{}' topic='{}' type='{}' return_code={}",
          count, client->info.func_name, names.response,
          client->info.rsp_type_support_ref.TypeName(),
          static_cast<int32_t>(result));
    }
    return DrainResult::kRetryLater;
  }
  if (!sample_info.valid_data) {
    // Fast DDS emits invalid-data samples for instance lifecycle changes
    // such as unregister/dispose. They carry no RPC payload and are not
    // malformed business responses.
    return DrainResult::kConsumed;
  }
  const auto& identity = sample_info.related_sample_identity;
  const auto identity_text = [](const auto& value) {
    std::ostringstream output;
    output << value;
    return output.str();
  };
  const auto reason_text = [](TerminalReason reason) -> std::string_view {
    switch (reason) {
      case TerminalReason::kResponseWon:
        return "response_won";
      case TerminalReason::kTimeout:
        return "timeout";
      case TerminalReason::kShutdown:
        return "shutdown";
    }
    return "";
  };
  const auto record_drop = [&](std::string_view classification,
                               std::string_view cause, bool history_hit,
                               std::optional<TerminalReason> terminal_reason,
                               std::optional<SampleIdentity> request_identity =
                                   std::nullopt) {
    uint64_t count = 0;
    if (classification == "foreign") {
      count = client->diagnostics->RecordRpcForeignResponse();
    } else if (classification == "late") {
      count = client->diagnostics->RecordRpcLateResponse();
    } else if (classification == "duplicate") {
      count = client->diagnostics->RecordRpcDuplicateResponse();
    } else {
      count = client->diagnostics->RecordRpcUnknownCorrelation();
    }
    const auto names = DeriveDdsRpcTopicNames(client->info.func_name);
    const auto message = fmt::format(
        "aimrt_dds_rpc_response_drop classification={} cause={} "
        "action=drop_no_callback function='{}' rsp_topic='{}' "
        "related_response_reader_guid='{}' request_sequence='{}' "
        "response_writer_guid='{}' response_sequence='{}' history_hit={} "
        "terminal_reason='{}' request_writer_guid='{}' "
        "request_writer_sequence='{}' count={}",
        classification, cause, client->info.func_name, names.response,
        identity_text(identity.writer_guid()), identity_text(identity.sequence_number()),
        identity_text(sample_info.sample_identity.writer_guid()),
        identity_text(sample_info.sample_identity.sequence_number()), history_hit,
        terminal_reason ? reason_text(*terminal_reason) : std::string_view{},
        request_identity ? identity_text(request_identity->writer_guid()) : "",
        request_identity ? identity_text(request_identity->sequence_number()) : "",
        count);
    if (classification == "duplicate" || classification == "unknown") {
      if (ShouldLogRateLimited(count)) AIMRT_WARN("{}", message);
    } else {
      AIMRT_DEBUG("{}", message);
    }
  };
  if (identity.writer_guid() != client->response_reader_guid) {
    auto local_identity = identity;
    local_identity.writer_guid(client->response_reader_guid);
    std::optional<SampleIdentity> request_identity;
    {
      std::lock_guard lock(client->mutex);
      const auto local_key = IdentityKey(local_identity);
      if (const auto pending = client->pending.find(local_key);
          pending != client->pending.end()) {
        request_identity = pending->second.request_identity;
      } else if (const auto terminal = client->terminal_history.find(local_key);
                 terminal != client->terminal_history.end()) {
        request_identity = terminal->second.request_identity;
      }
    }
    record_drop("foreign", "response_reader_guid_mismatch", false,
                std::nullopt, request_identity);
    return DrainResult::kConsumed;
  }
  const auto key = IdentityKey(identity);
  const auto classify_inactive = [&]() {
    std::optional<TerminalReason> terminal_reason;
    std::optional<SampleIdentity> request_identity;
    {
      std::lock_guard lock(client->mutex);
      if (const auto terminal = client->terminal_history.find(key);
          terminal != client->terminal_history.end()) {
        terminal_reason = terminal->second.reason;
        request_identity = terminal->second.request_identity;
      } else if (client->pending.contains(key)) {
        return false;
      }
    }
    if (!terminal_reason) {
      record_drop("unknown", "identity_not_active_or_retained", false,
                  std::nullopt);
    } else if (*terminal_reason == TerminalReason::kResponseWon) {
      record_drop("duplicate", "response_already_won", true, terminal_reason,
                  request_identity);
    } else {
      record_drop("late", "call_already_terminal", true, terminal_reason,
                  request_identity);
    }
    return true;
  };
  if (classify_inactive()) return DrainResult::kConsumed;
  if (client->wrapper) {
    auto& wire = *static_cast<aimrt::dds::SerializedMessage*>(response.get());
    const auto validation = ValidateSerializedMessage(
        wire, {.usage = SerializedMessageUsage::kRpc,
               .expected_type_name = client->info.rsp_type_support_ref.TypeName(),
               .expected_serialization_type = client->serialization_type});
    auto business_response = client->info.rsp_type_support_ref.CreateSharedPtr();
    aimrt::util::BufferArrayView payload(wire.data().data(), wire.data().size());
    if (!business_response || !validation.ok ||
        !client->info.rsp_type_support_ref.Deserialize(
            client->serialization_type, *payload.NativeHandle(), business_response.get())) {
      record_invalid_response(validation.ok ? "business_deserialize_failed"
                                            : validation.reason);
      return DrainResult::kConsumed;
    }
    if (!CompleteClient(client, identity, TerminalReason::kResponseWon,
                        aimrt::rpc::Status(), true, business_response.get())) {
      classify_inactive();
    }
  } else {
    if (!CompleteClient(client, identity, TerminalReason::kResponseWon,
                        aimrt::rpc::Status(), true, response.get())) {
      classify_inactive();
    }
  }
  return DrainResult::kConsumed;
}

bool DdsRpcBackend::CompleteClient(const std::shared_ptr<ClientState>& client,
                                   const SampleIdentity& identity,
                                   TerminalReason reason, aimrt::rpc::Status status,
                                   bool copy_response, void* response) {
  std::shared_ptr<runtime::core::rpc::InvokeWrapper> invoke;
  {
    std::lock_guard lock(client->mutex);
    const auto iterator = client->pending.find(IdentityKey(identity));
    if (iterator == client->pending.end()) return false;
    invoke = std::move(iterator->second.invoke);
    AddTerminalLocked(*client, IdentityKey(identity), iterator->second.request_identity,
                      reason);
    client->pending.erase(iterator);
  }
  if (!invoke || !invoke->callback) return true;
  try {
    if (copy_response && response != nullptr) {
      client->info.rsp_type_support_ref.Copy(response, invoke->rsp_ptr);
    }
  } catch (...) {
    status = aimrt::rpc::Status(AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
  }
  runtime::core::rpc::InvokeCallBack(*invoke, std::move(status));
  return true;
}

DdsReaderDrainState::DrainResult DdsRpcBackend::DrainServer(
    const std::shared_ptr<CallbackState>& callback_state,
    const std::shared_ptr<ServerState>& server) {
  using DrainResult = DdsReaderDrainState::DrainResult;
#if defined(BUILD_TESTING)
  callback_state->server_drains_in_flight.fetch_add(1);
  struct DrainObservationGuard {
    std::atomic_uint64_t& count;
    ~DrainObservationGuard() { count.fetch_sub(1); }
  } drain_observation_guard{callback_state->server_drains_in_flight};
#endif
  if (!callback_state->running->load() || server->request_reader == nullptr) {
    return DrainResult::kNoData;
  }
  const auto diagnostics = callback_state->backend_state->runtime.Diagnostics();
  const auto record_invalid_request = [&](std::string_view reason) {
    const auto count = diagnostics->RecordRpcInvalidRequest();
    if (ShouldLogRateLimited(count)) {
      const auto names = DeriveDdsRpcTopicNames(server->info.func_name);
      AIMRT_WARN(
          "aimrt_dds_rpc_invalid_request_total={} role=rpc_request "
          "function='{}' topic='{}' type='{}' reason='{}' action=drop_no_handler",
          count, server->info.func_name, names.request,
          server->info.req_type_support_ref.TypeName(), reason);
    }
  };
  auto request = server->wrapper
                     ? std::shared_ptr<void>(new aimrt::dds::SerializedMessage(),
                                             [](void* pointer) {
                                               delete static_cast<aimrt::dds::SerializedMessage*>(
                                                   pointer);
                                             })
                     : server->info.req_type_support_ref.CreateSharedPtr();
  auto response = server->wrapper
                      ? std::shared_ptr<void>(new aimrt::dds::SerializedMessage(),
                                              [](void* pointer) {
                                                delete static_cast<aimrt::dds::SerializedMessage*>(
                                                    pointer);
                                              })
                      : server->info.rsp_type_support_ref.CreateSharedPtr();
  if (!request || !response) return DrainResult::kRetryLater;
  eprosima::fastdds::dds::SampleInfo sample_info;
  eprosima::fastdds::dds::ReturnCode_t result;
#if defined(BUILD_TESTING)
  const auto forced_result = eprosima::fastdds::dds::ReturnCode_t(
      server->forced_request_take_result.exchange(
          static_cast<int32_t>(eprosima::fastdds::dds::RETCODE_OK)));
  if (forced_result != eprosima::fastdds::dds::RETCODE_OK) {
    result = forced_result;
  } else
#endif
  {
    result = server->request_reader->take_next_sample(request.get(), &sample_info);
  }
  if (result == eprosima::fastdds::dds::RETCODE_NO_DATA) return DrainResult::kNoData;
  if (result != eprosima::fastdds::dds::RETCODE_OK) {
    const auto count = diagnostics->RecordReaderTakeFailure();
    if (ShouldLogRateLimited(count)) {
      const auto names = DeriveDdsRpcTopicNames(server->info.func_name);
      AIMRT_WARN(
          "aimrt_dds_reader_take_failure_total={} role=rpc_request "
          "function='{}' topic='{}' type='{}' return_code={}",
          count, server->info.func_name, names.request,
          server->info.req_type_support_ref.TypeName(),
          static_cast<int32_t>(result));
    }
    return DrainResult::kRetryLater;
  }
  if (!sample_info.valid_data) {
    // Fast DDS emits invalid-data samples for instance lifecycle changes
    // such as unregister/dispose. They carry no RPC payload and are not
    // malformed business requests.
    return DrainResult::kConsumed;
  }
  auto related_identity = sample_info.related_sample_identity;
#if defined(BUILD_TESTING)
  const auto request_identity_override = callback_state->request_identity_override;
  if (request_identity_override && *request_identity_override) {
    (*request_identity_override)(related_identity, sample_info.sample_identity);
  }
#endif
  SampleIdentity correlation_token;
  if (!FillCorrelationToken(sample_info.sample_identity, related_identity,
                            correlation_token)) {
    record_invalid_request("invalid_correlation_identity");
    return DrainResult::kConsumed;
  }
  std::shared_ptr<void> request_business =
      server->wrapper ? server->info.req_type_support_ref.CreateSharedPtr() : request;
  std::shared_ptr<void> response_business =
      server->wrapper ? server->info.rsp_type_support_ref.CreateSharedPtr() : response;
  if (!request_business || !response_business) return DrainResult::kConsumed;
  if (server->wrapper) {
    auto& wire = *static_cast<aimrt::dds::SerializedMessage*>(request.get());
    const auto validation = ValidateSerializedMessage(
        wire, {.usage = SerializedMessageUsage::kRpc,
               .expected_type_name = server->info.req_type_support_ref.TypeName(),
               .expected_serialization_type = server->serialization_type});
    aimrt::util::BufferArrayView payload(wire.data().data(), wire.data().size());
    if (!validation.ok ||
        !server->info.req_type_support_ref.Deserialize(
            server->serialization_type, *payload.NativeHandle(), request_business.get())) {
      record_invalid_request(validation.ok ? "business_deserialize_failed"
                                           : validation.reason);
      return DrainResult::kConsumed;
    }
  }
  auto context = std::make_shared<aimrt::rpc::Context>(
      aimrt_rpc_context_type_t::AIMRT_RPC_SERVER_CONTEXT);
  context->SetMetaValue(AIMRT_RPC_CONTEXT_KEY_BACKEND, "dds");
  context->SetSerializationType(server->wrapper
                                    ? server->serialization_type
                                    : server->info.req_type_support_ref.DefaultSerializationType());
  auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = server->info,
                                        .req_ptr = request_business.get(),
                                        .rsp_ptr = response_business.get(),
                                        .ctx_ref = context});
  const auto request_identity = sample_info.sample_identity;
  const auto backend_state = callback_state->backend_state;
  const auto running_token = callback_state->running;
#if defined(BUILD_TESTING)
  const auto requester_match_override = callback_state->requester_match_override;
  const bool force_response_serialization_failure =
      callback_state->fail_next_response_serialization.exchange(false);
  std::function<void()> before_response_write;
  {
    std::lock_guard hook_lock(callback_state->response_write_hook_mutex);
    before_response_write = callback_state->before_response_write_hook;
  }
#endif
  auto handler_terminal = std::make_shared<std::atomic_bool>(false);
  invoke->callback = [backend_state, running_token, server, request, response,
                      request_business, response_business, request_identity,
                      correlation_token, handler_terminal
#if defined(BUILD_TESTING)
                      ,
                      callback_state,
                      requester_match_override, force_response_serialization_failure,
                      before_response_write
#endif
  ](aimrt::rpc::Status status) {
    if (handler_terminal->exchange(true)) {
      const auto count =
          backend_state->runtime.Diagnostics()->RecordRpcLateServerCompletion();
      if (ShouldLogRateLimited(count)) {
        AIMRT_WARN(
            "aimrt_dds_rpc_late_server_completion_total={} function='{}' "
            "action=drop_no_write",
            count, server->info.func_name);
      }
      return;
    }
    if (!status.OK()) {
      const auto count =
          backend_state->runtime.Diagnostics()->RecordRpcServerNonOkCompletion();
      if (ShouldLogRateLimited(count)) {
        AIMRT_WARN(
            "aimrt_dds_rpc_server_non_ok_completion_total={} function='{}' "
            "status_code={} action=drop_no_write",
            count, server->info.func_name, status.Code());
      }
      return;
    }
    if (!running_token->load()) {
      const auto count =
          backend_state->runtime.Diagnostics()->RecordRpcLateServerCompletion();
      if (ShouldLogRateLimited(count)) {
        AIMRT_WARN(
            "aimrt_dds_rpc_late_server_completion_total={} function='{}' "
            "reason=backend_stopping action=drop_no_write",
            count, server->info.func_name);
      }
      return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    auto send_reply = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak_send = send_reply;
    *send_reply = [backend_state, running_token, server, response, response_business,
                   request_identity, correlation_token, deadline, weak_send
#if defined(BUILD_TESTING)
                   ,
                   callback_state,
                   requester_match_override, force_response_serialization_failure,
                   before_response_write
#endif
    ] {
      if (!running_token->load()) {
        const auto count =
            backend_state->runtime.Diagnostics()->RecordRpcLateServerCompletion();
        if (ShouldLogRateLimited(count)) {
          AIMRT_WARN(
              "aimrt_dds_rpc_late_server_completion_total={} function='{}' "
              "reason=backend_stopping action=drop_no_write",
              count, server->info.func_name);
        }
        return;
      }
      auto operation = backend_state->runtime.Endpoints().AcquireWriterOperation();
      if (!operation || operation.Stopping() || !running_token->load()) return;
      auto match = RequesterMatchState(server->request_reader, server->response_writer,
                                       request_identity,
                                       correlation_token.writer_guid());
#if defined(BUILD_TESTING)
      if (requester_match_override && *requester_match_override) {
        switch ((*requester_match_override)()) {
          case 0:
            match = RequesterMatch::kUnmatched;
            break;
          case 1:
            match = RequesterMatch::kPartial;
            break;
          default:
            match = RequesterMatch::kMatched;
            break;
        }
      }
#endif
      if (match == RequesterMatch::kUnmatched) {
        const auto count = backend_state->runtime.Diagnostics()
                               ->RecordRpcRequesterUnmatchedOnReply();
        if (ShouldLogRateLimited(count)) {
          AIMRT_WARN(
              "aimrt_dds_rpc_requester_unmatched_on_reply_total={} "
              "function='{}' action=drop_no_write",
              count, server->info.func_name);
        }
        return;
      }
      if (match == RequesterMatch::kPartial) {
        if (std::chrono::steady_clock::now() >= deadline) {
          const auto count = backend_state->runtime.Diagnostics()
                                 ->RecordRpcReplyMatchTimeout();
          if (ShouldLogRateLimited(count)) {
            AIMRT_WARN(
                "aimrt_dds_rpc_reply_match_timeout_total={} function='{}' "
                "wait_seconds=3 action=drop_no_write",
                count, server->info.func_name);
          }
          return;
        }
        if (auto next = weak_send.lock()) {
          backend_state->runtime.Executor()->ScheduleAfter(
              std::chrono::milliseconds(100), [next] { (*next)(); });
        }
        return;
      }
      void* payload = response.get();
      if (server->wrapper) {
#if defined(BUILD_TESTING)
        if (force_response_serialization_failure) return;
#endif
        auto& wire = *static_cast<aimrt::dds::SerializedMessage*>(response.get());
        auto serialized = std::make_unique<BoundedSerializedPayload>();
        if (!server->info.rsp_type_support_ref.Serialize(
                server->serialization_type, response_business.get(),
                serialized->allocator.NativeHandle(),
                serialized->buffers.BufferArrayNativeHandle())) {
          return;
        }
        wire.type_name(std::string(server->info.rsp_type_support_ref.TypeName()));
        wire.serialization_type(server->serialization_type);
        wire.metadata().clear();
        wire.data().resize(serialized->buffers.BufferSize());
        auto* output = wire.data().data();
        for (const auto& item :
             std::span(serialized->buffers.Data(), serialized->buffers.Size())) {
          std::memcpy(output, item.data, item.len);
          output += item.len;
        }
        payload = &wire;
      }
#if defined(BUILD_TESTING)
      if (before_response_write) before_response_write();
#endif
      if (operation.Stopping() || !running_token->load()) {
        const auto count =
            backend_state->runtime.Diagnostics()->RecordRpcLateServerCompletion();
        if (ShouldLogRateLimited(count)) {
          AIMRT_WARN(
              "aimrt_dds_rpc_late_server_completion_total={} function='{}' "
              "reason=backend_stopping_before_write action=drop_no_write",
              count, server->info.func_name);
        }
        return;
      }
      eprosima::fastdds::rtps::WriteParams params;
      params.related_sample_identity(correlation_token);
#if defined(BUILD_TESTING)
      ++callback_state->response_write_calls;
#endif
      server->response_writer->write(payload, params);
    };
    (*send_reply)();
  };
  try {
    server->service_func(invoke);
  } catch (...) {
    if (invoke->callback) {
      invoke->callback(aimrt::rpc::Status(AIMRT_RPC_STATUS_SVR_HANDLE_FAILED));
    }
  }
  return DrainResult::kConsumed;
}

void DdsRpcBackend::Invoke(
    const std::shared_ptr<runtime::core::rpc::InvokeWrapper>& wrapper) noexcept {
  if (!wrapper || !wrapper->callback) return;
  std::shared_ptr<ClientState> client;
  {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(LogicalKey(wrapper->info));
    if (it != clients_.end()) client = it->second;
  }
  if (!running_token_->load() || !client || !client->request_writer) {
    runtime::core::rpc::InvokeCallBack(
        *wrapper, aimrt::rpc::Status(AIMRT_RPC_STATUS_SVR_NOT_FOUND));
    return;
  }
  SampleIdentity active_token;
  bool pending_published = false;
  try {
    aimrt::dds::SerializedMessage wire_request;
    void* request_payload = const_cast<void*>(wrapper->req_ptr);
    std::unique_ptr<BoundedSerializedPayload> serialized_request;
    if (client->wrapper) {
#if defined(BUILD_TESTING)
      if (fail_next_request_serialization_.exchange(false)) {
        throw std::runtime_error("DDS RPC forced request serialization failure");
      }
#endif
      serialized_request = std::make_unique<BoundedSerializedPayload>();
      if (!client->info.req_type_support_ref.Serialize(
              client->serialization_type, wrapper->req_ptr,
              serialized_request->allocator.NativeHandle(),
              serialized_request->buffers.BufferArrayNativeHandle())) {
        throw std::runtime_error("DDS RPC request serialization failed");
      }
      wire_request.type_name(std::string(client->info.req_type_support_ref.TypeName()));
      wire_request.serialization_type(client->serialization_type);
      wire_request.metadata().clear();
      wire_request.data().resize(serialized_request->buffers.BufferSize());
      auto* out = wire_request.data().data();
      for (const auto& item : std::span(serialized_request->buffers.Data(), serialized_request->buffers.Size())) {
        std::memcpy(out, item.data, item.len);
        out += item.len;
      }
      request_payload = &wire_request;
    }
#if defined(BUILD_TESTING)
    std::function<void()> before_write;
    std::function<void()> after_write;
    {
      std::lock_guard hook_lock(request_write_hook_mutex_);
      before_write = before_request_write_hook_;
      after_write = after_request_write_hook_;
    }
#endif
    std::optional<aimrt::rpc::Status> failure;
    {
      std::unique_lock publication_lock(client->publication_mutex);
      eprosima::fastdds::dds::PublicationMatchedStatus writer_status;
      const auto matched_servers =
          client->request_writer != nullptr &&
                  client->request_writer->get_publication_matched_status(writer_status) ==
                      eprosima::fastdds::dds::RETCODE_OK
              ? writer_status.current_count
              : 0;
      ObserveMatchedServers(client, matched_servers);
      if (!running_token_->load() || client->request_writer == nullptr) {
        failure.emplace(AIMRT_RPC_STATUS_SVR_NOT_FOUND);
      } else if (client->ReadinessState() != ClientState::Readiness::kMatched) {
        failure.emplace(AIMRT_RPC_STATUS_SVR_NOT_FOUND);
      } else {
        bool pending_full = false;
        {
          std::lock_guard pending_lock(client->mutex);
          pending_full = client->pending.size() >= ClientState::kPendingLimit;
        }
        if (pending_full) {
          const auto count = client->diagnostics->RecordRpcPendingRejected();
          if (ShouldLogRateLimited(count)) {
            AIMRT_ERROR(
                "aimrt_dds_rpc_pending_rejected_total={} function='{}' "
                "active_pending={} limit={} action=reject_no_write",
                count, client->info.func_name, ClientState::kPendingLimit,
                ClientState::kPendingLimit);
          }
          failure.emplace(AIMRT_RPC_STATUS_CLI_NOT_READY);
        } else {
          auto operation = state_->runtime.Endpoints().AcquireWriterOperation();
          if (!operation || operation.Stopping() || !running_token_->load()) {
            failure.emplace(AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR);
          } else {
#if defined(BUILD_TESTING)
            if (before_write) before_write();
#endif
            eprosima::fastdds::rtps::WriteParams params;
            SampleIdentity related_identity;
            related_identity.writer_guid(client->response_reader_guid);
            related_identity.sequence_number(
                eprosima::fastdds::rtps::c_SequenceNumber_Unknown);
            params.related_sample_identity(related_identity);
            eprosima::fastdds::dds::ReturnCode_t result;
#if defined(BUILD_TESTING)
            if (fail_next_request_write_.exchange(false)) {
              result = eprosima::fastdds::dds::RETCODE_ERROR;
            } else
#endif
            {
              result = client->request_writer->write(request_payload, params);
            }
            if (result != eprosima::fastdds::dds::RETCODE_OK) {
              failure.emplace(AIMRT_RPC_STATUS_CLI_SEND_REQ_FAILED);
            } else {
              const auto request_identity = params.sample_identity();
              if (!IdentityValid(request_identity)) {
                failure.emplace(AIMRT_RPC_STATUS_CLI_SEND_REQ_FAILED);
              } else {
                active_token = related_identity;
                active_token.sequence_number(request_identity.sequence_number());
#if defined(BUILD_TESTING)
                if (after_write) after_write();
#endif
                bool inserted = false;
                {
                  std::lock_guard pending_lock(client->mutex);
                  inserted = client->pending
                                 .emplace(IdentityKey(active_token),
                                          ClientState::Pending{
                                              .invoke = wrapper,
                                              .request_identity = request_identity,
                                              .deadline = std::chrono::steady_clock::now() +
                                                          wrapper->ctx_ref.Timeout()})
                                 .second;
                  pending_published = inserted;
                }
                if (!inserted) {
                  failure.emplace(AIMRT_RPC_STATUS_CLI_SEND_REQ_FAILED);
                }
              }
            }
          }
        }
      }
    }
    if (failure) {
      runtime::core::rpc::InvokeCallBack(*wrapper, std::move(*failure));
      return;
    }
    const auto timeout = wrapper->ctx_ref.Timeout();
    bool scheduled = false;
#if defined(BUILD_TESTING)
    if (!fail_next_schedule_.exchange(false))
#endif
    {
      scheduled = state_->runtime.Executor()->ScheduleAfter(
          timeout.count() > 0 ? timeout : std::chrono::seconds(5),
          [client, active_token, callback_state = callback_state_] {
#if defined(BUILD_TESTING)
            std::function<void()> before_timeout_completion;
            {
              std::lock_guard lock(callback_state->timeout_completion_hook_mutex);
              before_timeout_completion =
                  callback_state->before_timeout_completion_hook;
            }
            if (before_timeout_completion) before_timeout_completion();
#endif
            const bool stopping = !callback_state->running->load();
            CompleteClient(
                client, active_token,
                stopping ? TerminalReason::kShutdown : TerminalReason::kTimeout,
                aimrt::rpc::Status(stopping ? AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR
                                            : AIMRT_RPC_STATUS_TIMEOUT),
                false, nullptr);
          });
    }
    if (!scheduled) {
      CompleteClient(client, active_token, TerminalReason::kTimeout,
                     aimrt::rpc::Status(AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR), false,
                     nullptr);
    }
  } catch (...) {
    if (pending_published) {
      CompleteClient(client, active_token, TerminalReason::kTimeout,
                     aimrt::rpc::Status(AIMRT_RPC_STATUS_CLI_BACKEND_INTERNAL_ERROR), false,
                     nullptr);
    } else if (wrapper->callback) {
      runtime::core::rpc::InvokeCallBack(
          *wrapper, aimrt::rpc::Status(AIMRT_RPC_STATUS_CLI_SEND_REQ_FAILED));
    }
  }
}

#if defined(BUILD_TESTING)
eprosima::fastdds::rtps::GUID_t DdsRpcBackend::ResponseReaderGuidForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::lock_guard lock(mutex_);
  const auto iterator = clients_.find(LogicalKey(info));
  return iterator == clients_.end() ? eprosima::fastdds::rtps::GUID_t{}
                                    : iterator->second->response_reader_guid;
}

eprosima::fastdds::rtps::GUID_t DdsRpcBackend::RequestWriterGuidForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::lock_guard lock(mutex_);
  const auto iterator = clients_.find(LogicalKey(info));
  return iterator == clients_.end() || iterator->second->request_writer == nullptr
             ? eprosima::fastdds::rtps::GUID_t{}
             : iterator->second->request_writer->guid();
}

int64_t DdsRpcBackend::RequestWriterMaxBlockingTimeUsForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::lock_guard lock(mutex_);
  const auto iterator = clients_.find(LogicalKey(info));
  if (iterator == clients_.end() || iterator->second->request_writer == nullptr) {
    return -1;
  }
  eprosima::fastdds::dds::DataWriterQos qos;
  if (iterator->second->request_writer->get_qos(qos) !=
      eprosima::fastdds::dds::RETCODE_OK) {
    return -1;
  }
  return qos.reliability().max_blocking_time.to_ns() / 1000;
}

size_t DdsRpcBackend::PendingCountForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::shared_ptr<ClientState> client;
  {
    std::lock_guard lock(mutex_);
    const auto iterator = clients_.find(LogicalKey(info));
    if (iterator != clients_.end()) client = iterator->second;
  }
  if (!client) return 0;
  std::lock_guard lock(client->mutex);
  return client->pending.size();
}

size_t DdsRpcBackend::TerminalHistoryCountForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::shared_ptr<ClientState> client;
  {
    std::lock_guard lock(mutex_);
    const auto iterator = clients_.find(LogicalKey(info));
    if (iterator != clients_.end()) client = iterator->second;
  }
  if (!client) return 0;
  std::lock_guard lock(client->mutex);
  return client->terminal_history.size();
}

std::vector<eprosima::fastdds::rtps::SampleIdentity>
DdsRpcBackend::PendingTokensForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::shared_ptr<ClientState> client;
  {
    std::lock_guard lock(mutex_);
    const auto iterator = clients_.find(LogicalKey(info));
    if (iterator != clients_.end()) client = iterator->second;
  }
  std::vector<SampleIdentity> tokens;
  if (!client) return tokens;
  std::lock_guard lock(client->mutex);
  tokens.reserve(client->pending.size());
  for (const auto& [_, pending] : client->pending) {
    auto token = SampleIdentity{};
    token.writer_guid(client->response_reader_guid);
    token.sequence_number(pending.request_identity.sequence_number());
    tokens.emplace_back(token);
  }
  return tokens;
}

eprosima::fastdds::rtps::SampleIdentity
DdsRpcBackend::FillTerminalHistoryForTesting(
    const runtime::core::rpc::FuncInfo& info, size_t count) {
  std::shared_ptr<ClientState> client;
  {
    std::lock_guard lock(mutex_);
    const auto iterator = clients_.find(LogicalKey(info));
    if (iterator != clients_.end()) client = iterator->second;
  }
  SampleIdentity first;
  if (!client || count == 0) return first;
  std::lock_guard lock(client->mutex);
  const auto sequence_base = 1000000U +
                             static_cast<uint32_t>(client->terminal_order.size());
  for (size_t index = 0; index < count; ++index) {
    SampleIdentity token;
    token.writer_guid(client->response_reader_guid);
    token.sequence_number(eprosima::fastdds::rtps::SequenceNumber_t(
        0, sequence_base + static_cast<uint32_t>(index)));
    SampleIdentity request_identity;
    request_identity.writer_guid(client->request_writer->guid());
    request_identity.sequence_number(token.sequence_number());
    AddTerminalLocked(*client, IdentityKey(token), request_identity,
                      TerminalReason::kTimeout);
    if (index == 0) first = token;
  }
  return first;
}

void DdsRpcBackend::ForceNextResponseTakeResultForTesting(
    const runtime::core::rpc::FuncInfo& info, int32_t result) {
  std::lock_guard lock(mutex_);
  const auto iterator = clients_.find(LogicalKey(info));
  if (iterator != clients_.end()) {
    iterator->second->forced_response_take_result = result;
  }
}

void DdsRpcBackend::ForceNextRequestTakeResultForTesting(
    const runtime::core::rpc::FuncInfo& info, int32_t result) {
  std::lock_guard lock(mutex_);
  const auto iterator = servers_.find(LogicalKey(info));
  if (iterator != servers_.end()) {
    iterator->second->forced_request_take_result = result;
  }
}

std::string_view DdsRpcBackend::ReadinessForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::shared_ptr<ClientState> client;
  {
    std::lock_guard lock(mutex_);
    const auto iterator = clients_.find(LogicalKey(info));
    if (iterator != clients_.end()) client = iterator->second;
  }
  if (!client) return "MISSING";
  eprosima::fastdds::dds::PublicationMatchedStatus writer_status;
  const auto matched_servers =
      client->request_writer != nullptr &&
              client->request_writer->get_publication_matched_status(writer_status) ==
                  eprosima::fastdds::dds::RETCODE_OK
          ? writer_status.current_count
          : 0;
  ObserveMatchedServers(client, matched_servers);
  switch (client->ReadinessState()) {
    case ClientState::Readiness::kUnmatched:
      return "UNMATCHED";
    case ClientState::Readiness::kPartiallyMatched:
      return "PARTIALLY_MATCHED";
    case ClientState::Readiness::kMatched:
      return "MATCHED";
  }
  return "MISSING";
}

std::string_view DdsRpcBackend::ClassifyReadinessForTesting(
    uint32_t response_writer_matches, uint32_t request_reader_matches) noexcept {
  if (response_writer_matches == 0) return "UNMATCHED";
  if (request_reader_matches == 0 ||
      response_writer_matches != request_reader_matches) {
    return "PARTIALLY_MATCHED";
  }
  return "MATCHED";
}

bool DdsRpcBackend::FillCorrelationTokenForTesting(
    const eprosima::fastdds::rtps::SampleIdentity& request_identity,
    const eprosima::fastdds::rtps::SampleIdentity& related_identity,
    eprosima::fastdds::rtps::SampleIdentity& correlation_token) noexcept {
  return FillCorrelationToken(request_identity, related_identity, correlation_token);
}

uint64_t DdsRpcBackend::InvalidRequestCountForTesting() const noexcept {
  return state_->runtime.Diagnostics()->Snapshot().rpc_invalid_request_total;
}

void DdsRpcBackend::FailNextResponseSerializationForTesting() noexcept {
  callback_state_->fail_next_response_serialization = true;
}

uint64_t DdsRpcBackend::ServerDrainsInFlightForTesting() const noexcept {
  return callback_state_->server_drains_in_flight.load();
}

void DdsRpcBackend::SetRequesterMatchOverrideForTesting(
    std::function<int()> hook) {
  callback_state_->requester_match_override =
      std::make_shared<std::function<int()>>(std::move(hook));
}

void DdsRpcBackend::SetRequestIdentityOverrideForTesting(
    std::function<void(eprosima::fastdds::rtps::SampleIdentity&,
                       const eprosima::fastdds::rtps::SampleIdentity&)>
        hook) {
  callback_state_->request_identity_override =
      std::make_shared<decltype(hook)>(std::move(hook));
}

void DdsRpcBackend::SetBeforeRequestWriteHookForTesting(std::function<void()> hook) {
  std::lock_guard lock(request_write_hook_mutex_);
  before_request_write_hook_ = std::move(hook);
}

void DdsRpcBackend::SetAfterRequestWriteHookForTesting(std::function<void()> hook) {
  std::lock_guard lock(request_write_hook_mutex_);
  after_request_write_hook_ = std::move(hook);
}

void DdsRpcBackend::SetBeforeResponseWriteHookForTesting(std::function<void()> hook) {
  std::lock_guard lock(callback_state_->response_write_hook_mutex);
  callback_state_->before_response_write_hook = std::move(hook);
}

void DdsRpcBackend::SetBeforeTimeoutCompletionHookForTesting(
    std::function<void()> hook) {
  std::lock_guard lock(callback_state_->timeout_completion_hook_mutex);
  callback_state_->before_timeout_completion_hook = std::move(hook);
}

void DdsRpcBackend::SetAfterShutdownStartedHookForTesting(
    std::function<void()> hook) {
  std::lock_guard lock(callback_state_->shutdown_hook_mutex);
  callback_state_->after_shutdown_started_hook = std::move(hook);
}

uint64_t DdsRpcBackend::ResponseWriteCallCountForTesting() const noexcept {
  return callback_state_->response_write_calls.load();
}

uint64_t DdsRpcBackend::ResponseRetryScheduleCountForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::lock_guard lock(mutex_);
  const auto iterator = clients_.find(LogicalKey(info));
  return iterator == clients_.end() || !iterator->second->drain
             ? 0
             : iterator->second->drain->RetryScheduleCount();
}

uint64_t DdsRpcBackend::ResponseGateBusyRetryCountForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::lock_guard lock(mutex_);
  const auto iterator = clients_.find(LogicalKey(info));
  return iterator == clients_.end() || !iterator->second->drain
             ? 0
             : iterator->second->drain->RpcPublicationGateBusyRetryCount();
}

uint32_t DdsRpcBackend::ResponseLastRetryDelayUsForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::lock_guard lock(mutex_);
  const auto iterator = clients_.find(LogicalKey(info));
  return iterator == clients_.end() || !iterator->second->drain
             ? 0
             : iterator->second->drain->LastRetryDelayUs();
}

std::shared_ptr<DdsReaderDrainState> DdsRpcBackend::ClientDrainForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::lock_guard lock(mutex_);
  const auto iterator = clients_.find(LogicalKey(info));
  return iterator == clients_.end() ? nullptr : iterator->second->drain;
}

std::shared_ptr<DdsReaderDrainState> DdsRpcBackend::ServerDrainForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::lock_guard lock(mutex_);
  const auto iterator = servers_.find(LogicalKey(info));
  return iterator == servers_.end() ? nullptr : iterator->second->drain;
}

int64_t DdsRpcBackend::ResponseUnreadCountForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::lock_guard lock(mutex_);
  const auto iterator = clients_.find(LogicalKey(info));
  return iterator == clients_.end() || iterator->second->response_reader == nullptr
             ? 0
             : iterator->second->response_reader->get_unread_count();
}

int64_t DdsRpcBackend::RequestUnreadCountForTesting(
    const runtime::core::rpc::FuncInfo& info) const {
  std::lock_guard lock(mutex_);
  const auto iterator = servers_.find(LogicalKey(info));
  return iterator == servers_.end() || iterator->second->request_reader == nullptr
             ? 0
             : iterator->second->request_reader->get_unread_count();
}

void DdsRpcBackend::DisconnectRequestWriterForTesting(
    const runtime::core::rpc::FuncInfo& info) {
  std::shared_ptr<ClientState> client;
  {
    std::lock_guard lock(mutex_);
    const auto iterator = clients_.find(LogicalKey(info));
    if (iterator == clients_.end()) return;
    client = iterator->second;
  }
  std::lock_guard publication_lock(client->publication_mutex);
  auto* writer = client->request_writer;
  client->request_writer = nullptr;
  state_->runtime.Endpoints().DeleteWriter(writer);
}

void DdsRpcBackend::DisconnectServerForTesting(
    const runtime::core::rpc::FuncInfo& info) {
  std::shared_ptr<ServerState> server;
  {
    std::lock_guard lock(mutex_);
    const auto iterator = servers_.find(LogicalKey(info));
    if (iterator == servers_.end()) return;
    server = iterator->second;
  }
  if (server->drain) server->drain->Stop();
  auto* reader = server->request_reader;
  auto* writer = server->response_writer;
  server->request_reader = nullptr;
  server->response_writer = nullptr;
  state_->runtime.Endpoints().DeleteReader(reader);
  state_->runtime.Endpoints().DeleteWriter(writer);
}
#endif

}  // namespace aimrt::plugins::dds_plugin
