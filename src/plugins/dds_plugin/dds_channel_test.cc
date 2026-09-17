// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "dds_plugin/dds_backend.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/xtypes/type_representation/ITypeObjectRegistry.hpp>
#include <fastdds/dds/xtypes/type_representation/TypeObject.hpp>
#include <fastdds/rtps/transport/test_UDPv4TransportDescriptor.hpp>

#include "Calculator.h"
#include "DdsLoanTypes.h"
#include "aimrt_module_dds_interface/channel/dds_channel.h"
#include "dds_plugin/global.h"
#include "dds_plugin/serialized_message_adapter.h"
#include "detail/serialized_message/serialized_messagePubSubTypes.hpp"

namespace aimrt::plugins::dds_plugin {
namespace {

using namespace std::chrono_literals;
using namespace eprosima::fastdds::dds;

uint32_t TestDomain(uint32_t offset) {
  const uint32_t category = offset % 100U - 20U;
  const uint32_t phase = offset >= 100U ? 10U : 0U;
  return 20U + category * 20U + phase + static_cast<uint32_t>(getpid()) % 10U;
}

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

class TemporaryXml {
 public:
  TemporaryXml(std::string_view name, std::string_view contents)
      : path_(std::filesystem::temp_directory_path() /
              (std::string(name) + "_" + std::to_string(getpid()) + ".xml")) {
    std::ofstream output(path_);
    output << contents;
    if (!output) throw std::runtime_error("failed to create DDS test XML");
  }
  ~TemporaryXml() { std::filesystem::remove(path_); }
  std::string Path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

class TemporaryPath {
 public:
  explicit TemporaryPath(std::string_view name)
      : path_(std::filesystem::temp_directory_path() /
              (std::string(name) + "_" + std::to_string(getpid()))) {
    std::filesystem::remove(path_);
  }
  ~TemporaryPath() { std::filesystem::remove(path_); }
  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class ScopedLogCapture {
 public:
  ScopedLogCapture() {
    logger_ = aimrt_logger_base_t{
        .get_log_level = [](void*) { return AIMRT_LOG_LEVEL_TRACE; },
        .log = [](void* impl, aimrt_log_level_t level, uint32_t, const char*, const char*,
                  const char* data, size_t size) {
          auto* self = static_cast<ScopedLogCapture*>(impl);
          std::lock_guard lock(self->mutex_);
          self->logs_.emplace_back(level, std::string(data, size)); },
        .impl = this};
    SetLogger(aimrt::logger::LoggerRef(&logger_));
  }

  ~ScopedLogCapture() { SetLogger(aimrt::logger::GetSimpleLoggerRef()); }

  size_t Count(aimrt_log_level_t level, std::string_view text) const {
    std::lock_guard lock(mutex_);
    return std::ranges::count_if(logs_, [level, text](const auto& entry) {
      return entry.first == level && entry.second.find(text) != std::string::npos;
    });
  }

  bool Contains(aimrt_log_level_t level, std::string_view first,
                std::string_view second) const {
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(logs_, [level, first, second](const auto& entry) {
      return entry.first == level && entry.second.find(first) != std::string::npos &&
             entry.second.find(second) != std::string::npos;
    });
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::pair<aimrt_log_level_t, std::string>> logs_;
  aimrt_logger_base_t logger_{};
};

constexpr std::string_view kConstrainedReliableXml = R"xml(
<dds xmlns="http://www.eprosima.com"><profiles>
  <data_writer profile_name="dds05_reliable_writer" is_default_profile="true">
    <qos><reliability><kind>RELIABLE</kind></reliability></qos>
    <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
  </data_writer>
  <data_reader profile_name="dds05_constrained_reader" is_default_profile="true">
    <qos><reliability><kind>RELIABLE</kind></reliability></qos>
    <topic>
      <historyQos><kind>KEEP_ALL</kind></historyQos>
      <resourceLimitsQos>
        <max_samples>2</max_samples><max_instances>1</max_instances>
        <max_samples_per_instance>2</max_samples_per_instance><allocated_samples>2</allocated_samples>
      </resourceLimitsQos>
    </topic>
  </data_reader>
</profiles></dds>)xml";

constexpr std::string_view kReliableKeepAllXml = R"xml(
<dds xmlns="http://www.eprosima.com"><profiles>
  <data_writer profile_name="dds05_keep_all_writer" is_default_profile="true">
    <qos><reliability><kind>RELIABLE</kind></reliability></qos>
    <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
  </data_writer>
  <data_reader profile_name="dds05_keep_all_reader" is_default_profile="true">
    <qos><reliability><kind>RELIABLE</kind></reliability></qos>
    <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
  </data_reader>
</profiles></dds>)xml";

constexpr std::string_view kDataSharingOffXml = R"xml(
<dds xmlns="http://www.eprosima.com"><profiles>
  <data_writer profile_name="dds10_data_sharing_off_writer" is_default_profile="true">
    <qos>
      <reliability>
        <kind>RELIABLE</kind>
        <max_blocking_time><sec>0</sec><nanosec>0</nanosec></max_blocking_time>
      </reliability>
      <data_sharing><kind>OFF</kind></data_sharing>
    </qos>
    <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
  </data_writer>
  <data_reader profile_name="dds10_data_sharing_off_reader" is_default_profile="true">
    <qos>
      <reliability><kind>RELIABLE</kind></reliability>
      <data_sharing><kind>OFF</kind></data_sharing>
    </qos>
    <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
  </data_reader>
</profiles></dds>)xml";

constexpr std::string_view kLoanHistoryExhaustionXml = R"xml(
<dds xmlns="http://www.eprosima.com"><profiles>
  <data_writer profile_name="dds06_loan_history_exhaustion" is_default_profile="true">
    <qos>
      <reliability>
        <kind>RELIABLE</kind>
        <max_blocking_time><sec>0</sec><nanosec>0</nanosec></max_blocking_time>
      </reliability>
      <data_sharing><kind>OFF</kind></data_sharing>
    </qos>
    <topic>
      <historyQos><kind>KEEP_ALL</kind></historyQos>
      <resourceLimitsQos>
        <max_samples>2</max_samples><max_instances>2</max_instances>
        <max_samples_per_instance>1</max_samples_per_instance><allocated_samples>2</allocated_samples>
        <extra_samples>0</extra_samples>
      </resourceLimitsQos>
    </topic>
  </data_writer>
</profiles></dds>)xml";

constexpr std::string_view kLoanHistoryTimeoutXml = R"xml(
<dds xmlns="http://www.eprosima.com"><profiles>
  <data_writer profile_name="dds06_loan_history_timeout" is_default_profile="true">
    <qos>
      <reliability>
        <kind>RELIABLE</kind>
        <max_blocking_time><sec>0</sec><nanosec>100000000</nanosec></max_blocking_time>
      </reliability>
      <data_sharing><kind>OFF</kind></data_sharing>
    </qos>
    <topic>
      <historyQos><kind>KEEP_ALL</kind></historyQos>
      <resourceLimitsQos>
        <max_samples>2</max_samples><max_instances>1</max_instances>
        <max_samples_per_instance>1</max_samples_per_instance><allocated_samples>2</allocated_samples>
      </resourceLimitsQos>
    </topic>
  </data_writer>
</profiles></dds>)xml";

runtime::core::channel::TopicInfo MakeTopicInfo(std::string topic, std::string module) {
  return runtime::core::channel::TopicInfo{
      .msg_type = "dds:example::AddRequest",
      .topic_name = std::move(topic),
      .pkg_path = "dds_channel_test",
      .module_name = std::move(module),
      .index = 1,
      .msg_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetDdsMessageTypeSupport<example::AddRequest>())};
}

struct WrapperTestMessage {
  std::string value;
};

const aimrt_type_support_base_t* GetWrapperTestTypeSupport() {
  static constexpr char kTypeName[] = "pb:aimrt.dds.WrapperTestMessage";
  static const aimrt_string_view_t kSerializationTypes[] = {
      {.str = "pb", .len = 2}};
  static const aimrt_type_support_base_t kTypeSupport{
      .type_name = [](void*) { return aimrt_string_view_t{.str = kTypeName,
                                                          .len = sizeof(kTypeName) - 1}; },
      .create = [](void*) -> void* { return new WrapperTestMessage(); },
      .destroy = [](void*, void* message) { delete static_cast<WrapperTestMessage*>(message); },
      .copy = [](void*, const void* from, void* to) { *static_cast<WrapperTestMessage*>(to) =
                                                          *static_cast<const WrapperTestMessage*>(from); },
      .move = [](void*, void* from, void* to) { *static_cast<WrapperTestMessage*>(to) =
                                                    std::move(*static_cast<WrapperTestMessage*>(from)); },
      .serialize = [](void*, aimrt_string_view_t serialization_type,
                      const void* message,
                      const aimrt_buffer_array_allocator_t* allocator,
                      aimrt_buffer_array_t* output) {
        if (std::string_view(serialization_type.str, serialization_type.len) !=
            "pb")
          return false;
        const auto& value =
            static_cast<const WrapperTestMessage*>(message)->value;
        if (value == "serialize-failure") return false;
        auto buffer = allocator->allocate(allocator->impl, output, value.size());
        if (buffer.data == nullptr && !value.empty()) return false;
        if (!value.empty()) std::memcpy(buffer.data, value.data(), value.size());
        return true; },
      .deserialize = [](void*, aimrt_string_view_t serialization_type,
                        aimrt_buffer_array_view_t input, void* message) {
        if (std::string_view(serialization_type.str, serialization_type.len) !=
            "pb")
          return false;
        std::string value;
        for (size_t index = 0; index < input.len; ++index) {
          value.append(static_cast<const char*>(input.data[index].data),
                       input.data[index].len);
        }
        if (value == "invalid-payload") return false;
        static_cast<WrapperTestMessage*>(message)->value = std::move(value);
        return true; },
      .serialization_types_supported_num = [](void*) { return size_t{1}; },
      .serialization_types_supported_list = [](void*) { return kSerializationTypes; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .impl = nullptr};
  return &kTypeSupport;
}

runtime::core::channel::TopicInfo MakeWrapperTopicInfo(
    std::string topic, std::string module) {
  return runtime::core::channel::TopicInfo{
      .msg_type = "pb:aimrt.dds.WrapperTestMessage",
      .topic_name = std::move(topic),
      .pkg_path = "dds_channel_test",
      .module_name = std::move(module),
      .index = 1,
      .msg_type_support_ref =
          aimrt::util::TypeSupportRef(GetWrapperTestTypeSupport())};
}

template <aimrt::DdsMessageType MsgType>
runtime::core::channel::TopicInfo MakeDdsTopicInfo(std::string topic,
                                                   std::string module) {
  return runtime::core::channel::TopicInfo{
      .msg_type = aimrt::channel::DdsAimrtTypeName<MsgType>(),
      .topic_name = std::move(topic),
      .pkg_path = "dds_channel_test",
      .module_name = std::move(module),
      .index = 1,
      .msg_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetDdsMessageTypeSupport<MsgType>())};
}

class ChannelHarness {
 public:
  ChannelHarness(uint32_t domain, uint32_t threads = 2, std::string fastdds_xml = {}) {
    context_.Initialize(DdsPluginOptions{
        .domain_id = domain,
        .participant_name = "aimrt_dds_channel_test_" + std::to_string(getpid()),
        .fastdds_xml = std::move(fastdds_xml),
        .executor = {.type = "asio_thread", .thread_num = threads}});
    runtime_.Initialize(context_);
    state_ = std::make_shared<DdsBackendState>(context_, runtime_);
    backend_ = std::make_unique<DdsChannelBackend>(state_);
    backend_->Initialize(YAML::Load("{}"));
  }

  ~ChannelHarness() { Stop(); }

  DdsChannelBackend& Backend() { return *backend_; }
  DdsRuntime& Runtime() { return runtime_; }
  DdsParticipantContext& Context() { return context_; }
  std::shared_ptr<DdsBackendState> State() { return state_; }

  void StartRuntime() {
    if (runtime_started_) return;
    context_.Start();
    runtime_.Start();
    runtime_started_ = true;
  }

  void Start() {
    StartRuntime();
    backend_->Start();
    backend_started_ = true;
  }

  void Stop() {
    if (!backend_) return;
    if (backend_started_) backend_->Shutdown();
    runtime_.Shutdown();
    context_.Shutdown();
    backend_started_ = false;
    runtime_started_ = false;
    backend_.reset();
  }

  DataWriter* Writer(std::string_view topic, std::string_view origin = "test writer lookup") {
    return WriterFor<example::AddRequest>(topic, origin);
  }

  template <aimrt::DdsMessageType MsgType>
  DataWriter* WriterFor(std::string_view topic,
                        std::string_view origin = "test writer lookup") {
    return runtime_.Endpoints().GetOrCreateWriter(
        topic, aimrt::GetStaticDdsTypeSupportHandle<MsgType>(),
        context_.Qos().channel_writer, origin);
  }

  DataWriter* SerializedMessageWriter(
      std::string_view topic,
      std::string_view origin = "serialized message writer lookup") {
    return runtime_.Endpoints().GetOrCreateWriter(
        topic, GetSerializedMessageTypeSupport(), context_.Qos().channel_writer,
        origin);
  }

 private:
  DdsParticipantContext context_;
  DdsRuntime runtime_;
  std::shared_ptr<DdsBackendState> state_;
  std::unique_ptr<DdsChannelBackend> backend_;
  bool runtime_started_ = false;
  bool backend_started_ = false;
};

class ExternalReliableReader {
 public:
  ExternalReliableReader(uint32_t domain, std::string_view topic_name,
                         const TypeSupport& type) {
    auto* factory = DomainParticipantFactory::get_instance();
    DomainParticipantQos participant_qos;
    if (factory->get_default_participant_qos(participant_qos) != RETCODE_OK) return;
    transport_ =
        std::make_shared<eprosima::fastdds::rtps::test_UDPv4TransportDescriptor>();
    participant_qos.transport().use_builtin_transports = false;
    participant_qos.transport().user_transports.emplace_back(transport_);
    participant_ = factory->create_participant(domain, participant_qos);
    if (!participant_) return;
    if (type.register_type(participant_) != RETCODE_OK) return;
    topic_ = participant_->create_topic(
        std::string(topic_name), type->get_name(), TOPIC_QOS_DEFAULT);
    if (!topic_) return;
    subscriber_ = participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    if (!subscriber_) return;
    auto qos = DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.representation().m_value = {XCDR2_DATA_REPRESENTATION};
    qos.data_sharing().off();
    reader_ = subscriber_->create_datareader(topic_, qos);
  }

  ~ExternalReliableReader() { Reset(); }

  DataReader* Reader() const { return reader_; }

  void ShutdownNetwork() {
    transport_->dropAckNackMessagesPercentage = 100;
    transport_->test_transport_options->test_UDPv4Transport_ShutdownAllNetwork = true;
  }

  void Reset() {
    if (!participant_) return;
    transport_->dropAckNackMessagesPercentage = 0;
    transport_->test_transport_options->test_UDPv4Transport_ShutdownAllNetwork = false;
    if (reader_) {
      EXPECT_EQ(subscriber_->delete_datareader(reader_), RETCODE_OK);
      reader_ = nullptr;
    }
    if (subscriber_) {
      EXPECT_EQ(participant_->delete_subscriber(subscriber_), RETCODE_OK);
      subscriber_ = nullptr;
    }
    if (topic_) {
      EXPECT_EQ(participant_->delete_topic(topic_), RETCODE_OK);
      topic_ = nullptr;
    }
    EXPECT_EQ(DomainParticipantFactory::get_instance()->delete_participant(participant_),
              RETCODE_OK);
    participant_ = nullptr;
  }

 private:
  std::shared_ptr<eprosima::fastdds::rtps::test_UDPv4TransportDescriptor>
      transport_;
  DomainParticipant* participant_ = nullptr;
  Topic* topic_ = nullptr;
  Subscriber* subscriber_ = nullptr;
  DataReader* reader_ = nullptr;
};

class ExternalSerializedMessageWriter {
 public:
  ExternalSerializedMessageWriter(uint32_t domain, std::string_view topic_name)
      : type_(new aimrt::dds::SerializedMessagePubSubType()) {
    auto* factory = DomainParticipantFactory::get_instance();
    DomainParticipantQos participant_qos;
    if (factory->get_default_participant_qos(participant_qos) != RETCODE_OK)
      return;
    transport_ =
        std::make_shared<eprosima::fastdds::rtps::test_UDPv4TransportDescriptor>();
    participant_qos.transport().use_builtin_transports = false;
    participant_qos.transport().user_transports.emplace_back(transport_);
    participant_ = factory->create_participant(domain, participant_qos);
    if (!participant_) return;
    if (type_.register_type(participant_) != RETCODE_OK) return;
    topic_ = participant_->create_topic(
        std::string(topic_name), type_->get_name(), TOPIC_QOS_DEFAULT);
    if (!topic_) return;
    publisher_ = participant_->create_publisher(PUBLISHER_QOS_DEFAULT);
    if (!publisher_) return;
    auto qos = DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.representation().m_value = {XCDR2_DATA_REPRESENTATION};
    qos.data_sharing().off();
    writer_ = publisher_->create_datawriter(topic_, qos);
  }

  ~ExternalSerializedMessageWriter() { Reset(); }

  DataWriter* Writer() const { return writer_; }

  void Reset() {
    if (!participant_) return;
    if (writer_) {
      EXPECT_EQ(publisher_->delete_datawriter(writer_), RETCODE_OK);
      writer_ = nullptr;
    }
    if (publisher_) {
      EXPECT_EQ(participant_->delete_publisher(publisher_), RETCODE_OK);
      publisher_ = nullptr;
    }
    if (topic_) {
      EXPECT_EQ(participant_->delete_topic(topic_), RETCODE_OK);
      topic_ = nullptr;
    }
    EXPECT_EQ(DomainParticipantFactory::get_instance()->delete_participant(
                  participant_),
              RETCODE_OK);
    participant_ = nullptr;
  }

 private:
  TypeSupport type_;
  std::shared_ptr<eprosima::fastdds::rtps::test_UDPv4TransportDescriptor>
      transport_;
  DomainParticipant* participant_ = nullptr;
  Topic* topic_ = nullptr;
  Publisher* publisher_ = nullptr;
  DataWriter* writer_ = nullptr;
};

bool WaitForWriterMatch(DataWriter* writer, std::chrono::milliseconds timeout = 5s) {
  return WaitFor([writer] {
    PublicationMatchedStatus status;
    return writer->get_publication_matched_status(status) == RETCODE_OK &&
           status.current_count > 0;
  },
                 timeout);
}

class XtypesDiscoveryObserver final : public DomainParticipantListener {
 public:
  explicit XtypesDiscoveryObserver(std::string topic) : topic_(std::move(topic)) {}

  void on_data_writer_discovery(
      DomainParticipant*, eprosima::fastdds::rtps::WriterDiscoveryStatus status,
      const PublicationBuiltinTopicData& info, bool& should_be_ignored) override {
    should_be_ignored = false;
    if (status !=
            eprosima::fastdds::rtps::WriterDiscoveryStatus::DISCOVERED_WRITER ||
        info.topic_name.to_string() != topic_) {
      return;
    }
    std::lock_guard lock(mutex_);
    discovered_ = info;
    condition_.notify_all();
  }

  std::optional<PublicationBuiltinTopicData> Wait(
      std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, timeout, [&] { return discovered_.has_value(); }))
      return std::nullopt;
    return discovered_;
  }

 private:
  std::string topic_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<PublicationBuiltinTopicData> discovered_;
};

int RunXtypesDiscoveryObserver(uint32_t domain, const std::string& topic,
                               const std::filesystem::path& report_path,
                               pid_t publisher_pid, int ready_fd,
                               int continue_fd) {
  alarm(30);
  XtypesDiscoveryObserver listener(topic);
  auto* factory = DomainParticipantFactory::get_instance();
  DomainParticipantQos qos;
  if (factory->get_default_participant_qos(qos) != RETCODE_OK) return 40;
  qos.name("aimrt_dds_xtypes_observer_" + std::to_string(getpid()));
  auto* participant = factory->create_participant(
      domain, qos, &listener, StatusMask::all());
  if (participant == nullptr) return 41;
  const char ready = 'R';
  if (write(ready_fd, &ready, 1) != 1) return 42;
  char proceed = 0;
  if (read(continue_fd, &proceed, 1) != 1 || proceed != 'G') return 43;

  const auto discovered = listener.Wait(15s);
  if (!discovered ||
      discovered->type_name.to_string() != "aimrt_dds_test::NestedSample") {
    factory->delete_participant(participant);
    return 44;
  }
  const auto complete_id = discovered->type_information.type_information
                               .complete()
                               .typeid_with_size()
                               .type_id();
  namespace xtypes = eprosima::fastdds::dds::xtypes;
  xtypes::TypeObject outer_object;
  auto& registry = factory->type_object_registry();
  if (!WaitFor([&] {
        return registry.get_type_object(complete_id, outer_object) == RETCODE_OK;
      },
               10s)) {
    factory->delete_participant(participant);
    return 45;
  }

  const auto& outer_members = outer_object.complete().struct_type().member_seq();
  if (outer_members.size() != 2 ||
      outer_members[0].detail().name() != "inner" ||
      outer_members[1].detail().name() != "values") {
    factory->delete_participant(participant);
    return 46;
  }
  const auto& values_id = outer_members[1].common().member_type_id();
  if (values_id._d() != xtypes::TI_PLAIN_SEQUENCE_SMALL ||
      values_id.seq_sdefn().bound() != 4 ||
      !values_id.seq_sdefn().element_identifier() ||
      values_id.seq_sdefn().element_identifier()->_d() != xtypes::TK_INT32) {
    factory->delete_participant(participant);
    return 47;
  }
  const auto inner_id = outer_members[0].common().member_type_id();
  xtypes::TypeObject inner_object;
  if (!WaitFor([&] {
        return registry.get_type_object(inner_id, inner_object) == RETCODE_OK;
      },
               10s)) {
    factory->delete_participant(participant);
    return 48;
  }
  const auto& inner_members = inner_object.complete().struct_type().member_seq();
  if (inner_members.size() != 1 ||
      inner_members[0].detail().name() != "value" ||
      inner_members[0].common().member_type_id()._d() != xtypes::TK_INT32) {
    factory->delete_participant(participant);
    return 49;
  }

  std::ofstream report(report_path, std::ios::binary);
  report << "observer_pid=" << getpid() << '\n'
         << "publisher_pid=" << publisher_pid << '\n'
         << "type=aimrt_dds_test::NestedSample\n"
         << "members=inner,values\n"
         << "inner_type=aimrt_dds_test::PlainSample\n"
         << "inner_members=value\n"
         << "values_type=sequence<int32,4>\n"
         << "value_type=int32\n"
         << "source=remote_discovery_type_lookup\n";
  const bool report_ok = static_cast<bool>(report);
  report.close();
  const bool cleanup_ok = factory->delete_participant(participant) == RETCODE_OK;
  return report_ok && cleanup_ok ? 0 : 50;
}

runtime::core::rpc::FuncInfo MakeStressRpcInfo(std::string module,
                                               uint64_t index) {
  return runtime::core::rpc::FuncInfo{
      .func_name = "dds:/example::Calculator/Add",
      .pkg_path = "dds_stress_test",
      .module_name = std::move(module),
      .index = index,
      .custom_type_support_ptr = nullptr,
      .req_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetDdsMessageTypeSupport<example::AddRequest>()),
      .rsp_type_support_ref = aimrt::util::TypeSupportRef(
          aimrt::GetDdsMessageTypeSupport<example::AddResponse>())};
}

bool InvokeStressRpc(DdsRpcBackend& backend,
                     const runtime::core::rpc::FuncInfo& info, int32_t value) {
  auto request = std::make_shared<example::AddRequest>();
  auto response = std::make_shared<example::AddResponse>();
  request->lhs(value);
  request->rhs(100000 - value);
  auto promise = std::make_shared<std::promise<aimrt::rpc::Status>>();
  auto future = promise->get_future();
  auto callbacks = std::make_shared<std::atomic_uint32_t>(0);
  aimrt::rpc::Context context;
  context.SetTimeout(2s);
  auto invoke = std::make_shared<runtime::core::rpc::InvokeWrapper>(
      runtime::core::rpc::InvokeWrapper{.info = info,
                                        .req_ptr = request.get(),
                                        .rsp_ptr = response.get(),
                                        .ctx_ref = context});
  invoke->callback = [promise, callbacks](aimrt::rpc::Status status) {
    if (callbacks->fetch_add(1) == 0) promise->set_value(std::move(status));
  };
  backend.Invoke(invoke);
  if (future.wait_for(4s) != std::future_status::ready) return false;
  return future.get().OK() && response->sum() == 100000 &&
         callbacks->load() == 1;
}

class TransientBuiltinReader {
 public:
  TransientBuiltinReader(uint32_t domain, std::string_view topic_name,
                         const TypeSupport& type) {
    auto* factory = DomainParticipantFactory::get_instance();
    participant_ = factory->create_participant(domain, PARTICIPANT_QOS_DEFAULT);
    if (participant_ == nullptr || type.register_type(participant_) != RETCODE_OK)
      return;
    topic_ = participant_->create_topic(std::string(topic_name), type->get_name(),
                                        TOPIC_QOS_DEFAULT);
    subscriber_ = participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    auto qos = DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    qos.representation().m_value = {XCDR2_DATA_REPRESENTATION};
    qos.data_sharing().off();
    reader_ = subscriber_ && topic_
                  ? subscriber_->create_datareader(topic_, qos)
                  : nullptr;
  }

  ~TransientBuiltinReader() { cleanup_ok_ = Reset(); }
  DataReader* Reader() const { return reader_; }

  bool Reset() {
    if (participant_ == nullptr) return false;
    bool ok = true;
    if (reader_ != nullptr) {
      ok = subscriber_->delete_datareader(reader_) == RETCODE_OK && ok;
      reader_ = nullptr;
    }
    if (subscriber_ != nullptr) {
      ok = participant_->delete_subscriber(subscriber_) == RETCODE_OK && ok;
      subscriber_ = nullptr;
    }
    if (topic_ != nullptr) {
      ok = participant_->delete_topic(topic_) == RETCODE_OK && ok;
      topic_ = nullptr;
    }
    ok = DomainParticipantFactory::get_instance()->delete_participant(participant_) ==
             RETCODE_OK &&
         ok;
    participant_ = nullptr;
    cleanup_ok_ = ok;
    return ok;
  }

 private:
  DomainParticipant* participant_ = nullptr;
  Topic* topic_ = nullptr;
  Subscriber* subscriber_ = nullptr;
  DataReader* reader_ = nullptr;
  bool cleanup_ok_ = false;
};

int RunAckDroppingReader(uint32_t domain, const std::string& topic,
                         int ready_fd, int continue_fd) {
  alarm(20);
  ExternalReliableReader reader(
      domain, topic,
      aimrt::GetStaticDdsTypeSupportHandle<aimrt_dds_test::KeyedPlainSample>());
  if (reader.Reader() == nullptr) return 30;
  if (!WaitFor([&] {
        SubscriptionMatchedStatus status;
        return reader.Reader()->get_subscription_matched_status(status) == RETCODE_OK &&
               status.current_count > 0;
      },
               12s)) {
    return 31;
  }
  reader.ShutdownNetwork();
  const char ready = 'R';
  if (write(ready_fd, &ready, 1) != 1) return 32;
  char proceed = 0;
  if (read(continue_fd, &proceed, 1) != 1 || proceed != 'G') return 33;
  reader.Reset();
  return 0;
}

void Publish(DdsChannelBackend& backend, const runtime::core::channel::TopicInfo& info,
             int32_t sequence, aimrt::channel::ContextRef context = {}) {
  example::AddRequest message;
  message.lhs(sequence);
  message.rhs(sequence + 1);
  runtime::core::channel::MsgWrapper wrapper{
      .info = info, .msg_ptr = &message, .ctx_ref = context};
  backend.Publish(wrapper);
}

template <typename MsgType>
void PublishDds(DdsChannelBackend& backend,
                const runtime::core::channel::TopicInfo& info,
                const MsgType& message) {
  aimrt::channel::Context context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  context.SetSerializationType("dds_xcdr2");
  runtime::core::channel::MsgWrapper wrapper{
      .info = info, .msg_ptr = &message, .ctx_ref = context};
  backend.Publish(wrapper);
}

int RunExternalWriter(uint32_t domain, const std::string& topic, int32_t sequence,
                      int ready_fd = -1, int continue_fd = -1) {
  alarm(20);
  ChannelHarness harness(domain);
  auto pub_info = MakeTopicInfo(topic, "external_publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  if (!harness.Backend().RegisterPublishType(publisher)) return 10;
  try {
    harness.Start();
  } catch (...) {
    return 11;
  }
  if (!WaitForWriterMatch(harness.Writer(topic), 12s)) return 12;
  aimrt::channel::Context publish_context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  publish_context.SetMetaValue("publisher-unique-key", "must-not-cross-native-dds");
  Publish(harness.Backend(), pub_info, sequence, publish_context);
  if (ready_fd >= 0 && continue_fd >= 0) {
    const char ready = 'R';
    if (write(ready_fd, &ready, 1) != 1) return 13;
    char proceed = 0;
    if (read(continue_fd, &proceed, 1) != 1 || proceed != 'G') return 14;
    Publish(harness.Backend(), pub_info, sequence + 1, publish_context);
  }
  std::this_thread::sleep_for(100ms);
  harness.Stop();
  return 0;
}

int RunLossWriter(uint32_t domain, const std::string& topic) {
  alarm(10);
  auto* factory = DomainParticipantFactory::get_instance();
  DomainParticipantQos participant_qos;
  if (factory->get_default_participant_qos(participant_qos) != RETCODE_OK) return 20;
  auto transport =
      std::make_shared<eprosima::fastdds::rtps::test_UDPv4TransportDescriptor>();
  transport->sequenceNumberDataMessagesToDrop = {
      {0, 2}, {0, 3}, {0, 6}};
  participant_qos.transport().use_builtin_transports = false;
  participant_qos.transport().user_transports.emplace_back(transport);
  auto* participant = factory->create_participant(domain, participant_qos);
  if (participant == nullptr) return 21;

  auto type = aimrt::GetStaticDdsTypeSupportHandle<example::AddRequest>();
  if (type.register_type(participant) != RETCODE_OK) return 22;
  auto* dds_topic = participant->create_topic(topic, type->get_name(), TOPIC_QOS_DEFAULT);
  auto* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
  auto writer_qos = DATAWRITER_QOS_DEFAULT;
  writer_qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
  writer_qos.history().kind = KEEP_LAST_HISTORY_QOS;
  writer_qos.history().depth = 20;
  writer_qos.data_sharing().off();
  writer_qos.representation().m_value = {XCDR2_DATA_REPRESENTATION};
  auto* writer = publisher ? publisher->create_datawriter(dds_topic, writer_qos) : nullptr;
  if (dds_topic == nullptr || publisher == nullptr || writer == nullptr) return 23;
  if (!WaitForWriterMatch(writer)) return 24;

  for (int32_t sequence = 0; sequence < 10; ++sequence) {
    example::AddRequest message;
    message.lhs(sequence);
    message.rhs(sequence + 1);
    if (writer->write(&message) != RETCODE_OK) return 25;
    std::this_thread::sleep_for(2ms);
  }
  std::this_thread::sleep_for(200ms);
  if (publisher->delete_datawriter(writer) != RETCODE_OK ||
      participant->delete_publisher(publisher) != RETCODE_OK ||
      participant->delete_topic(dds_topic) != RETCODE_OK ||
      factory->delete_participant(participant) != RETCODE_OK) {
    return 26;
  }
  return 0;
}

int RunCoreLoanLeakProbe(const std::filesystem::path& runtime_core_test) {
  alarm(20);
  std::vector<std::string> owned_args{
      runtime_core_test.string(),
      "--gtest_filter=ChannelBackendManagerTest."
      "ShutdownRejectsOutstandingPublisherLoan",
      "--gtest_death_test_style=threadsafe"};
  std::vector<char*> argv;
  argv.reserve(owned_args.size() + 1);
  for (auto& argument : owned_args) argv.emplace_back(argument.data());
  argv.emplace_back(nullptr);
  execv(argv.front(), argv.data());
  return 119;
}

struct ProcessResult {
  int status = -1;
  bool process_group_established = false;
  bool term_sent = false;
  bool kill_fallback_sent = false;
  bool child_reaped = false;
  bool no_residual_process_group = false;
};

class ScopedTerminationSignalMask {
 public:
  ScopedTerminationSignalMask() {
    sigemptyset(&termination_signals_);
    sigaddset(&termination_signals_, SIGTERM);
    sigaddset(&termination_signals_, SIGINT);
    active_ = pthread_sigmask(SIG_BLOCK, &termination_signals_, &previous_mask_) == 0;
  }

  ~ScopedTerminationSignalMask() { Restore(); }
  ScopedTerminationSignalMask(const ScopedTerminationSignalMask&) = delete;
  ScopedTerminationSignalMask& operator=(const ScopedTerminationSignalMask&) = delete;

  bool Active() const { return active_; }
  const sigset_t& PreviousMask() const { return previous_mask_; }

  void Restore() {
    if (!active_) return;
    pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
    active_ = false;
  }

 private:
  sigset_t termination_signals_{};
  sigset_t previous_mask_{};
  bool active_ = false;
};

class OwnedProcessGroup {
 public:
  static std::unique_ptr<OwnedProcessGroup> Launch(
      std::string mode, std::vector<std::string> arguments, bool protocol = false,
      std::function<void()> before_registry_publication = {}) {
    int ready_pipe[2] = {-1, -1};
    int continue_pipe[2] = {-1, -1};
    if (protocol && (pipe(ready_pipe) != 0 || pipe(continue_pipe) != 0)) {
      if (ready_pipe[0] >= 0) close(ready_pipe[0]);
      if (ready_pipe[1] >= 0) close(ready_pipe[1]);
      return {};
    }
    auto result = std::unique_ptr<OwnedProcessGroup>(new OwnedProcessGroup());
    ScopedTerminationSignalMask signal_mask;
    if (!signal_mask.Active()) {
      for (const int fd : {ready_pipe[0], ready_pipe[1], continue_pipe[0], continue_pipe[1]}) {
        if (fd >= 0) close(fd);
      }
      return {};
    }
    std::unique_lock registry_lock(RegistryMutex());
    if (RegistrySealed()) {
      for (const int fd : {ready_pipe[0], ready_pipe[1], continue_pipe[0], continue_pipe[1]}) {
        if (fd >= 0) close(fd);
      }
      return {};
    }
    const pid_t child = fork();
    if (child < 0) {
      for (const int fd : {ready_pipe[0], ready_pipe[1], continue_pipe[0], continue_pipe[1]}) {
        if (fd >= 0) close(fd);
      }
      return {};
    }
    if (child == 0) {
      sigprocmask(SIG_SETMASK, &signal_mask.PreviousMask(), nullptr);
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() == 1 || setpgid(0, 0) != 0) _exit(124);
      if (protocol) {
        close(ready_pipe[0]);
        close(continue_pipe[1]);
        arguments.emplace_back(std::to_string(ready_pipe[1]));
        arguments.emplace_back(std::to_string(continue_pipe[0]));
      }
      std::vector<std::string> owned_args{"/proc/self/exe", std::move(mode)};
      owned_args.insert(owned_args.end(), arguments.begin(), arguments.end());
      std::vector<char*> argv;
      argv.reserve(owned_args.size() + 1);
      for (auto& argument : owned_args) argv.emplace_back(argument.data());
      argv.emplace_back(nullptr);
      execv(argv.front(), argv.data());
      _exit(127);
    }

    result->child_ = child;
    result->group_ = child;
    result->finished_ = false;
    result->RegisterLocked();
    if (protocol) {
      close(ready_pipe[1]);
      close(continue_pipe[0]);
      result->ready_fd_ = ready_pipe[0];
      result->continue_fd_ = continue_pipe[1];
    }
    const int set_result = setpgid(child, child);
    const int set_error = errno;
    const pid_t observed_group = getpgid(child);
    result->group_established_ = observed_group == child &&
                                 (set_result == 0 || set_error == EACCES || set_error == EPERM);
    if (!result->group_established_) {
      result->Cleanup();
      result->UnregisterLocked();
      registry_lock.unlock();
      signal_mask.Restore();
      return {};
    }
    result->result_.process_group_established = true;
    try {
      if (before_registry_publication) before_registry_publication();
    } catch (...) {
      result->Cleanup();
      result->UnregisterLocked();
      registry_lock.unlock();
      signal_mask.Restore();
      return {};
    }
    registry_lock.unlock();
    signal_mask.Restore();
    return result;
  }

  ~OwnedProcessGroup() {
    Cleanup();
    Unregister();
  }
  OwnedProcessGroup(const OwnedProcessGroup&) = delete;
  OwnedProcessGroup& operator=(const OwnedProcessGroup&) = delete;

  pid_t Pid() const {
    std::lock_guard lock(mutex_);
    return child_;
  }

  void SetBeforeTeardownHookForTesting(std::function<void()> hook) {
    std::lock_guard lock(mutex_);
    before_teardown_hook_ = std::move(hook);
  }

  bool ReadReady(std::chrono::milliseconds timeout = 5s) {
    int ready_fd = -1;
    {
      std::lock_guard lock(mutex_);
      std::swap(ready_fd, ready_fd_);
    }
    if (ready_fd < 0) return false;
    pollfd ready_poll{.fd = ready_fd, .events = POLLIN, .revents = 0};
    char ready = 0;
    const bool result = poll(&ready_poll, 1, static_cast<int>(timeout.count())) == 1 &&
                        read(ready_fd, &ready, 1) == 1 && ready == 'R';
    close(ready_fd);
    return result;
  }

  bool Continue() {
    int continue_fd = -1;
    {
      std::lock_guard lock(mutex_);
      std::swap(continue_fd, continue_fd_);
    }
    if (continue_fd < 0) return false;
    const char proceed = 'G';
    const bool result = write(continue_fd, &proceed, 1) == 1;
    close(continue_fd);
    return result;
  }

  ProcessResult Finish(std::chrono::milliseconds timeout = 8s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock(mutex_);
        if (finished_) return result_;
        const pid_t waited = waitpid(child_, &result_.status, WNOHANG);
        if (waited == child_) {
          child_ = -1;
          result_.child_reaped = true;
          return FinalizeLocked();
        }
        if (waited < 0 && errno != EINTR) {
          if (errno == ECHILD) child_ = -1;
          return FinalizeLocked();
        }
      }
      std::this_thread::sleep_for(10ms);
    }
    std::lock_guard lock(mutex_);
    if (finished_) return result_;
    if (child_ > 0) TerminateAndReapLocked();
    return FinalizeLocked();
  }

  static std::vector<ProcessResult> CleanupActiveForSignal(
      std::atomic_bool* cleanup_started = nullptr) {
    std::lock_guard registry_lock(RegistryMutex());
    RegistrySealed() = true;
    if (cleanup_started) cleanup_started->store(true);
    std::vector<ProcessResult> results;
    results.reserve(Registry().size());
    for (auto* process : Registry()) results.emplace_back(process->Finish(0ms));
    return results;
  }

 private:
  OwnedProcessGroup() = default;

  static std::mutex& RegistryMutex() {
    static std::mutex mutex;
    return mutex;
  }

  static std::vector<OwnedProcessGroup*>& Registry() {
    static std::vector<OwnedProcessGroup*> registry;
    return registry;
  }

  static bool& RegistrySealed() {
    static bool sealed = false;
    return sealed;
  }

  void RegisterLocked() {
    Registry().emplace_back(this);
    registered_ = true;
  }

  void UnregisterLocked() {
    if (!registered_) return;
    std::erase(Registry(), this);
    registered_ = false;
  }

  void Unregister() {
    std::lock_guard lock(RegistryMutex());
    UnregisterLocked();
  }

  static void CloseFd(int& fd) {
    if (fd >= 0) close(fd);
    fd = -1;
  }

  void CloseProtocol() {
    CloseFd(ready_fd_);
    CloseFd(continue_fd_);
  }

  void TerminateAndReapLocked() noexcept {
    if (child_ > 0) {
      const int result = kill(group_established_ ? -group_ : child_, SIGTERM);
      result_.term_sent = result == 0;
    }
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (child_ > 0 && std::chrono::steady_clock::now() < deadline) {
      const pid_t waited = waitpid(child_, &result_.status, WNOHANG);
      if (waited == child_) {
        child_ = -1;
        result_.child_reaped = true;
        return;
      }
      if (waited < 0 && errno != EINTR) {
        if (errno == ECHILD) child_ = -1;
        return;
      }
      std::this_thread::sleep_for(10ms);
    }
    if (child_ > 0) {
      const int result = kill(group_established_ ? -group_ : child_, SIGKILL);
      result_.kill_fallback_sent = result == 0;
    }
    if (child_ > 0) {
      pid_t waited = -1;
      do {
        waited = waitpid(child_, &result_.status, 0);
      } while (waited < 0 && errno == EINTR);
      if (waited == child_) {
        result_.child_reaped = true;
      }
      child_ = -1;
    }
  }

  bool WaitForNoResidualGroup() const noexcept {
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    do {
      if (kill(-group_, 0) < 0 && errno == ESRCH) return true;
      std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return kill(-group_, 0) < 0 && errno == ESRCH;
  }

  bool EnsureNoResidualGroup() const noexcept {
    if (WaitForNoResidualGroup()) return true;
    kill(-group_, SIGTERM);
    if (WaitForNoResidualGroup()) return true;
    kill(-group_, SIGKILL);
    return WaitForNoResidualGroup();
  }

  ProcessResult FinalizeLocked() noexcept {
    CloseProtocol();
    result_.no_residual_process_group = EnsureNoResidualGroup();
    finished_ = true;
    return result_;
  }

  ProcessResult Cleanup() noexcept {
    std::function<void()> teardown_hook;
    {
      std::lock_guard lock(mutex_);
      teardown_hook = std::move(before_teardown_hook_);
    }
    if (teardown_hook) {
      try {
        teardown_hook();
      } catch (...) {
      }
    }
    try {
      const auto result = Finish(0ms);
      if (group_ > 0 && (!result.child_reaped || !result.no_residual_process_group)) {
        std::terminate();
      }
      return result;
    } catch (...) {
      std::terminate();
    }
  }

  mutable std::mutex mutex_;
  pid_t child_ = -1;
  pid_t group_ = -1;
  int ready_fd_ = -1;
  int continue_fd_ = -1;
  bool group_established_ = false;
  bool registered_ = false;
  bool finished_ = true;
  std::function<void()> before_teardown_hook_;
  ProcessResult result_;
};

enum class LoanTransition { kBorrow,
                            kPublish,
                            kDiscard };

void VerifyLoanTransitionDeletionOverlap(uint32_t domain_offset,
                                         LoanTransition transition) {
  ChannelHarness harness(TestDomain(domain_offset));
  auto lifetime = std::make_shared<DdsListenerLifetimeTracker>();
  harness.Runtime().Endpoints().SetListenerLifetimeTracker(lifetime);
  auto pub_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/loan_transition/" + std::to_string(domain_offset), "publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  harness.Start();
  runtime::core::channel::BackendLoanedPublisher route;
  ASSERT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);

  aimrt_channel_loaned_message_base_t message;
  if (transition != LoanTransition::kBorrow) {
    ASSERT_EQ(route.borrow(route.impl, message), AIMRT_CHANNEL_LOAN_STATUS_OK);
  }

  std::promise<void> entered_promise;
  auto entered = entered_promise.get_future();
  std::promise<void> release_promise;
  auto release = release_promise.get_future().share();
  const auto hook = [&] {
    entered_promise.set_value();
    release.wait();
  };
  switch (transition) {
    case LoanTransition::kBorrow:
      harness.Backend().SetBeforeLoanSampleHookForTesting(hook);
      break;
    case LoanTransition::kPublish:
      harness.Backend().SetBeforeLoanedWriteHookForTesting(hook);
      break;
    case LoanTransition::kDiscard:
      harness.Backend().SetBeforeDiscardLoanHookForTesting(hook);
      break;
  }

  auto operation = std::async(std::launch::async, [&] {
    if (transition == LoanTransition::kBorrow) {
      return route.borrow(route.impl, message);
    }
    if (transition == LoanTransition::kPublish) {
      aimrt::channel::Context context(
          aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
      return route.publish(route.impl, context, message);
    }
    return aimrt::channel::ReleaseLoanedMessage(message);
  });

  const bool operation_entered =
      entered.wait_for(5s) == std::future_status::ready;
  EXPECT_TRUE(operation_entered);
  if (!operation_entered) {
    release_promise.set_value();
    operation.wait();
    return;
  }

  auto shutdown =
      std::async(std::launch::async, [&] { harness.Runtime().Shutdown(); });
  EXPECT_TRUE(WaitFor(
      [&] { return harness.Runtime().State() == DdsRuntimeState::kStopping; }));
  EXPECT_EQ(shutdown.wait_for(100ms), std::future_status::timeout);
  {
    std::lock_guard lock(lifetime->mutex);
    EXPECT_TRUE(std::ranges::none_of(lifetime->events, [](const auto& event) {
      return event.starts_with("writer_delete_begin:");
    }));
  }

  release_promise.set_value();
  ASSERT_EQ(operation.wait_for(5s), std::future_status::ready);
  const auto operation_status = operation.get();
  EXPECT_EQ(operation_status,
            transition == LoanTransition::kBorrow
                ? AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE
                : AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(message.msg_ptr, nullptr);
  ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
  shutdown.get();
  {
    std::lock_guard lock(lifetime->mutex);
    const auto begin = std::ranges::find_if(lifetime->events, [](const auto& event) {
      return event.starts_with("writer_delete_begin:");
    });
    const auto complete = std::ranges::find_if(lifetime->events, [](const auto& event) {
      return event.starts_with("writer_delete_ok:");
    });
    EXPECT_NE(begin, lifetime->events.end());
    EXPECT_NE(complete, lifetime->events.end());
    EXPECT_LT(std::distance(lifetime->events.begin(), begin),
              std::distance(lifetime->events.begin(), complete));
  }
  harness.Stop();
}

volatile sig_atomic_t g_launcher_signal_write_fd = -1;
volatile sig_atomic_t g_launcher_signal_received = 0;
std::atomic_bool g_launcher_signal_cleanup_started = false;
int g_launcher_signal_read_fd = -1;
std::thread g_launcher_signal_thread;
struct sigaction g_previous_sigterm_action {};
struct sigaction g_previous_sigint_action {};

void LauncherSignalHandler(int signal_number) {
  const int saved_errno = errno;
  g_launcher_signal_received = signal_number;
  const int signal_fd = g_launcher_signal_write_fd;
  if (signal_fd >= 0) {
    const unsigned char signal_byte = static_cast<unsigned char>(signal_number);
    while (write(signal_fd, &signal_byte, sizeof(signal_byte)) < 0 && errno == EINTR) {
    }
  }
  errno = saved_errno;
}

bool StartLauncherSignalRelay(std::optional<std::filesystem::path> report_path = std::nullopt) {
  if (g_launcher_signal_thread.joinable()) return false;
  int signal_pipe[2] = {-1, -1};
  if (pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK) != 0) return false;
  g_launcher_signal_read_fd = signal_pipe[0];
  g_launcher_signal_write_fd = signal_pipe[1];
  g_launcher_signal_received = 0;
  g_launcher_signal_cleanup_started = false;

  struct sigaction action {};
  sigemptyset(&action.sa_mask);
  action.sa_handler = LauncherSignalHandler;
  if (sigaction(SIGTERM, &action, &g_previous_sigterm_action) != 0) {
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    g_launcher_signal_read_fd = -1;
    g_launcher_signal_write_fd = -1;
    return false;
  }
  if (sigaction(SIGINT, &action, &g_previous_sigint_action) != 0) {
    sigaction(SIGTERM, &g_previous_sigterm_action, nullptr);
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    g_launcher_signal_read_fd = -1;
    g_launcher_signal_write_fd = -1;
    return false;
  }

  try {
    g_launcher_signal_thread = std::thread(
        [read_fd = signal_pipe[0], report_path = std::move(report_path)] {
          sigset_t termination_signals;
          sigemptyset(&termination_signals);
          sigaddset(&termination_signals, SIGTERM);
          sigaddset(&termination_signals, SIGINT);
          pthread_sigmask(SIG_BLOCK, &termination_signals, nullptr);

          unsigned char signal_byte = 0;
          while (true) {
            pollfd signal_poll{.fd = read_fd, .events = POLLIN, .revents = 0};
            if (poll(&signal_poll, 1, -1) < 0) {
              if (errno == EINTR) continue;
              _exit(122);
            }
            if (read(read_fd, &signal_byte, sizeof(signal_byte)) == sizeof(signal_byte)) break;
          }

          if (signal_byte == 0) {
            sigaction(SIGTERM, &g_previous_sigterm_action, nullptr);
            sigaction(SIGINT, &g_previous_sigint_action, nullptr);
            g_launcher_signal_write_fd = -1;
            close(read_fd);
            g_launcher_signal_read_fd = -1;
            return;
          }

          const auto results = OwnedProcessGroup::CleanupActiveForSignal(
              &g_launcher_signal_cleanup_started);
          bool cleanup_complete = true;
          if (report_path) {
            bool group_established = !results.empty();
            bool term_sent = !results.empty();
            bool kill_sent = !results.empty();
            bool child_reaped = !results.empty();
            bool no_residual = !results.empty();
            for (const auto& result : results) {
              group_established = group_established && result.process_group_established;
              term_sent = term_sent && result.term_sent;
              kill_sent = kill_sent && result.kill_fallback_sent;
              child_reaped = child_reaped && result.child_reaped;
              no_residual = no_residual && result.no_residual_process_group;
            }
            cleanup_complete = child_reaped && no_residual;
            std::ofstream report(*report_path);
            report << "received_signal=" << static_cast<int>(signal_byte) << '\n'
                   << "registered_process_groups=" << results.size() << '\n'
                   << "process_group_established=" << group_established << '\n'
                   << "term_sent=" << term_sent << '\n'
                   << "kill_fallback_sent=" << kill_sent << '\n'
                   << "child_reaped=" << child_reaped << '\n'
                   << "no_residual_process_group=" << no_residual << '\n'
                   << "cleanup_complete=" << cleanup_complete << '\n';
            report.close();
          } else {
            for (const auto& result : results) {
              cleanup_complete = cleanup_complete && result.child_reaped &&
                                 result.no_residual_process_group;
            }
          }

          if (!cleanup_complete) _exit(120);
          const auto& previous_action = signal_byte == SIGTERM
                                            ? g_previous_sigterm_action
                                            : g_previous_sigint_action;
          sigaction(signal_byte, &previous_action, nullptr);
          kill(getpid(), signal_byte);
          _exit(128 + signal_byte);
        });
  } catch (...) {
    sigaction(SIGTERM, &g_previous_sigterm_action, nullptr);
    sigaction(SIGINT, &g_previous_sigint_action, nullptr);
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    g_launcher_signal_read_fd = -1;
    g_launcher_signal_write_fd = -1;
    return false;
  }
  return true;
}

bool StopLauncherSignalRelay() {
  if (!g_launcher_signal_thread.joinable()) return true;
  ScopedTerminationSignalMask signal_mask;
  if (!signal_mask.Active()) return false;
  const int signal_fd = g_launcher_signal_write_fd;
  if (signal_fd < 0) return false;
  const unsigned char stop_byte = 0;
  ssize_t written = -1;
  do {
    written = write(signal_fd, &stop_byte, sizeof(stop_byte));
  } while (written < 0 && errno == EINTR);
  if (written != sizeof(stop_byte)) return false;
  g_launcher_signal_thread.join();
  close(signal_fd);
  signal_mask.Restore();
  return true;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

volatile sig_atomic_t g_stubborn_term_fd = -1;

void StubbornTermHandler(int) {
  const int saved_errno = errno;
  const int term_fd = g_stubborn_term_fd;
  if (term_fd >= 0) {
    constexpr char marker[] = "TERM\n";
    while (write(term_fd, marker, sizeof(marker) - 1) < 0 && errno == EINTR) {
    }
  }
  errno = saved_errno;
}

int RunStubbornChild(const std::filesystem::path& term_path,
                     const std::filesystem::path& ready_path) {
  const int term_fd = open(term_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (term_fd < 0) return 121;
  g_stubborn_term_fd = term_fd;
  struct sigaction action {};
  sigemptyset(&action.sa_mask);
  action.sa_handler = StubbornTermHandler;
  if (sigaction(SIGTERM, &action, nullptr) != 0) return 120;
  {
    std::ofstream ready(ready_path);
    ready << "ready\n";
  }
  while (true) pause();
}

int RunSignalCleanupLauncherProbe(std::string_view window,
                                  const std::filesystem::path& report_path,
                                  const std::filesystem::path& launcher_ready_path,
                                  const std::filesystem::path& child_ready_path,
                                  const std::filesystem::path& term_path,
                                  const std::filesystem::path& signal_sent_path) {
  if (!StartLauncherSignalRelay(report_path)) return 119;
  const auto publish_signal_window = [&] {
    std::ofstream ready(launcher_ready_path);
    ready << "ready\n";
    ready.close();
    if (!WaitFor([&] { return std::filesystem::exists(signal_sent_path); })) {
      throw std::runtime_error("signal injection was not acknowledged");
    }
  };

  std::function<void()> publication_hook;
  if (window == "launch-publication") {
    publication_hook = [&] {
      if (!WaitFor([&] { return std::filesystem::exists(child_ready_path); })) {
        throw std::runtime_error("stubborn child did not become ready");
      }
      publish_signal_window();
    };
  }
  auto child = OwnedProcessGroup::Launch(
      "--dds-stubborn-child", {term_path.string(), child_ready_path.string()}, false,
      std::move(publication_hook));
  if (!child) return 118;
  if (!WaitFor([&] { return std::filesystem::exists(child_ready_path); })) return 117;

  if (window == "steady-state") {
    publish_signal_window();
  } else if (window == "teardown") {
    child->SetBeforeTeardownHookForTesting([&] {
      publish_signal_window();
      if (!WaitFor([&] { return g_launcher_signal_cleanup_started.load(); })) {
        throw std::runtime_error("signal cleanup did not acquire ownership registry");
      }
    });
    child.reset();
  } else if (window != "launch-publication") {
    return 116;
  }
  while (true) pause();
}

TEST(DdsChannelRoundtrip, SingleAndIndependentProcessPreservePayloadAndLocalContextOnly) {
  const auto domain = TestDomain(20);
  ChannelHarness harness(domain);
  auto pub_info = MakeTopicInfo("dds05/h01", "publisher");
  auto sub_info = MakeTopicInfo("dds05/h01", "subscriber");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};

  std::mutex mutex;
  std::condition_variable condition;
  std::vector<int32_t> values;
  bool context_ok = true;
  runtime::core::channel::SubscribeWrapper subscriber{
      .info = sub_info,
      .callback = [&](runtime::core::channel::MsgWrapper& message, std::function<void()>&&) {
        const auto* typed = static_cast<const example::AddRequest*>(message.msg_ptr);
        std::lock_guard lock(mutex);
        values.emplace_back(typed->lhs());
        context_ok = context_ok &&
                     message.ctx_ref.GetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_BACKEND) == "dds" &&
                     message.ctx_ref.GetSerializationType() == "dds_xcdr2" &&
                     message.ctx_ref.GetMetaValue("publisher-unique-key").empty() &&
                     message.info.topic_name == "dds05/h01";
        condition.notify_all();
      }};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  ASSERT_TRUE(harness.Backend().Subscribe(subscriber));
  harness.Start();
  ASSERT_TRUE(WaitForWriterMatch(harness.Writer(pub_info.topic_name)));

  aimrt::channel::Context publish_context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  publish_context.SetMetaValue("publisher-unique-key", "must-not-cross-native-dds");
  Publish(harness.Backend(), pub_info, 7, publish_context);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, 5s, [&] { return values.size() >= 1; }));
  }

  auto child = OwnedProcessGroup::Launch(
      "--dds-external-writer",
      {std::to_string(domain), pub_info.topic_name, "9"});
  ASSERT_NE(child, nullptr);
  bool received_external = false;
  {
    std::unique_lock lock(mutex);
    received_external = condition.wait_for(lock, 15s, [&] { return values.size() >= 2; });
  }
  const auto result = child->Finish();
  ASSERT_TRUE(received_external);
  EXPECT_TRUE(result.process_group_established);
  ASSERT_TRUE(WIFEXITED(result.status));
  EXPECT_EQ(WEXITSTATUS(result.status), 0);
  EXPECT_TRUE(result.no_residual_process_group);
  EXPECT_TRUE(context_ok);
  EXPECT_EQ(values, (std::vector<int32_t>{7, 9}));
}

TEST(DdsXtypesCrossProcess,
     DiscoversNativeBusinessTypeAndNestedTypeObjectWithoutSharedRegistry) {
#if !defined(AIMRT_DDS_ENABLE_XTYPES)
  GTEST_SKIP() << "XTypes support is disabled in this build";
#else
  const auto domain = TestDomain(126);
  const std::string topic = "dds10/x01/native_nested";
  TemporaryPath report("aimrt_dds_xtypes_report");
  auto observer = OwnedProcessGroup::Launch(
      "--dds-xtypes-observer",
      {std::to_string(domain), topic, report.Path().string(),
       std::to_string(getpid())},
      true);
  ASSERT_NE(observer, nullptr);
  ASSERT_TRUE(observer->ReadReady(5s));

  ChannelHarness harness(domain);
  auto info = MakeDdsTopicInfo<aimrt_dds_test::NestedSample>(
      topic, "xtypes_native_publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  harness.Start();
  ASSERT_NE(harness.WriterFor<aimrt_dds_test::NestedSample>(topic), nullptr);
  ASSERT_TRUE(observer->Continue());

  const auto result = observer->Finish(20s);
  ASSERT_TRUE(WIFEXITED(result.status));
  EXPECT_EQ(WEXITSTATUS(result.status), 0);
  EXPECT_TRUE(result.process_group_established);
  EXPECT_TRUE(result.child_reaped);
  EXPECT_TRUE(result.no_residual_process_group);

  std::ifstream input(report.Path(), std::ios::binary);
  const std::string evidence((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  EXPECT_NE(evidence.find("observer_pid="), std::string::npos);
  EXPECT_NE(evidence.find("publisher_pid=" + std::to_string(getpid())),
            std::string::npos);
  EXPECT_EQ(evidence.find("observer_pid=" + std::to_string(getpid())),
            std::string::npos);
  EXPECT_NE(evidence.find("type=aimrt_dds_test::NestedSample"),
            std::string::npos);
  EXPECT_NE(evidence.find("members=inner,values"), std::string::npos);
  EXPECT_NE(evidence.find("inner_type=aimrt_dds_test::PlainSample"),
            std::string::npos);
  EXPECT_NE(evidence.find("inner_members=value"), std::string::npos);
  EXPECT_NE(evidence.find("values_type=sequence<int32,4>"),
            std::string::npos);
  EXPECT_NE(evidence.find("value_type=int32"), std::string::npos);
  EXPECT_NE(evidence.find("source=remote_discovery_type_lookup"),
            std::string::npos);
#endif
}

TEST(DdsStress,
     MixesChannelRpcLoanMultiServerAndRepeatedMatchingForThirtySeconds) {
  const auto domain = TestDomain(127);
  TemporaryXml xml("dds10_stress", kDataSharingOffXml);
  ChannelHarness harness(domain, 4, xml.Path());
  auto rpc = std::make_unique<DdsRpcBackend>(harness.State());
  rpc->Initialize(YAML::Load("{}"));

  const auto channel_pub = MakeTopicInfo("dds10/s02/channel", "stress_publisher");
  const auto channel_sub = MakeTopicInfo("dds10/s02/channel", "stress_subscriber");
  runtime::core::channel::PublishTypeWrapper channel_publisher{
      .info = channel_pub};
  std::atomic_uint64_t channel_received = 0;
  std::atomic_int32_t channel_last = -1;
  std::atomic_uint64_t channel_order_errors = 0;
  runtime::core::channel::SubscribeWrapper channel_subscriber{
      .info = channel_sub,
      .callback = [&](runtime::core::channel::MsgWrapper& message,
                      std::function<void()>&&) {
        const auto value =
            static_cast<const example::AddRequest*>(message.msg_ptr)->lhs();
        const auto previous = channel_last.exchange(value);
        if (value <= previous) ++channel_order_errors;
        ++channel_received;
      }};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(channel_publisher));
  ASSERT_TRUE(harness.Backend().Subscribe(channel_subscriber));

  const auto loan_pub = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds10/s02/loan", "stress_loan_publisher");
  const auto loan_sub = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds10/s02/loan", "stress_loan_subscriber");
  runtime::core::channel::PublishTypeWrapper loan_publisher{.info = loan_pub};
  std::atomic_uint64_t loan_received = 0;
  std::atomic_int32_t loan_last = -1;
  std::atomic_uint64_t loan_order_errors = 0;
  runtime::core::channel::LoanedSubscribeWrapper loan_subscriber{
      .info = loan_sub,
      .callback = [&](aimrt::channel::ContextRef, const void* sample) {
        const auto value =
            static_cast<const aimrt_dds_test::PlainSample*>(sample)->value();
        const auto previous = loan_last.exchange(value);
        if (value <= previous) ++loan_order_errors;
        ++loan_received;
      }};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(loan_publisher));
  ASSERT_EQ(harness.Backend().SubscribeLoaned(loan_subscriber),
            AIMRT_CHANNEL_LOAN_STATUS_OK);

  const auto server_one = MakeStressRpcInfo("stress_server_one", 1);
  const auto server_two = MakeStressRpcInfo("stress_server_two", 2);
  const auto client = MakeStressRpcInfo("stress_client", 3);
  std::atomic_uint64_t server_calls = 0;
  const auto service = [&server_calls](
                           const std::shared_ptr<runtime::core::rpc::InvokeWrapper>&
                               invoke) {
    ++server_calls;
    const auto& request =
        *static_cast<const example::AddRequest*>(invoke->req_ptr);
    static_cast<example::AddResponse*>(invoke->rsp_ptr)
        ->sum(request.lhs() + request.rhs());
    invoke->callback(aimrt::rpc::Status());
  };
  ASSERT_TRUE(rpc->RegisterServiceFunc(
      runtime::core::rpc::ServiceFuncWrapper{.info = server_one,
                                             .service_func = service}));
  ASSERT_TRUE(rpc->RegisterServiceFunc(
      runtime::core::rpc::ServiceFuncWrapper{.info = server_two,
                                             .service_func = service}));
  ASSERT_TRUE(rpc->RegisterClientFunc(
      runtime::core::rpc::ClientFuncWrapper{.info = client}));

  harness.Start();
  rpc->Start();
  ASSERT_TRUE(WaitFor([&] {
    return rpc->ReadinessForTesting(client) == "MATCHED" &&
           rpc->DiagnosticsForTesting().rpc_matched_servers == 2;
  }));
  ASSERT_TRUE(WaitForWriterMatch(harness.Writer(channel_pub.topic_name)));
  ASSERT_TRUE(WaitForWriterMatch(
      harness.WriterFor<aimrt_dds_test::PlainSample>(loan_pub.topic_name)));
  runtime::core::channel::BackendLoanedPublisher loan_route;
  ASSERT_EQ(harness.Backend().PrepareLoanedPublisher(loan_publisher, loan_route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);

  std::atomic_uint64_t channel_published = 0;
  std::atomic_uint64_t loan_published = 0;
  std::atomic_uint64_t rpc_succeeded = 0;
  std::atomic_uint64_t errors = 0;
  std::atomic_uint64_t matcher_cycles = 0;
  std::atomic_int32_t rpc_value = 0;
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + 30s;

  std::thread channel_thread([&] {
    int32_t value = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      Publish(harness.Backend(), channel_pub, value++);
      ++channel_published;
      std::this_thread::sleep_for(1ms);
    }
  });
  std::thread loan_thread([&] {
    int32_t value = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      aimrt_channel_loaned_message_base_t message;
      if (loan_route.borrow(loan_route.impl, message) !=
              AIMRT_CHANNEL_LOAN_STATUS_OK ||
          message.msg_ptr == nullptr) {
        ++errors;
        continue;
      }
      static_cast<aimrt_dds_test::PlainSample*>(message.msg_ptr)->value(value++);
      aimrt::channel::Context context(
          aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
      if (loan_route.publish(loan_route.impl, context, message) !=
              AIMRT_CHANNEL_LOAN_STATUS_OK ||
          message.msg_ptr != nullptr) {
        ++errors;
      } else {
        ++loan_published;
      }
      std::this_thread::sleep_for(2ms);
    }
  });
  std::vector<std::thread> rpc_threads;
  for (size_t index = 0; index < 3; ++index) {
    rpc_threads.emplace_back([&] {
      while (std::chrono::steady_clock::now() < deadline) {
        if (InvokeStressRpc(*rpc, client, rpc_value.fetch_add(1)))
          ++rpc_succeeded;
        else
          ++errors;
      }
    });
  }
  std::thread matcher_thread([&] {
    while (std::chrono::steady_clock::now() < deadline) {
      TransientBuiltinReader reader(
          domain, channel_pub.topic_name,
          aimrt::GetStaticDdsTypeSupportHandle<example::AddRequest>());
      if (reader.Reader() == nullptr ||
          !WaitFor([&] {
            SubscriptionMatchedStatus status;
            return reader.Reader()->get_subscription_matched_status(status) ==
                       RETCODE_OK &&
                   status.current_count > 0;
          },
                   2s)) {
        ++errors;
      }
      std::this_thread::sleep_for(50ms);
      if (!reader.Reset()) ++errors;
      ++matcher_cycles;
      std::this_thread::sleep_for(20ms);
    }
  });

  channel_thread.join();
  loan_thread.join();
  for (auto& thread : rpc_threads) thread.join();
  matcher_thread.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  ASSERT_TRUE(WaitFor([&] {
    return channel_received.load() == channel_published.load() &&
           loan_received.load() == loan_published.load() &&
           rpc->PendingCountForTesting(client) == 0;
  }));

  const auto diagnostics = rpc->DiagnosticsForTesting();
  EXPECT_GE(elapsed, 30s);
  EXPECT_EQ(errors.load(), 0U);
  EXPECT_EQ(channel_order_errors.load(), 0U);
  EXPECT_EQ(loan_order_errors.load(), 0U);
  EXPECT_GT(channel_published.load(), 1000U);
  EXPECT_GT(loan_published.load(), 1000U);
  EXPECT_GT(rpc_succeeded.load(), 100U);
  EXPECT_EQ(server_calls.load(), rpc_succeeded.load() * 2);
  EXPECT_EQ(diagnostics.rpc_duplicate_response_total, rpc_succeeded.load());
  EXPECT_EQ(diagnostics.rpc_pending_rejected_total, 0U);
  EXPECT_EQ(diagnostics.rpc_unknown_correlation_total, 0U);
  EXPECT_GE(matcher_cycles.load(), 10U);
  EXPECT_GE(diagnostics.writer_match_events, matcher_cycles.load() * 2);
  EXPECT_EQ(harness.Backend().LoanReturnCountForTesting(
                loan_sub.topic_name, loan_sub.msg_type),
            loan_published.load());

  rpc->Shutdown();
  harness.Stop();
}

TEST(DdsWrapperValidation, PreservesChannelMetadataAndDropsInvalidWireSamples) {
#if !defined(AIMRT_BUILD_WITH_PROTOBUF)
  GTEST_SKIP() << "protobuf adapter is disabled in this build";
#else
  ScopedLogCapture logs;
  ChannelHarness harness(TestDomain(20));
  auto pub_info = MakeWrapperTopicInfo("dds07/h11", "wrapper_publisher");
  auto sub_info = MakeWrapperTopicInfo("dds07/h11", "wrapper_subscriber");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};

  std::mutex mutex;
  std::condition_variable condition;
  size_t callback_count = 0;
  std::string received_value;
  bool context_ok = false;
  runtime::core::channel::SubscribeWrapper subscriber{
      .info = sub_info,
      .callback = [&](runtime::core::channel::MsgWrapper& message,
                      std::function<void()>&&) {
        const auto* typed =
            static_cast<const WrapperTestMessage*>(message.msg_ptr);
        std::lock_guard lock(mutex);
        ++callback_count;
        received_value = typed->value;
        context_ok =
            message.ctx_ref.GetMetaValue("trace-id") == "trace-42" &&
            message.ctx_ref.GetMetaValue("binary-safe-text") == "value" &&
            message.ctx_ref.GetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_BACKEND) ==
                "dds" &&
            message.ctx_ref.GetSerializationType() == "pb";
        condition.notify_all();
      }};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  ASSERT_TRUE(harness.Backend().Subscribe(subscriber));
  runtime::core::channel::LoanedSubscribeWrapper loaned_subscriber{
      .info = sub_info,
      .callback = [](aimrt::channel::ContextRef, const void*) {}};
  EXPECT_EQ(harness.Backend().SubscribeLoaned(loaned_subscriber),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);
  harness.Start();
  auto* writer = harness.SerializedMessageWriter(pub_info.topic_name);
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(WaitForWriterMatch(writer));

  std::atomic_uint64_t write_attempts = 0;
  harness.Backend().SetBeforeWriteHookForTesting([&] { ++write_attempts; });
  const auto write_failures_before =
      harness.Runtime().Diagnostics()->Snapshot().channel_write_failure_total;
  aimrt::channel::Context excessive_metadata_context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  for (size_t index = 0; index <= kSerializedMessageMetadataMaxEntries;
       ++index) {
    excessive_metadata_context.SetMetaValue("key-" + std::to_string(index),
                                            "value");
  }
  WrapperTestMessage excessive_metadata_message{.value = "not-written"};
  runtime::core::channel::MsgWrapper excessive_metadata_wrapper{
      .info = pub_info,
      .msg_ptr = &excessive_metadata_message,
      .ctx_ref = excessive_metadata_context};
  harness.Backend().Publish(excessive_metadata_wrapper);
  EXPECT_EQ(harness.Runtime()
                .Diagnostics()
                ->Snapshot()
                .channel_write_failure_total,
            write_failures_before + 1);
  EXPECT_EQ(write_attempts.load(), 0U);
  EXPECT_TRUE(logs.Contains(AIMRT_LOG_LEVEL_ERROR,
                            "aimrt_dds_channel_write_failure_total=1",
                            "metadata exceeds 64 entries"));

  aimrt::channel::Context excessive_key_context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  excessive_key_context.SetMetaValue(
      std::string(kSerializedMessageKeyMaxBytes + 1, 'k'), "value");
  runtime::core::channel::MsgWrapper excessive_key_wrapper{
      .info = pub_info,
      .msg_ptr = &excessive_metadata_message,
      .ctx_ref = excessive_key_context};
  harness.Backend().Publish(excessive_key_wrapper);
  EXPECT_EQ(harness.Runtime()
                .Diagnostics()
                ->Snapshot()
                .channel_write_failure_total,
            write_failures_before + 2);
  EXPECT_EQ(write_attempts.load(), 0U);
  EXPECT_TRUE(logs.Contains(AIMRT_LOG_LEVEL_ERROR,
                            "aimrt_dds_channel_write_failure_total=2",
                            "metadata key exceeds 256 UTF-8 bytes"));

  aimrt::channel::Context excessive_value_context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  excessive_value_context.SetMetaValue(
      "key", std::string(kSerializedMessageValueMaxBytes + 1, 'v'));
  runtime::core::channel::MsgWrapper excessive_value_wrapper{
      .info = pub_info,
      .msg_ptr = &excessive_metadata_message,
      .ctx_ref = excessive_value_context};
  harness.Backend().Publish(excessive_value_wrapper);
  EXPECT_EQ(harness.Runtime()
                .Diagnostics()
                ->Snapshot()
                .channel_write_failure_total,
            write_failures_before + 3);
  EXPECT_EQ(write_attempts.load(), 0U);

  WrapperTestMessage oversized_message{
      .value = std::string(kSerializedMessageDataMaxBytes + 1, 'x')};
  runtime::core::channel::MsgWrapper oversized_wrapper{
      .info = pub_info, .msg_ptr = &oversized_message};
  harness.Backend().Publish(oversized_wrapper);
  EXPECT_EQ(harness.Runtime()
                .Diagnostics()
                ->Snapshot()
                .channel_write_failure_total,
            write_failures_before + 4);
  EXPECT_EQ(write_attempts.load(), 0U);
  {
    std::lock_guard lock(mutex);
    EXPECT_EQ(callback_count, 0U);
  }

  WrapperTestMessage message{.value = "wrapper-payload"};
  aimrt::channel::Context publish_context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  publish_context.SetSerializationType("pb");
  publish_context.SetMetaValue("trace-id", "trace-42");
  publish_context.SetMetaValue("binary-safe-text", "value");
  runtime::core::channel::MsgWrapper message_wrapper{
      .info = pub_info, .msg_ptr = &message, .ctx_ref = publish_context};
  harness.Backend().Publish(message_wrapper);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, 5s,
                                   [&] { return callback_count == 1; }));
    EXPECT_EQ(received_value, "wrapper-payload");
    EXPECT_TRUE(context_ok);
  }
  EXPECT_EQ(write_attempts.load(), 1U);

  runtime::core::channel::BackendLoanedPublisher loan_route;
  EXPECT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, loan_route),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);

  const auto invalid_before =
      harness.Runtime().Diagnostics()->Snapshot().reader_take_failure_total;
  auto write_invalid = [&](aimrt::dds::SerializedMessage invalid,
                           uint64_t expected_invalid_total) {
    ASSERT_EQ(writer->write(&invalid), RETCODE_OK);
    ASSERT_TRUE(WaitFor([&] {
      return harness.Runtime()
                 .Diagnostics()
                 ->Snapshot()
                 .reader_take_failure_total >= expected_invalid_total;
    }));
    std::lock_guard lock(mutex);
    EXPECT_EQ(callback_count, 1U);
  };
  auto valid_wire_sample = [] {
    aimrt::dds::SerializedMessage sample;
    sample.type_name("pb:aimrt.dds.WrapperTestMessage");
    sample.serialization_type("pb");
    sample.data() = {'v', 'a', 'l', 'i', 'd'};
    return sample;
  };

  auto wrong_type = valid_wire_sample();
  wrong_type.type_name("pb:aimrt.dds.OtherMessage");
  write_invalid(std::move(wrong_type), invalid_before + 1);

  auto wrong_serialization = valid_wire_sample();
  wrong_serialization.serialization_type("ros2");
  write_invalid(std::move(wrong_serialization), invalid_before + 2);

  auto invalid_payload = valid_wire_sample();
  invalid_payload.data() = {'i', 'n', 'v', 'a', 'l', 'i', 'd', '-',
                            'p', 'a', 'y', 'l', 'o', 'a', 'd'};
  write_invalid(std::move(invalid_payload), invalid_before + 3);

  auto local_excessive_metadata = valid_wire_sample();
  local_excessive_metadata.metadata().resize(
      kSerializedMessageMetadataMaxEntries + 1);
  EXPECT_NE(writer->write(&local_excessive_metadata), RETCODE_OK);
  auto local_excessive_data = valid_wire_sample();
  local_excessive_data.data().resize(kSerializedMessageDataMaxBytes + 1);
  EXPECT_NE(writer->write(&local_excessive_data), RETCODE_OK);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(harness.Runtime()
                .Diagnostics()
                ->Snapshot()
                .reader_take_failure_total,
            invalid_before + 3);
  {
    std::lock_guard lock(mutex);
    EXPECT_EQ(callback_count, 1U);
  }

  ExternalSerializedMessageWriter external_writer(TestDomain(20),
                                                  pub_info.topic_name);
  ASSERT_NE(external_writer.Writer(), nullptr);
  ASSERT_TRUE(WaitForWriterMatch(external_writer.Writer()));
  const auto deserialize_rejects_before =
      SerializedMessageDeserializeBoundRejectCount();
  auto write_external_invalid = [&](aimrt::dds::SerializedMessage invalid,
                                    uint64_t expected_wire_rejects,
                                    uint64_t expected_invalid_total) {
    ASSERT_EQ(external_writer.Writer()->write(&invalid), RETCODE_OK);
    ASSERT_TRUE(WaitFor([&] {
      return SerializedMessageDeserializeBoundRejectCount() >=
             expected_wire_rejects;
    }));
    ASSERT_TRUE(WaitFor([&] {
      return harness.Runtime()
                 .Diagnostics()
                 ->Snapshot()
                 .reader_take_failure_total >= expected_invalid_total;
    }));
    std::lock_guard lock(mutex);
    EXPECT_EQ(callback_count, 1U);
  };

  auto excessive_wire_metadata = valid_wire_sample();
  excessive_wire_metadata.metadata().resize(
      kSerializedMessageMetadataMaxEntries + 1);
  write_external_invalid(std::move(excessive_wire_metadata),
                         deserialize_rejects_before + 1,
                         invalid_before + 4);
  EXPECT_TRUE(logs.Contains(
      AIMRT_LOG_LEVEL_ERROR,
      "aimrt_dds_wrapper_invalid_total=" + std::to_string(invalid_before + 4),
      "wrapper type_name does not match the registered endpoint type"));

  auto excessive_wire_data = valid_wire_sample();
  excessive_wire_data.data().resize(kSerializedMessageDataMaxBytes + 1);
  write_external_invalid(std::move(excessive_wire_data),
                         deserialize_rejects_before + 2,
                         invalid_before + 5);

  auto following_valid_sample = valid_wire_sample();
  following_valid_sample.data() = {'e', 'x', 't', 'e', 'r', 'n', 'a', 'l',
                                   '-', 'v', 'a', 'l', 'i', 'd'};
  ASSERT_EQ(external_writer.Writer()->write(&following_valid_sample),
            RETCODE_OK);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, 10s,
                                   [&] { return callback_count == 2; }));
    EXPECT_EQ(received_value, "external-valid");
  }
#endif
}

TEST(DdsChannelMultiModule, AggregatesEndpointsAndDispatchesEveryLogicalWrapper) {
  ChannelHarness harness(TestDomain(21));
  auto pub_a = MakeTopicInfo("dds05/h02", "publisher_a");
  auto pub_b = MakeTopicInfo("dds05/h02", "publisher_b");
  auto sub_a = MakeTopicInfo("dds05/h02", "subscriber_a");
  auto sub_b = MakeTopicInfo("dds05/h02", "subscriber_b");
  runtime::core::channel::PublishTypeWrapper publisher_a{.info = pub_a};
  runtime::core::channel::PublishTypeWrapper publisher_b{.info = pub_b};
  std::atomic_int callback_a = 0;
  std::atomic_int callback_b = 0;
  runtime::core::channel::SubscribeWrapper subscriber_a{
      .info = sub_a,
      .callback = [&](runtime::core::channel::MsgWrapper& message, std::function<void()>&&) {
        EXPECT_NE(message.msg_ptr, nullptr);
        ++callback_a;
      }};
  runtime::core::channel::SubscribeWrapper subscriber_b{
      .info = sub_b,
      .callback = [&](runtime::core::channel::MsgWrapper& message, std::function<void()>&&) {
        EXPECT_TRUE(runtime::core::channel::TryCheckMsg(message));
        EXPECT_EQ(static_cast<const example::AddRequest*>(message.msg_ptr)->lhs(), 22);
        ++callback_b;
      }};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher_a));
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher_b));
  ASSERT_TRUE(harness.Backend().Subscribe(subscriber_a));
  ASSERT_TRUE(harness.Backend().Subscribe(subscriber_b));
  EXPECT_EQ(harness.Runtime().Endpoints().WriterCount(), 1U);
  EXPECT_EQ(harness.Runtime().Endpoints().ReaderCount(), 1U);
  harness.Start();
  ASSERT_TRUE(WaitForWriterMatch(harness.Writer(pub_a.topic_name)));
  Publish(harness.Backend(), pub_b, 22);
  ASSERT_TRUE(WaitFor([&] { return callback_a == 1 && callback_b == 1; }));
}

TEST(DdsChannelOrder, SerializesOneReaderAndDoesNotLoseTheLastWake) {
  TemporaryXml xml("aimrt_dds05_h03", kReliableKeepAllXml);
  ChannelHarness harness(TestDomain(22), 4, xml.Path());
  auto pub_info = MakeTopicInfo("dds05/h03", "publisher");
  auto sub_info = MakeTopicInfo("dds05/h03", "subscriber");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  std::mutex mutex;
  std::vector<int32_t> delivered;
  std::atomic_int active = 0;
  std::atomic_int max_active = 0;
  runtime::core::channel::SubscribeWrapper subscriber{
      .info = sub_info,
      .callback = [&](runtime::core::channel::MsgWrapper& message, std::function<void()>&&) {
        const int now_active = ++active;
        max_active.store(std::max(max_active.load(), now_active));
        std::this_thread::sleep_for(2ms);
        {
          std::lock_guard lock(mutex);
          delivered.emplace_back(static_cast<const example::AddRequest*>(message.msg_ptr)->lhs());
        }
        --active;
      }};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  ASSERT_TRUE(harness.Backend().Subscribe(subscriber));
  harness.Start();
  ASSERT_TRUE(WaitForWriterMatch(harness.Writer(pub_info.topic_name)));
  for (int32_t sequence = 0; sequence < 50; ++sequence) {
    Publish(harness.Backend(), pub_info, sequence);
    if (sequence % 5 == 0) std::this_thread::sleep_for(1ms);
  }
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard lock(mutex);
    return !delivered.empty() && delivered.back() == 49;
  }));
  std::lock_guard lock(mutex);
  std::vector<int32_t> expected(50);
  std::iota(expected.begin(), expected.end(), 0);
  EXPECT_EQ(delivered, expected);
  EXPECT_EQ(max_active, 1);
}

TEST(DdsChannelReaderParallelism, DifferentReadersRunInParallelOffListenerThreads) {
  ChannelHarness harness(TestDomain(23), 2);
  auto pub_a = MakeTopicInfo("dds05/h04/a", "publisher_a");
  auto pub_b = MakeTopicInfo("dds05/h04/b", "publisher_b");
  auto sub_a = MakeTopicInfo("dds05/h04/a", "subscriber_a");
  auto sub_b = MakeTopicInfo("dds05/h04/b", "subscriber_b");
  runtime::core::channel::PublishTypeWrapper publisher_a{.info = pub_a};
  runtime::core::channel::PublishTypeWrapper publisher_b{.info = pub_b};
  std::mutex mutex;
  std::condition_variable condition;
  int entered = 0;
  bool release = false;
  std::atomic_bool workers_only = true;
  auto callback = [&](runtime::core::channel::MsgWrapper&, std::function<void()>&&) {
    workers_only = workers_only && harness.Runtime().Executor()->IsWorkerThread();
    std::unique_lock lock(mutex);
    ++entered;
    condition.notify_all();
    condition.wait_for(lock, 5s, [&] { return release; });
  };
  runtime::core::channel::SubscribeWrapper subscriber_a{.info = sub_a, .callback = callback};
  runtime::core::channel::SubscribeWrapper subscriber_b{.info = sub_b, .callback = callback};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher_a));
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher_b));
  ASSERT_TRUE(harness.Backend().Subscribe(subscriber_a));
  ASSERT_TRUE(harness.Backend().Subscribe(subscriber_b));
  harness.Start();
  ASSERT_TRUE(WaitForWriterMatch(harness.Writer(pub_a.topic_name)));
  ASSERT_TRUE(WaitForWriterMatch(harness.Writer(pub_b.topic_name)));
  Publish(harness.Backend(), pub_a, 1);
  Publish(harness.Backend(), pub_b, 2);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, 5s, [&] { return entered == 2; }));
    release = true;
  }
  condition.notify_all();
  EXPECT_TRUE(workers_only);
}

TEST(DdsChannelStartBoundary, RuntimeCannotDispatchBeforeBackendStart) {
  const auto domain = TestDomain(24);
  ChannelHarness harness(domain);
  auto pub_info = MakeTopicInfo("dds05/h05", "publisher");
  auto sub_info = MakeTopicInfo("dds05/h05", "subscriber");
  std::atomic_int callbacks = 0;
  runtime::core::channel::SubscribeWrapper subscriber{
      .info = sub_info,
      .callback = [&](runtime::core::channel::MsgWrapper&, std::function<void()>&&) {
        ++callbacks;
      }};
  ASSERT_TRUE(harness.Backend().Subscribe(subscriber));
  harness.StartRuntime();
  auto child = OwnedProcessGroup::Launch(
      "--dds-boundary-writer",
      {std::to_string(domain), pub_info.topic_name, "1"}, true);
  ASSERT_NE(child, nullptr);
  const bool ready_read = child->ReadReady(15s);
  std::this_thread::sleep_for(100ms);
  const bool no_callback_before_start = callbacks == 0;
  harness.Backend().Start();
  const bool continued = child->Continue();
  const bool callback_after_start = WaitFor([&] { return callbacks > 0; });
  const auto result = child->Finish();
  EXPECT_TRUE(ready_read);
  EXPECT_TRUE(no_callback_before_start);
  EXPECT_TRUE(continued);
  EXPECT_TRUE(callback_after_start);
  EXPECT_TRUE(result.process_group_established);
  ASSERT_TRUE(WIFEXITED(result.status));
  EXPECT_EQ(WEXITSTATUS(result.status), 0);
  EXPECT_TRUE(result.no_residual_process_group);
}

TEST(DdsChannelStartBoundary, ReceivedSignalUsesUnifiedCleanupBeforeReraise) {
  struct Scenario {
    std::string_view window;
    int signal_number;
  };
  for (const auto& scenario :
       {Scenario{"launch-publication", SIGTERM}, Scenario{"steady-state", SIGINT},
        Scenario{"teardown", SIGTERM}}) {
    SCOPED_TRACE(scenario.window);
    const std::string suffix(scenario.window);
    TemporaryPath report_path("aimrt_dds05_signal_report_" + suffix);
    TemporaryPath launcher_ready_path("aimrt_dds05_launcher_ready_" + suffix);
    TemporaryPath child_ready_path("aimrt_dds05_child_ready_" + suffix);
    TemporaryPath term_path("aimrt_dds05_term_evidence_" + suffix);
    TemporaryPath signal_sent_path("aimrt_dds05_signal_sent_" + suffix);
    auto launcher = OwnedProcessGroup::Launch(
        "--dds-signal-cleanup-launcher",
        {suffix, report_path.Path().string(), launcher_ready_path.Path().string(),
         child_ready_path.Path().string(), term_path.Path().string(),
         signal_sent_path.Path().string()});
    ASSERT_NE(launcher, nullptr);
    ASSERT_TRUE(WaitFor([&] { return std::filesystem::exists(launcher_ready_path.Path()); }));

    const auto signal_started = std::chrono::steady_clock::now();
    ASSERT_EQ(kill(launcher->Pid(), scenario.signal_number), 0);
    {
      std::ofstream signal_sent(signal_sent_path.Path());
      signal_sent << "sent\n";
    }
    const auto result = launcher->Finish(5s);
    const auto signal_elapsed = std::chrono::steady_clock::now() - signal_started;
    const auto report = ReadFile(report_path.Path());
    const auto term_evidence = ReadFile(term_path.Path());

    EXPECT_TRUE(result.process_group_established);
    ASSERT_TRUE(WIFSIGNALED(result.status));
    EXPECT_EQ(WTERMSIG(result.status), scenario.signal_number);
    EXPECT_TRUE(result.child_reaped);
    EXPECT_TRUE(result.no_residual_process_group);
    EXPECT_LT(signal_elapsed, 5s);
    EXPECT_NE(term_evidence.find("TERM"), std::string::npos);
    EXPECT_NE(report.find("received_signal=" + std::to_string(scenario.signal_number)),
              std::string::npos);
    EXPECT_NE(report.find("registered_process_groups=1"), std::string::npos);
    EXPECT_NE(report.find("process_group_established=1"), std::string::npos);
    EXPECT_NE(report.find("term_sent=1"), std::string::npos);
    EXPECT_NE(report.find("kill_fallback_sent=1"), std::string::npos);
    EXPECT_NE(report.find("child_reaped=1"), std::string::npos);
    EXPECT_NE(report.find("no_residual_process_group=1"), std::string::npos);
    EXPECT_NE(report.find("cleanup_complete=1"), std::string::npos);
  }
}

TEST(DdsChannelWriteFailure, CountsInjectedWriteAndMaxBlockingTimeoutFailures) {
  ChannelHarness harness(TestDomain(25));
  auto pub_info = MakeTopicInfo("dds05/h06", "publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  harness.Start();
  harness.Backend().SetWriteResultForTesting(RETCODE_ERROR);
  Publish(harness.Backend(), pub_info, 1);
  harness.Backend().SetWriteResultForTesting(RETCODE_TIMEOUT);
  Publish(harness.Backend(), pub_info, 2);
  EXPECT_EQ(harness.Runtime().Diagnostics()->Snapshot().channel_write_failure_total, 2U);
}

TEST(DdsChannelWriteFailure, ShutdownRejectsNewPublishesAndWaitsForAdmittedWrite) {
  ChannelHarness harness(TestDomain(125));
  auto lifetime = std::make_shared<DdsListenerLifetimeTracker>();
  harness.Runtime().Endpoints().SetListenerLifetimeTracker(lifetime);
  auto pub_info = MakeTopicInfo("dds05/h06/shutdown_race", "publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  harness.Start();

  std::promise<void> admitted_promise;
  auto admitted = admitted_promise.get_future();
  std::promise<void> release_promise;
  auto release = release_promise.get_future().share();
  harness.Backend().SetBeforeWriteHookForTesting([&] {
    admitted_promise.set_value();
    release.wait();
  });

  std::thread publishing([&] { Publish(harness.Backend(), pub_info, 1); });
  const bool write_admitted = admitted.wait_for(5s) == std::future_status::ready;
  auto stopping = std::async(std::launch::async, [&] { harness.Runtime().Shutdown(); });
  const bool entered_stopping = WaitFor(
      [&] { return harness.Runtime().State() == DdsRuntimeState::kStopping; });
  const bool shutdown_waited = stopping.wait_for(100ms) == std::future_status::timeout;

  Publish(harness.Backend(), pub_info, 2);
  release_promise.set_value();
  publishing.join();
  const bool shutdown_finished = stopping.wait_for(5s) == std::future_status::ready;
  if (shutdown_finished) stopping.get();

  EXPECT_TRUE(write_admitted);
  EXPECT_TRUE(entered_stopping);
  EXPECT_TRUE(shutdown_waited);
  EXPECT_TRUE(shutdown_finished);
  EXPECT_EQ(harness.Runtime().Diagnostics()->Snapshot().channel_write_failure_total, 1U);
  std::lock_guard lock(lifetime->mutex);
  EXPECT_NE(std::ranges::find_if(lifetime->events, [](const std::string& event) {
              return event.starts_with("writer_delete_ok:dds05/h06/shutdown_race");
            }),
            lifetime->events.end());
}

TEST(DdsLoanPublisherPlain, FreezesCapabilityAndPreparesOnlyAfterStart) {
  ChannelHarness harness(TestDomain(28));
  auto pub_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/l01", "plain_publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  const auto report = harness.Backend().GenInitializationReport();
  EXPECT_TRUE(std::ranges::any_of(report, [](const auto& entry) {
    return entry.first == "DDS Channel Loan Endpoint" &&
           entry.second.find("topic=dds06/l01") != std::string::npos &&
           entry.second.find("type=dds:aimrt_dds_test::PlainSample") !=
               std::string::npos &&
           entry.second.find("representation=xcdr2, capable=true") !=
               std::string::npos;
  }));

  runtime::core::channel::BackendLoanedPublisher route;
  EXPECT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE);
  EXPECT_EQ(route.impl, nullptr);

  harness.Start();
  ASSERT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  ASSERT_NE(route.impl, nullptr);
  runtime::core::channel::BackendLoanedPublisher cached_route;
  ASSERT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, cached_route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(cached_route.impl, route.impl);

  aimrt_channel_loaned_message_base_t message;
  ASSERT_EQ(route.borrow(route.impl, message), AIMRT_CHANNEL_LOAN_STATUS_OK);
  ASSERT_NE(message.msg_ptr, nullptr);
  static_cast<aimrt_dds_test::PlainSample*>(message.msg_ptr)->value(11);
  aimrt::channel::Context context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  EXPECT_EQ(route.publish(route.impl, context, message),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(message.msg_ptr, nullptr);

  ASSERT_EQ(route.borrow(route.impl, message), AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt::channel::ReleaseLoanedMessage(message);
  EXPECT_EQ(message.msg_ptr, nullptr);
}

TEST(DdsLoanPublisherNonPlain, RejectsFrozenNonPlainCapabilityWithoutBorrow) {
  ChannelHarness harness(TestDomain(29));
  auto pub_info = MakeDdsTopicInfo<aimrt_dds_test::NonPlainSample>(
      "dds06/l02", "non_plain_publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  auto protobuf_info = pub_info;
  protobuf_info.msg_type = "pb:test.Message";
  runtime::core::channel::PublishTypeWrapper protobuf_publisher{
      .info = protobuf_info};
  auto ros2_info = pub_info;
  ros2_info.msg_type = "ros2:test_msgs/msg/Bounded";
  runtime::core::channel::PublishTypeWrapper ros2_publisher{.info = ros2_info};
  runtime::core::channel::BackendLoanedPublisher route;
  EXPECT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE);
  EXPECT_EQ(harness.Backend().PrepareLoanedPublisher(protobuf_publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE);
  EXPECT_EQ(harness.Backend().PrepareLoanedPublisher(ros2_publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE);

  runtime::core::channel::LoanedSubscribeWrapper protobuf_subscriber{
      .info = protobuf_info, .callback = [](aimrt::channel::ContextRef, const void*) {}};
  runtime::core::channel::LoanedSubscribeWrapper ros2_subscriber{
      .info = ros2_info, .callback = [](aimrt::channel::ContextRef, const void*) {}};
  EXPECT_EQ(harness.Backend().SubscribeLoaned(protobuf_subscriber),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);
  EXPECT_EQ(harness.Backend().SubscribeLoaned(ros2_subscriber),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);
  EXPECT_EQ(harness.Runtime().Endpoints().ReaderCount(), 0U);

  harness.Start();
  EXPECT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN);
  EXPECT_EQ(route.impl, nullptr);
  EXPECT_EQ(route.borrow, nullptr);
  EXPECT_EQ(route.publish, nullptr);
  EXPECT_EQ(harness.Backend().PrepareLoanedPublisher(protobuf_publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);
  EXPECT_EQ(route.impl, nullptr);
  EXPECT_EQ(harness.Backend().PrepareLoanedPublisher(ros2_publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);
  EXPECT_EQ(route.impl, nullptr);
  EXPECT_EQ(harness.Backend().LoanSampleCallCountForTesting(), 0U);
}

TEST(DdsLoanPublisherRuntimeFailure, RetainsNonterminalWriteAndDiscardForRetry) {
  ScopedLogCapture logs;
  ChannelHarness harness(TestDomain(30));
  auto pub_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/l03", "plain_publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  runtime::core::channel::BackendLoanedPublisher route;
  EXPECT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE);
  harness.Start();
  ASSERT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);

  aimrt_channel_loaned_message_base_t message;
  harness.Backend().SetLoanSampleResultForTesting(RETCODE_OUT_OF_RESOURCES);
  EXPECT_EQ(route.borrow(route.impl, message),
            AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE);
  EXPECT_EQ(message.msg_ptr, nullptr);
  harness.Backend().SetLoanSampleResultForTesting(RETCODE_ILLEGAL_OPERATION);
  EXPECT_EQ(route.borrow(route.impl, message),
            AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN);
  EXPECT_TRUE(logs.Contains(AIMRT_LOG_LEVEL_ERROR,
                            "loan capability inconsistent", "dds06/l03"));

  harness.Backend().SetLoanSampleResultForTesting(RETCODE_OK);
  ASSERT_EQ(route.borrow(route.impl, message), AIMRT_CHANNEL_LOAN_STATUS_OK);
  const auto* retained_pointer = message.msg_ptr;
  harness.Backend().SetWriteResultForTesting(RETCODE_TIMEOUT);
  aimrt::channel::Context context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  EXPECT_EQ(route.publish(route.impl, context, message),
            AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
  EXPECT_EQ(message.msg_ptr, retained_pointer);
  EXPECT_EQ(harness.Backend().DiscardLoanCallCountForTesting(), 0U);

  harness.Backend().SetDiscardLoanResultForTesting(RETCODE_NOT_ENABLED);
  EXPECT_EQ(aimrt::channel::ReleaseLoanedMessage(message),
            AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
  EXPECT_EQ(message.msg_ptr, retained_pointer);
  harness.Backend().SetDiscardLoanResultForTesting(RETCODE_ILLEGAL_OPERATION);
  EXPECT_EQ(aimrt::channel::ReleaseLoanedMessage(message),
            AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
  EXPECT_EQ(message.msg_ptr, retained_pointer);
  EXPECT_EQ(harness.Backend().DiscardLoanCallCountForTesting(), 2U);

  harness.Backend().SetDiscardLoanResultForTesting(RETCODE_OK);
  EXPECT_EQ(aimrt::channel::ReleaseLoanedMessage(message),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(message.msg_ptr, nullptr);
  EXPECT_EQ(harness.Backend().DiscardLoanCallCountForTesting(), 3U);

  harness.Backend().SetWriteResultForTesting(RETCODE_OK);
  ASSERT_EQ(route.borrow(route.impl, message), AIMRT_CHANNEL_LOAN_STATUS_OK);
  auto* writer = harness.WriterFor<aimrt_dds_test::PlainSample>(pub_info.topic_name);
  void* externally_discarded = message.msg_ptr;
  ASSERT_EQ(writer->discard_loan(externally_discarded), RETCODE_OK);
  ASSERT_EQ(externally_discarded, nullptr);
  EXPECT_EQ(aimrt::channel::ReleaseLoanedMessage(message),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(message.msg_ptr, nullptr);
  EXPECT_TRUE(logs.Contains(AIMRT_LOG_LEVEL_ERROR,
                            "discard_loan failed", "dds06/l03"));
}

TEST(DdsLoanPublisherRuntimeFailure, RejectsWrongRouteWithoutCallingWriter) {
  ChannelHarness harness(TestDomain(30));
  auto first_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/l03/wrong_route_first", "first_publisher");
  auto second_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/l03/wrong_route_second", "second_publisher");
  runtime::core::channel::PublishTypeWrapper first_publisher{.info = first_info};
  runtime::core::channel::PublishTypeWrapper second_publisher{.info = second_info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(first_publisher));
  ASSERT_TRUE(harness.Backend().RegisterPublishType(second_publisher));
  harness.Start();
  runtime::core::channel::BackendLoanedPublisher first_route;
  runtime::core::channel::BackendLoanedPublisher second_route;
  ASSERT_EQ(harness.Backend().PrepareLoanedPublisher(first_publisher, first_route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  ASSERT_EQ(harness.Backend().PrepareLoanedPublisher(second_publisher, second_route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt_channel_loaned_message_base_t message;
  ASSERT_EQ(first_route.borrow(first_route.impl, message),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  const auto* retained_pointer = message.msg_ptr;
  std::atomic_int writer_calls = 0;
  harness.Backend().SetBeforeLoanedWriteHookForTesting([&] { ++writer_calls; });
  aimrt::channel::Context context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  EXPECT_EQ(second_route.publish(second_route.impl, context, message),
            AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(message.msg_ptr, retained_pointer);
  EXPECT_EQ(writer_calls, 0);
  EXPECT_EQ(aimrt::channel::ReleaseLoanedMessage(message),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
}

TEST(DdsLoanPublisherRuntimeFailure,
     RealHistoryOutOfResourcesTerminallyReleasesRecognizedLoan) {
  const auto domain = TestDomain(30);
  TemporaryXml xml("aimrt_dds06_l03_history_oor", kLoanHistoryExhaustionXml);
  ChannelHarness harness(domain, 2, xml.Path());
  auto pub_info = MakeDdsTopicInfo<aimrt_dds_test::KeyedPlainSample>(
      "dds06/l03/history_oor", "plain_publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  harness.Start();
  runtime::core::channel::BackendLoanedPublisher route;
  ASSERT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  auto* writer =
      harness.WriterFor<aimrt_dds_test::KeyedPlainSample>(pub_info.topic_name);
  auto reader = OwnedProcessGroup::Launch(
      "--dds-ack-dropping-reader",
      {std::to_string(domain), pub_info.topic_name}, true);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(reader->ReadReady(12s));
  ASSERT_TRUE(WaitForWriterMatch(writer));

  aimrt::channel::Context context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  aimrt_dds_test::KeyedPlainSample disposed;
  disposed.key(1);
  const auto first_handle = writer->register_instance(&disposed);
  ASSERT_NE(first_handle, HANDLE_NIL);
  ASSERT_EQ(writer->dispose(&disposed, first_handle), RETCODE_OK);
  disposed.key(2);
  const auto second_handle = writer->register_instance(&disposed);
  ASSERT_NE(second_handle, HANDLE_NIL);
  ASSERT_EQ(writer->dispose(&disposed, second_handle), RETCODE_OK);

  aimrt_channel_loaned_message_base_t exhausted;
  ASSERT_EQ(route.borrow(route.impl, exhausted), AIMRT_CHANNEL_LOAN_STATUS_OK);
  void* exhausted_pointer = exhausted.msg_ptr;
  auto* exhausted_sample =
      static_cast<aimrt_dds_test::KeyedPlainSample*>(exhausted.msg_ptr);
  exhausted_sample->key(1);
  exhausted_sample->value(3);
  EXPECT_EQ(route.publish(route.impl, context, exhausted),
            AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
  EXPECT_EQ(exhausted.msg_ptr, nullptr);

  EXPECT_EQ(writer->discard_loan(exhausted_pointer), RETCODE_BAD_PARAMETER);
  aimrt_channel_loaned_message_base_t replacement;
  ASSERT_EQ(route.borrow(route.impl, replacement), AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(aimrt::channel::ReleaseLoanedMessage(replacement),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  ASSERT_TRUE(reader->Continue());
  const auto reader_result = reader->Finish(5s);
  ASSERT_TRUE(WIFEXITED(reader_result.status));
  EXPECT_EQ(WEXITSTATUS(reader_result.status), 0);
  EXPECT_TRUE(reader_result.child_reaped);
  EXPECT_TRUE(reader_result.no_residual_process_group);
  harness.Stop();
}

TEST(DdsLoanPublisherRuntimeFailure,
     RealHistoryTimeoutRestoresTheSameLoanForExplicitDiscard) {
  const auto domain = TestDomain(30);
  TemporaryXml xml("aimrt_dds06_l03_history_timeout", kLoanHistoryTimeoutXml);
  ChannelHarness harness(domain, 2, xml.Path());
  auto pub_info = MakeDdsTopicInfo<aimrt_dds_test::KeyedPlainSample>(
      "dds06/l03/history_timeout", "plain_publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  harness.Start();
  runtime::core::channel::BackendLoanedPublisher route;
  ASSERT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  auto* writer =
      harness.WriterFor<aimrt_dds_test::KeyedPlainSample>(pub_info.topic_name);
  auto reader = OwnedProcessGroup::Launch(
      "--dds-ack-dropping-reader",
      {std::to_string(domain), pub_info.topic_name}, true);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(reader->ReadReady(12s));
  ASSERT_TRUE(WaitForWriterMatch(writer));

  aimrt::channel::Context context(
      aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT);
  aimrt_channel_loaned_message_base_t first;
  ASSERT_EQ(route.borrow(route.impl, first), AIMRT_CHANNEL_LOAN_STATUS_OK);
  auto* first_sample =
      static_cast<aimrt_dds_test::KeyedPlainSample*>(first.msg_ptr);
  first_sample->key(1);
  first_sample->value(1);
  ASSERT_EQ(route.publish(route.impl, context, first),
            AIMRT_CHANNEL_LOAN_STATUS_OK);

  aimrt_channel_loaned_message_base_t timed_out;
  ASSERT_EQ(route.borrow(route.impl, timed_out), AIMRT_CHANNEL_LOAN_STATUS_OK);
  void* retained_pointer = timed_out.msg_ptr;
  auto* timed_out_sample =
      static_cast<aimrt_dds_test::KeyedPlainSample*>(timed_out.msg_ptr);
  timed_out_sample->key(1);
  timed_out_sample->value(2);
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(route.publish(route.impl, context, timed_out),
            AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
  EXPECT_GE(std::chrono::steady_clock::now() - started, 50ms);
  EXPECT_EQ(timed_out.msg_ptr, retained_pointer);

  void* directly_discarded = retained_pointer;
  ASSERT_EQ(writer->discard_loan(directly_discarded), RETCODE_OK);
  ASSERT_EQ(directly_discarded, nullptr);
  EXPECT_EQ(aimrt::channel::ReleaseLoanedMessage(timed_out),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(timed_out.msg_ptr, nullptr);
  ASSERT_TRUE(reader->Continue());
  const auto reader_result = reader->Finish(5s);
  ASSERT_TRUE(WIFEXITED(reader_result.status));
  EXPECT_EQ(WEXITSTATUS(reader_result.status), 0);
  EXPECT_TRUE(reader_result.child_reaped);
  EXPECT_TRUE(reader_result.no_residual_process_group);
  harness.Stop();
}

TEST(DdsLoanSubscriber, DeliversCallbackScopedLoanOnPluginExecutor) {
  ChannelHarness harness(TestDomain(120));
  auto pub_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/l04", "publisher");
  auto sub_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/l04", "loan_subscriber");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  std::atomic_int received = 0;
  std::atomic_bool worker_thread = false;
  runtime::core::channel::LoanedSubscribeWrapper subscriber{
      .info = sub_info,
      .callback = [&](aimrt::channel::ContextRef context, const void* sample) {
        worker_thread = harness.Runtime().Executor()->IsWorkerThread();
        EXPECT_EQ(context.GetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_BACKEND), "dds");
        EXPECT_EQ(static_cast<const aimrt_dds_test::PlainSample*>(sample)->value(), 41);
        ++received;
      }};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  ASSERT_EQ(harness.Backend().SubscribeLoaned(subscriber),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  harness.Start();
  ASSERT_TRUE(WaitForWriterMatch(
      harness.WriterFor<aimrt_dds_test::PlainSample>(pub_info.topic_name)));
  harness.Backend().SetReaderTakeResultForTesting(RETCODE_OUT_OF_RESOURCES);
  aimrt_dds_test::PlainSample message;
  message.value(41);
  PublishDds(harness.Backend(), pub_info, message);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend().ReaderRetryScheduleCountForTesting(
               sub_info.topic_name, sub_info.msg_type) >= 8 &&
           harness.Backend().ReaderLastRetryDelayMsForTesting(
               sub_info.topic_name, sub_info.msg_type) == 100;
  }));
  EXPECT_EQ(received, 0);
  EXPECT_EQ(harness.Backend().LoanReturnCountForTesting(
                sub_info.topic_name, sub_info.msg_type),
            0U);
  EXPECT_GE(harness.Backend().ReaderUnreadCountForTesting(
                sub_info.topic_name, sub_info.msg_type),
            1);

  std::promise<void> fair_task;
  ASSERT_TRUE(harness.Runtime().Executor()->Post([&] { fair_task.set_value(); }));
  EXPECT_EQ(fair_task.get_future().wait_for(250ms), std::future_status::ready);
  const auto retry_count =
      harness.Backend().ReaderRetryScheduleCountForTesting(
          sub_info.topic_name, sub_info.msg_type);
  std::this_thread::sleep_for(250ms);
  EXPECT_LE(harness.Backend().ReaderRetryScheduleCountForTesting(
                sub_info.topic_name, sub_info.msg_type) -
                retry_count,
            3U);
  EXPECT_EQ(received, 0);

  harness.Backend().SetReaderTakeResultForTesting(RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] { return received == 1; }));
  EXPECT_TRUE(worker_thread);
  EXPECT_EQ(harness.Backend().LoanReturnCountForTesting(
                sub_info.topic_name, sub_info.msg_type),
            1U);
  EXPECT_GE(harness.Runtime().Diagnostics()->Snapshot().reader_take_failure_total,
            8U);
  ASSERT_TRUE(WaitFor([&] {
    return harness.Backend().ReaderRetryStepForTesting(
               sub_info.topic_name, sub_info.msg_type) == 0;
  }));
}

TEST(DdsLoanMixedSubscription, SharesReaderAndCopiesBeforeReturningLoan) {
  ChannelHarness harness(TestDomain(121));
  auto pub_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/l05", "publisher");
  auto loan_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/l05", "loan_subscriber");
  auto ordinary_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/l05", "ordinary_subscriber");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  std::atomic_int loaned_value = 0;
  std::atomic_int ordinary_value = 0;
  std::atomic<const void*> loaned_pointer = nullptr;
  std::atomic<const void*> ordinary_pointer = nullptr;
  runtime::core::channel::LoanedSubscribeWrapper loaned_subscriber{
      .info = loan_info,
      .callback = [&](aimrt::channel::ContextRef, const void* sample) {
        loaned_pointer = sample;
        loaned_value =
            static_cast<const aimrt_dds_test::PlainSample*>(sample)->value();
      }};
  runtime::core::channel::SubscribeWrapper ordinary_subscriber{
      .info = ordinary_info,
      .callback = [&](runtime::core::channel::MsgWrapper& message,
                      std::function<void()>&&) {
        ordinary_pointer = message.msg_ptr;
        ordinary_value =
            static_cast<const aimrt_dds_test::PlainSample*>(message.msg_ptr)->value();
      }};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  ASSERT_EQ(harness.Backend().SubscribeLoaned(loaned_subscriber),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  ASSERT_TRUE(harness.Backend().Subscribe(ordinary_subscriber));
  EXPECT_EQ(harness.Runtime().Endpoints().ReaderCount(), 1U);
  harness.Start();
  ASSERT_TRUE(WaitForWriterMatch(
      harness.WriterFor<aimrt_dds_test::PlainSample>(pub_info.topic_name)));
  aimrt_dds_test::PlainSample message;
  message.value(52);
  PublishDds(harness.Backend(), pub_info, message);
  ASSERT_TRUE(WaitFor([&] {
    return loaned_value.load() == 52 && ordinary_value.load() == 52;
  }));
  EXPECT_NE(loaned_pointer.load(), ordinary_pointer.load());
  EXPECT_EQ(harness.Backend().LoanReturnCountForTesting(
                loan_info.topic_name, loan_info.msg_type),
            1U);
}

TEST(DdsLoanShutdown, ReturnsNormalLoanAndIsolatesCoreLeakProtection) {
  ChannelHarness harness(TestDomain(122));
  auto pub_info = MakeDdsTopicInfo<aimrt_dds_test::PlainSample>(
      "dds06/l07", "publisher");
  runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
  ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
  harness.Start();
  runtime::core::channel::BackendLoanedPublisher route;
  ASSERT_EQ(harness.Backend().PrepareLoanedPublisher(publisher, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt_channel_loaned_message_base_t message;
  ASSERT_EQ(route.borrow(route.impl, message), AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt::channel::ReleaseLoanedMessage(message);
  harness.Stop();

  const char* core_test = std::getenv("AIMRT_RUNTIME_CORE_TEST_PATH");
  ASSERT_NE(core_test, nullptr);
  auto child = OwnedProcessGroup::Launch("--dds-core-loan-leak-probe", {core_test});
  ASSERT_NE(child, nullptr);
  const auto result = child->Finish(20s);
  EXPECT_TRUE(result.process_group_established);
  ASSERT_TRUE(WIFEXITED(result.status));
  EXPECT_EQ(WEXITSTATUS(result.status), 0);
  EXPECT_TRUE(result.child_reaped);
  EXPECT_TRUE(result.no_residual_process_group);
}

TEST(DdsLoanShutdown, WriterOperationLeasesDrainBeforeDeletion) {
  VerifyLoanTransitionDeletionOverlap(23, LoanTransition::kBorrow);
  VerifyLoanTransitionDeletionOverlap(24, LoanTransition::kPublish);
  VerifyLoanTransitionDeletionOverlap(25, LoanTransition::kDiscard);
}

TEST(DdsListenerEntityDeletionLoan,
     WriterDeletionWaitsForEveryLoanTransition) {
  VerifyLoanTransitionDeletionOverlap(123, LoanTransition::kBorrow);
  VerifyLoanTransitionDeletionOverlap(124, LoanTransition::kPublish);
  VerifyLoanTransitionDeletionOverlap(125, LoanTransition::kDiscard);
}

TEST(DdsListenerLossRejection, RecordsRealStatusesAndOrdersSameReaderSurvivors) {
  ScopedLogCapture logs;
  DdsDiagnosticsSnapshot loss_snapshot;
  std::vector<int32_t> loss_survivors;
  {
    const auto domain = TestDomain(26);
    ChannelHarness harness(domain);
    auto sub_info = MakeTopicInfo("dds05/d03/loss", "loss_subscriber");
    std::mutex delivered_mutex;
    runtime::core::channel::SubscribeWrapper subscriber{
        .info = sub_info,
        .callback = [&](runtime::core::channel::MsgWrapper& message, std::function<void()>&&) {
          std::lock_guard lock(delivered_mutex);
          loss_survivors.emplace_back(
              static_cast<const example::AddRequest*>(message.msg_ptr)->lhs());
        }};
    ASSERT_TRUE(harness.Backend().Subscribe(subscriber));
    harness.Start();
    auto child = OwnedProcessGroup::Launch(
        "--dds-loss-writer", {std::to_string(domain), sub_info.topic_name});
    ASSERT_NE(child, nullptr);
    const auto child_result = child->Finish();
    EXPECT_TRUE(child_result.process_group_established);
    ASSERT_TRUE(WIFEXITED(child_result.status));
    EXPECT_EQ(WEXITSTATUS(child_result.status), 0);
    EXPECT_TRUE(child_result.no_residual_process_group);
    EXPECT_TRUE(WaitFor([&] {
      std::lock_guard lock(delivered_mutex);
      return harness.Runtime().Diagnostics()->Snapshot().sample_lost_total >= 3 &&
             !loss_survivors.empty() && loss_survivors.back() == 9;
    }));
    loss_snapshot = harness.Runtime().Diagnostics()->Snapshot();
    std::lock_guard lock(delivered_mutex);
    EXPECT_TRUE(std::ranges::is_sorted(loss_survivors));
    EXPECT_EQ(loss_survivors, (std::vector<int32_t>{0, 3, 4, 6, 7, 8, 9}));
  }

  TemporaryXml xml("aimrt_dds05_d03", kConstrainedReliableXml);
  DdsDiagnosticsSnapshot rejected_snapshot;
  std::vector<int32_t> rejected_survivors;
  {
    ChannelHarness harness(TestDomain(126), 2, xml.Path());
    auto pub_info = MakeTopicInfo("dds05/d03/rejected", "rejection_publisher");
    auto sub_info = MakeTopicInfo("dds05/d03/rejected", "rejection_subscriber");
    runtime::core::channel::PublishTypeWrapper publisher{.info = pub_info};
    std::mutex delivered_mutex;
    std::promise<void> callback_entered_promise;
    auto callback_entered = callback_entered_promise.get_future();
    std::promise<void> release_callback_promise;
    auto release_callback = release_callback_promise.get_future().share();
    runtime::core::channel::SubscribeWrapper subscriber{
        .info = sub_info,
        .callback = [&](runtime::core::channel::MsgWrapper& message, std::function<void()>&&) {
          const auto sequence =
              static_cast<const example::AddRequest*>(message.msg_ptr)->lhs();
          {
            std::lock_guard lock(delivered_mutex);
            rejected_survivors.emplace_back(sequence);
          }
          if (sequence == 0) {
            callback_entered_promise.set_value();
            release_callback.wait();
          }
        }};
    ASSERT_TRUE(harness.Backend().RegisterPublishType(publisher));
    ASSERT_TRUE(harness.Backend().Subscribe(subscriber));
    harness.Start();
    ASSERT_TRUE(WaitForWriterMatch(harness.Writer(pub_info.topic_name)));
    Publish(harness.Backend(), pub_info, 0);
    const bool callback_blocked =
        callback_entered.wait_for(5s) == std::future_status::ready;
    if (callback_blocked) {
      for (int32_t sequence = 1; sequence < 21; ++sequence) {
        Publish(harness.Backend(), pub_info, sequence);
      }
    }
    const bool real_rejection = WaitFor([&] {
      return harness.Runtime().Diagnostics()->Snapshot().sample_rejected_total > 0;
    });
    release_callback_promise.set_value();
    const bool surviving_dispatch = WaitFor([&] {
      std::lock_guard lock(delivered_mutex);
      return rejected_survivors.size() +
                 harness.Runtime().Diagnostics()->Snapshot().sample_rejected_total ==
             21;
    });
    rejected_snapshot = harness.Runtime().Diagnostics()->Snapshot();

    EXPECT_TRUE(callback_blocked);
    EXPECT_TRUE(real_rejection);
    EXPECT_TRUE(surviving_dispatch);
    std::lock_guard lock(delivered_mutex);
    EXPECT_TRUE(std::ranges::is_sorted(rejected_survivors));
    EXPECT_EQ(rejected_survivors.size() + rejected_snapshot.sample_rejected_total, 21U);
  }

  EXPECT_EQ(loss_snapshot.sample_lost_total, 3U);
  EXPECT_GT(loss_snapshot.sample_lost_last_change, 0);
  EXPECT_GT(rejected_snapshot.sample_rejected_total, 0U);
  EXPECT_GT(rejected_snapshot.sample_rejected_last_change, 0);
  EXPECT_EQ(rejected_snapshot.last_sample_rejected_reason, REJECTED_BY_SAMPLES_LIMIT);
  EXPECT_TRUE(logs.Contains(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_sample_lost",
                            "reason='sequence_gap'"));
  EXPECT_TRUE(logs.Contains(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_sample_rejected",
                            "reason='samples_limit'"));
  EXPECT_LE(logs.Count(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_sample_lost"), 4U);
  EXPECT_LE(logs.Count(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_sample_rejected"), 6U);
}

TEST(DdsListenerDeadlineLiveliness, RecordsReaderAndWriterReasonsAndChanges) {
  ScopedLogCapture logs;
  ChannelHarness harness(TestDomain(27));
  auto type = aimrt::GetStaticDdsTypeSupportHandle<example::AddRequest>();
  auto writer_qos = harness.Context().Qos().channel_writer;
  auto reader_qos = harness.Context().Qos().channel_reader;
  writer_qos.deadline().period.seconds = 0;
  writer_qos.deadline().period.nanosec = 100000000;
  reader_qos.deadline().period = writer_qos.deadline().period;
  writer_qos.liveliness().kind = MANUAL_BY_TOPIC_LIVELINESS_QOS;
  reader_qos.liveliness().kind = MANUAL_BY_TOPIC_LIVELINESS_QOS;
  writer_qos.liveliness().lease_duration.seconds = 0;
  writer_qos.liveliness().lease_duration.nanosec = 100000000;
  reader_qos.liveliness().lease_duration = writer_qos.liveliness().lease_duration;
  auto* writer = harness.Runtime().Endpoints().GetOrCreateWriter(
      "dds05/d04", type, writer_qos, "D04 finite deadline/liveliness writer");
  auto reader = harness.Runtime().Endpoints().GetOrCreateReader(
      "dds05/d04", type, reader_qos, "D04 finite deadline/liveliness reader");
  auto diagnostics = harness.Runtime().Diagnostics();
  harness.StartRuntime();
  ASSERT_TRUE(WaitForWriterMatch(writer));
  example::AddRequest initial_sample;
  initial_sample.lhs(1);
  ASSERT_EQ(writer->write(&initial_sample), RETCODE_OK);
  ASSERT_TRUE(WaitFor([&] {
    const auto snapshot = diagnostics->Snapshot();
    return snapshot.requested_deadline_missed_total > 0 &&
           snapshot.offered_deadline_missed_total > 0 &&
           snapshot.reader_liveliness_change_events > 0 &&
           snapshot.writer_liveliness_lost_total > 0;
  }));
  const auto snapshot = diagnostics->Snapshot();
  EXPECT_GT(snapshot.requested_deadline_missed_total, 0U);
  EXPECT_GT(snapshot.offered_deadline_missed_total, 0U);
  EXPECT_GT(snapshot.reader_liveliness_change_events, 0U);
  EXPECT_GT(snapshot.writer_liveliness_lost_total, 0U);
  EXPECT_GT(snapshot.requested_deadline_last_change, 0);
  EXPECT_GT(snapshot.offered_deadline_last_change, 0);
  EXPECT_NE(snapshot.reader_alive_last_change, 0);
  EXPECT_GT(snapshot.writer_liveliness_lost_last_change, 0);
  EXPECT_TRUE(logs.Contains(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_deadline endpoint=reader",
                            "reason='requested_deadline_missed'"));
  EXPECT_TRUE(logs.Contains(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_deadline endpoint=writer",
                            "reason='offered_deadline_missed'"));
  EXPECT_TRUE(logs.Contains(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_liveliness endpoint=reader",
                            "reason='writer_liveliness_changed'"));
  EXPECT_TRUE(logs.Contains(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_liveliness endpoint=writer",
                            "reason='lease_expired'"));
  EXPECT_LE(logs.Count(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_deadline endpoint=reader"), 8U);
  EXPECT_LE(logs.Count(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_deadline endpoint=writer"), 8U);
  EXPECT_LE(logs.Count(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_liveliness endpoint=reader"),
            8U);
  EXPECT_LE(logs.Count(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_liveliness endpoint=writer"),
            8U);
}

TEST(DdsListenerDeadlineLiveliness, RateLimitsBatchedDiagnosticStormsByCallbackEvent) {
  ScopedLogCapture logs;
  ChannelHarness harness(TestDomain(127));
  auto type = aimrt::GetStaticDdsTypeSupportHandle<example::AddRequest>();
  auto reader = harness.Runtime().Endpoints().GetOrCreateReader(
      "dds05/d04/rate", type, harness.Context().Qos().channel_reader,
      "D04 rate limiter reader");
  auto* writer = harness.Runtime().Endpoints().GetOrCreateWriter(
      "dds05/d04/rate", type, harness.Context().Qos().channel_writer,
      "D04 rate limiter writer");
  auto diagnostics = harness.Runtime().Diagnostics();
  DdsDataReaderListener reader_listener(
      DdsEndpointMetadata{.participant = "d04-rate",
                          .topic = "dds05/d04/rate",
                          .type = type->get_name()},
      reader.drain_state, diagnostics);
  DdsDataWriterListener writer_listener(
      DdsEndpointMetadata{.participant = "d04-rate",
                          .topic = "dds05/d04/rate",
                          .type = type->get_name()},
      diagnostics);

  for (uint32_t event = 1; event <= 16; ++event) {
    RequestedDeadlineMissedStatus requested;
    requested.total_count = event * 17;
    requested.total_count_change = 17;
    reader_listener.on_requested_deadline_missed(reader.reader, requested);
    OfferedDeadlineMissedStatus offered;
    offered.total_count = event * 17;
    offered.total_count_change = 17;
    writer_listener.on_offered_deadline_missed(writer, offered);
    LivelinessChangedStatus reader_liveliness;
    reader_liveliness.alive_count = 1;
    reader_liveliness.alive_count_change = 1;
    reader_listener.on_liveliness_changed(reader.reader, reader_liveliness);
    LivelinessLostStatus writer_liveliness;
    writer_liveliness.total_count = event * 17;
    writer_liveliness.total_count_change = 17;
    writer_listener.on_liveliness_lost(writer, writer_liveliness);
  }

  const auto snapshot = diagnostics->Snapshot();
  EXPECT_EQ(snapshot.requested_deadline_missed_total, 272U);
  EXPECT_EQ(snapshot.requested_deadline_last_change, 17);
  EXPECT_EQ(snapshot.offered_deadline_missed_total, 272U);
  EXPECT_EQ(snapshot.offered_deadline_last_change, 17);
  EXPECT_EQ(snapshot.reader_liveliness_change_events, 16U);
  EXPECT_EQ(snapshot.reader_alive_last_change, 1);
  EXPECT_EQ(snapshot.writer_liveliness_lost_total, 272U);
  EXPECT_EQ(snapshot.writer_liveliness_lost_last_change, 17);
  EXPECT_EQ(logs.Count(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_deadline endpoint=reader"), 5U);
  EXPECT_EQ(logs.Count(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_deadline endpoint=writer"), 5U);
  EXPECT_EQ(logs.Count(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_liveliness endpoint=reader"),
            5U);
  EXPECT_EQ(logs.Count(AIMRT_LOG_LEVEL_WARN, "aimrt_dds_listener_liveliness endpoint=writer"),
            5U);
}

}  // namespace

int RunDdsChannelExternalWriter(uint32_t domain, const std::string& topic,
                                int32_t sequence, int ready_fd = -1,
                                int continue_fd = -1) {
  return RunExternalWriter(domain, topic, sequence, ready_fd, continue_fd);
}

int RunDdsChannelLossWriter(uint32_t domain, const std::string& topic) {
  return RunLossWriter(domain, topic);
}

int RunDdsChannelAckDroppingReader(uint32_t domain, const std::string& topic,
                                   int ready_fd, int continue_fd) {
  return RunAckDroppingReader(domain, topic, ready_fd, continue_fd);
}

int RunDdsXtypesDiscoveryObserver(uint32_t domain, const std::string& topic,
                                  const std::filesystem::path& report_path,
                                  pid_t publisher_pid, int ready_fd,
                                  int continue_fd) {
  return RunXtypesDiscoveryObserver(domain, topic, report_path, publisher_pid,
                                    ready_fd, continue_fd);
}

int RunDdsCoreLoanLeakProbe(const std::filesystem::path& runtime_core_test) {
  return RunCoreLoanLeakProbe(runtime_core_test);
}

int RunDdsChannelStubbornChild(const std::filesystem::path& term_path,
                               const std::filesystem::path& ready_path) {
  return RunStubbornChild(term_path, ready_path);
}

int RunDdsChannelSignalCleanupLauncher(
    std::string_view window,
    const std::filesystem::path& report_path,
    const std::filesystem::path& launcher_ready_path,
    const std::filesystem::path& child_ready_path,
    const std::filesystem::path& term_path,
    const std::filesystem::path& signal_sent_path) {
  return RunSignalCleanupLauncherProbe(window, report_path, launcher_ready_path,
                                       child_ready_path, term_path, signal_sent_path);
}

bool StartDdsChannelLauncherSignalRelay() { return StartLauncherSignalRelay(); }
bool StopDdsChannelLauncherSignalRelay() { return StopLauncherSignalRelay(); }

}  // namespace aimrt::plugins::dds_plugin

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  if (argc == 5 && std::string_view(argv[1]) == "--dds-external-writer") {
    try {
      return aimrt::plugins::dds_plugin::RunDdsChannelExternalWriter(
          static_cast<uint32_t>(std::stoul(argv[2])), argv[3], std::stoi(argv[4]));
    } catch (...) {
      return 126;
    }
  }
  if (argc == 7 && std::string_view(argv[1]) == "--dds-boundary-writer") {
    try {
      return aimrt::plugins::dds_plugin::RunDdsChannelExternalWriter(
          static_cast<uint32_t>(std::stoul(argv[2])), argv[3], std::stoi(argv[4]),
          std::stoi(argv[5]), std::stoi(argv[6]));
    } catch (...) {
      return 125;
    }
  }
  if (argc == 4 && std::string_view(argv[1]) == "--dds-loss-writer") {
    try {
      return aimrt::plugins::dds_plugin::RunDdsChannelLossWriter(
          static_cast<uint32_t>(std::stoul(argv[2])), argv[3]);
    } catch (...) {
      return 123;
    }
  }
  if (argc == 6 && std::string_view(argv[1]) == "--dds-ack-dropping-reader") {
    try {
      return aimrt::plugins::dds_plugin::RunDdsChannelAckDroppingReader(
          static_cast<uint32_t>(std::stoul(argv[2])), argv[3],
          std::stoi(argv[4]), std::stoi(argv[5]));
    } catch (...) {
      return 122;
    }
  }
  if (argc == 8 && std::string_view(argv[1]) == "--dds-xtypes-observer") {
    try {
      return aimrt::plugins::dds_plugin::RunDdsXtypesDiscoveryObserver(
          static_cast<uint32_t>(std::stoul(argv[2])), argv[3], argv[4],
          static_cast<pid_t>(std::stol(argv[5])), std::stoi(argv[6]),
          std::stoi(argv[7]));
    } catch (...) {
      return 118;
    }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--dds-core-loan-leak-probe") {
    return aimrt::plugins::dds_plugin::RunDdsCoreLoanLeakProbe(argv[2]);
  }
  if (argc == 4 && std::string_view(argv[1]) == "--dds-stubborn-child") {
    return aimrt::plugins::dds_plugin::RunDdsChannelStubbornChild(argv[2], argv[3]);
  }
  if (argc == 8 && std::string_view(argv[1]) == "--dds-signal-cleanup-launcher") {
    return aimrt::plugins::dds_plugin::RunDdsChannelSignalCleanupLauncher(
        argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);
  }
  if (!aimrt::plugins::dds_plugin::StartDdsChannelLauncherSignalRelay()) return 121;
  testing::InitGoogleTest(&argc, argv);
  const int test_result = RUN_ALL_TESTS();
  if (!aimrt::plugins::dds_plugin::StopDdsChannelLauncherSignalRelay()) return 120;
  return test_result;
}
