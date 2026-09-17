// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "dds_plugin/dds_config.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include <tinyxml2.h>
#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>

namespace aimrt::plugins::dds_plugin {
namespace {

using namespace eprosima::fastdds::dds;

struct XmlDefaults {
  std::optional<std::string> participant;
  std::optional<std::string> writer;
  std::optional<std::string> reader;
  std::optional<std::string> topic;
  bool writer_max_blocking_time_configured = false;
};

struct ProcessParticipantRegistry {
  std::mutex mutex;
  const DdsParticipantContext* owner = nullptr;
  uint32_t domain_id = 0;
  std::string participant_name;
};

ProcessParticipantRegistry& GetProcessParticipantRegistry() {
  static ProcessParticipantRegistry registry;
  return registry;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error(
        "Fast DDS XML error: file='" + path.string() +
        "', entity='profiles', profile='<all>': file cannot be opened");
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string LocalXmlName(const char* name) {
  std::string value = name == nullptr ? std::string{} : std::string{name};
  const auto separator = value.rfind(':');
  return separator == std::string::npos ? value : value.substr(separator + 1);
}

bool IsTrueAttribute(const tinyxml2::XMLElement& element, const char* name) {
  const char* value = element.Attribute(name);
  if (value == nullptr) return false;
  std::string normalized(value);
  std::ranges::transform(normalized, normalized.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return normalized == "true" || normalized == "1";
}

bool HasDescendantNamed(const tinyxml2::XMLElement* element,
                        std::string_view expected_name) {
  for (auto* current = element; current != nullptr;
       current = current->NextSiblingElement()) {
    if (LocalXmlName(current->Name()) == expected_name ||
        HasDescendantNamed(current->FirstChildElement(), expected_name)) {
      return true;
    }
  }
  return false;
}

void CollectXmlDefaults(
    const tinyxml2::XMLElement* element,
    std::unordered_map<std::string, std::vector<std::string>>& defaults,
    bool& writer_max_blocking_time_configured) {
  for (auto* current = element; current != nullptr; current = current->NextSiblingElement()) {
    if (IsTrueAttribute(*current, "is_default_profile")) {
      auto kind = LocalXmlName(current->Name());
      if (kind == "publisher") kind = "data_writer";
      if (kind == "subscriber") kind = "data_reader";
      if (kind == "participant" || kind == "topic" || kind == "data_writer" || kind == "data_reader") {
        const char* profile_name = current->Attribute("profile_name");
        defaults[kind].emplace_back(profile_name == nullptr ? "<unnamed>" : profile_name);
        if (kind == "data_writer") {
          writer_max_blocking_time_configured = HasDescendantNamed(
              current->FirstChildElement(), "max_blocking_time");
        }
      }
    }
    CollectXmlDefaults(current->FirstChildElement(), defaults,
                       writer_max_blocking_time_configured);
  }
}

XmlDefaults InspectXml(const std::string& path, const std::string& xml) {
  tinyxml2::XMLDocument document;
  const auto parse_result = document.Parse(xml.data(), xml.size());
  if (parse_result != tinyxml2::XML_SUCCESS) {
    throw std::runtime_error(
        "Fast DDS XML error: file='" + path +
        "', entity='profiles', profile='<all>': parse failed: " + document.ErrorStr());
  }
  if (document.RootElement() == nullptr) {
    throw std::runtime_error(
        "Fast DDS XML error: file='" + path +
        "', entity='profiles', profile='<all>': empty document");
  }

  std::unordered_map<std::string, std::vector<std::string>> defaults;
  bool writer_max_blocking_time_configured = false;
  CollectXmlDefaults(document.RootElement(), defaults,
                     writer_max_blocking_time_configured);
  for (const auto& [kind, profiles] : defaults) {
    if (profiles.size() > 1) {
      std::ostringstream message;
      message << "Fast DDS XML default profile conflict in '" << path << "' for entity kind '" << kind << "': ";
      for (size_t index = 0; index < profiles.size(); ++index) {
        if (index != 0) message << ", ";
        message << profiles[index];
      }
      throw std::runtime_error(message.str());
    }
  }

  const auto profile = [&defaults](std::string_view kind) -> std::optional<std::string> {
    const auto iterator = defaults.find(std::string(kind));
    if (iterator == defaults.end()) return std::nullopt;
    return iterator->second.front();
  };
  return XmlDefaults{
      .participant = profile("participant"),
      .writer = profile("data_writer"),
      .reader = profile("data_reader"),
      .topic = profile("topic"),
      .writer_max_blocking_time_configured =
          writer_max_blocking_time_configured};
}

void RequireOk(ReturnCode_t result, std::string_view operation, std::string_view path,
               std::string_view entity_kind, std::string_view profile = "<none>") {
  if (result == RETCODE_OK) return;
  throw std::runtime_error(
      std::string(operation) + " failed for Fast DDS XML '" + std::string(path) +
      "', entity kind '" + std::string(entity_kind) + "', profile '" +
      std::string(profile) + "', return code " + std::to_string(result));
}

void SetFixedXcdr2(DataWriterQos& qos) {
  qos.representation().m_value = {XCDR2_DATA_REPRESENTATION};
}

void SetFixedXcdr2(DataReaderQos& qos) {
  qos.representation().m_value = {XCDR2_DATA_REPRESENTATION};
}

DataWriterQos MakeDefaultChannelWriterQos() {
  auto qos = DATAWRITER_QOS_DEFAULT;
  qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
  qos.durability().kind = VOLATILE_DURABILITY_QOS;
  qos.history().kind = KEEP_LAST_HISTORY_QOS;
  qos.history().depth = 20;
  SetFixedXcdr2(qos);
  return qos;
}

DataReaderQos MakeDefaultChannelReaderQos() {
  auto qos = DATAREADER_QOS_DEFAULT;
  qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
  qos.durability().kind = VOLATILE_DURABILITY_QOS;
  qos.history().kind = KEEP_LAST_HISTORY_QOS;
  qos.history().depth = 20;
  SetFixedXcdr2(qos);
  return qos;
}

DataWriterQos MakeDefaultRpcWriterQos() {
  auto qos = DATAWRITER_QOS_DEFAULT;
  qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
  qos.durability().kind = VOLATILE_DURABILITY_QOS;
  qos.history().kind = KEEP_ALL_HISTORY_QOS;
  qos.data_sharing().off();
  SetFixedXcdr2(qos);
  return qos;
}

DataReaderQos MakeDefaultRpcReaderQos() {
  auto qos = DATAREADER_QOS_DEFAULT;
  qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
  qos.durability().kind = VOLATILE_DURABILITY_QOS;
  qos.history().kind = KEEP_ALL_HISTORY_QOS;
  qos.data_sharing().off();
  SetFixedXcdr2(qos);
  return qos;
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

std::string DurationName(const Duration_t& duration) {
  return duration.is_infinite() ? "infinite" : std::to_string(duration.to_ns()) + "ns";
}

std::string LivelinessName(LivelinessQosPolicyKind kind) {
  switch (kind) {
    case AUTOMATIC_LIVELINESS_QOS:
      return "automatic";
    case MANUAL_BY_PARTICIPANT_LIVELINESS_QOS:
      return "manual_by_participant";
    case MANUAL_BY_TOPIC_LIVELINESS_QOS:
      return "manual_by_topic";
  }
  return "unknown";
}

std::string HistoryName(HistoryQosPolicyKind kind, int32_t depth) {
  if (kind == KEEP_ALL_HISTORY_QOS) return "keep_all";
  return "keep_last(depth=" + std::to_string(depth) + ")";
}

bool HasInvalidNameCharacter(std::string_view value) {
  return std::ranges::any_of(value, [](unsigned char character) {
    return std::iscntrl(character) || std::isspace(character) || character == '\\';
  });
}

void ValidatePathLikeName(std::string_view value, std::string_view label) {
  if (value.empty()) throw std::invalid_argument(std::string(label) + " is empty");
  if (HasInvalidNameCharacter(value)) {
    throw std::invalid_argument(std::string(label) + " contains whitespace, a control character, or a backslash: " +
                                std::string(value));
  }
  if (value.find("//") != std::string_view::npos) {
    throw std::invalid_argument(std::string(label) + " contains an empty path segment: " + std::string(value));
  }
  size_t begin = 0;
  while (begin <= value.size()) {
    const auto end = value.find('/', begin);
    const auto segment = value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
    if (segment == "." || segment == "..") {
      throw std::invalid_argument(std::string(label) + " contains a reserved path segment: " + std::string(value));
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
}

void ValidateDdsServiceFqn(std::string_view service) {
  if (service.starts_with("::") || service.ends_with("::") || service.find(":::") != std::string_view::npos) {
    throw std::invalid_argument("DDS service must be a canonical IDL interface FQN without a leading '::': " +
                                std::string(service));
  }
  size_t begin = 0;
  while (begin < service.size()) {
    const auto end = service.find("::", begin);
    const auto component = service.substr(begin, end == std::string_view::npos ? service.size() - begin : end - begin);
    if (component.empty() || !(std::isalpha(static_cast<unsigned char>(component.front())) || component.front() == '_') ||
        !std::ranges::all_of(component, [](unsigned char character) {
          return std::isalnum(character) || character == '_';
        })) {
      throw std::invalid_argument("DDS service is not a canonical IDL interface FQN: " + std::string(service));
    }
    if (end == std::string_view::npos) break;
    begin = end + 2;
  }
}

void ValidateIdlIdentifier(std::string_view value, std::string_view label) {
  if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_') ||
      !std::ranges::all_of(value, [](unsigned char character) {
        return std::isalnum(character) || character == '_';
      })) {
    throw std::invalid_argument(std::string(label) + " is not a canonical IDL identifier: " + std::string(value));
  }
}

std::string EffectiveParticipantName(const DomainParticipantExtendedQos& qos) {
  return std::string(qos.name().c_str());
}

template <typename EndpointQos>
void ValidateEndpointQosConsistency(const EndpointQos& qos, std::string_view endpoint,
                                    std::string_view source, std::string_view profile,
                                    std::string_view xml_file) {
  const auto& history = qos.history();
  const auto& limits = qos.resource_limits();
  std::string reason;
  if (history.kind == KEEP_LAST_HISTORY_QOS && history.depth <= 0) {
    reason = "KEEP_LAST depth must be greater than zero";
  } else if (history.kind == KEEP_LAST_HISTORY_QOS && limits.max_samples_per_instance > 0 &&
             history.depth > limits.max_samples_per_instance) {
    reason = "KEEP_LAST depth exceeds max_samples_per_instance";
  } else if (limits.max_samples > 0 && limits.max_samples_per_instance > limits.max_samples) {
    reason = "max_samples_per_instance exceeds max_samples";
  } else if (limits.max_samples > 0 && limits.max_instances > 0 &&
             limits.max_samples_per_instance > 0 &&
             static_cast<int64_t>(limits.max_instances) * limits.max_samples_per_instance > limits.max_samples) {
    reason = "max_instances * max_samples_per_instance exceeds max_samples";
  } else if (limits.allocated_samples < 0) {
    reason = "allocated_samples must not be negative";
  } else if (limits.extra_samples < 0) {
    reason = "extra_samples must not be negative";
  }
  if (reason.empty()) return;

  throw std::invalid_argument(
      "Fast DDS effective QoS conflict: file='" + std::string(xml_file) +
      "', entity='" + std::string(endpoint) + "', profile='" +
      (profile.empty() ? std::string("<plugin-default>") : std::string(profile)) +
      "', source='" + std::string(source) + "', history='" +
      HistoryName(history.kind, history.depth) + "', max_samples=" +
      std::to_string(limits.max_samples) + ", max_instances=" +
      std::to_string(limits.max_instances) + ", max_samples_per_instance=" +
      std::to_string(limits.max_samples_per_instance) + ": " + reason);
}

void ValidateAllEndpointQos(const DdsQosSnapshot& qos, std::string_view xml_file) {
  ValidateEndpointQosConsistency(qos.channel_writer, "channel_writer", qos.writer_source,
                                 qos.writer_profile, xml_file);
  ValidateEndpointQosConsistency(qos.channel_reader, "channel_reader", qos.reader_source,
                                 qos.reader_profile, xml_file);
  ValidateEndpointQosConsistency(qos.rpc_writer, "rpc_writer", qos.writer_source,
                                 qos.writer_profile, xml_file);
  ValidateEndpointQosConsistency(qos.rpc_reader, "rpc_reader", qos.reader_source,
                                 qos.reader_profile, xml_file);
}

}  // namespace

DdsPluginOptions DecodeDdsPluginOptions(const YAML::Node& node) {
  DdsPluginOptions options;
  if (!node || node.IsNull()) return options;
  if (!node.IsMap()) throw std::invalid_argument("DDS plugin options must be a map");

  static const std::set<std::string> kKnownKeys{"domain_id", "participant_name", "fastdds_xml", "executor"};
  for (const auto& entry : node) {
    const auto key = entry.first.as<std::string>();
    if (!kKnownKeys.contains(key)) throw std::invalid_argument("Unsupported DDS plugin option: " + key);
  }
  if (node["domain_id"]) options.domain_id = node["domain_id"].as<uint32_t>();
  if (node["participant_name"]) options.participant_name = node["participant_name"].as<std::string>();
  if (node["fastdds_xml"]) options.fastdds_xml = node["fastdds_xml"].as<std::string>();
  if (const auto executor = node["executor"]) {
    if (!executor.IsMap()) throw std::invalid_argument("DDS executor options must be a map");
    static const std::set<std::string> kKnownExecutorKeys{"type", "thread_num"};
    for (const auto& entry : executor) {
      const auto key = entry.first.as<std::string>();
      if (!kKnownExecutorKeys.contains(key)) throw std::invalid_argument("Unsupported DDS executor option: " + key);
    }
    if (executor["type"]) options.executor.type = executor["type"].as<std::string>();
    if (executor["thread_num"]) options.executor.thread_num = executor["thread_num"].as<uint32_t>();
  }
  if (options.executor.type != "asio_thread") {
    throw std::invalid_argument("DDS executor type must be 'asio_thread', got: " + options.executor.type);
  }
  return options;
}

YAML::Node EncodeDdsPluginOptions(const DdsPluginOptions& options) {
  YAML::Node node;
  node["domain_id"] = options.domain_id;
  node["participant_name"] = options.participant_name;
  node["fastdds_xml"] = options.fastdds_xml;
  node["executor"]["type"] = options.executor.type;
  node["executor"]["thread_num"] = options.executor.thread_num;
  return node;
}

uint32_t ResolveDdsExecutorThreadNum(uint32_t configured, uint32_t hardware_concurrency) {
  if (configured != 0) return configured;
  if (hardware_concurrency == 0) return 2;
  return std::clamp(hardware_concurrency / 2, 2U, 4U);
}

std::string GetDdsDefaultParticipantName() {
  try {
    const auto executable = std::filesystem::read_symlink("/proc/self/exe");
    const auto basename = executable.filename().string();
    if (!basename.empty()) return basename;
  } catch (...) {
  }
  return "aimrt_dds";
}

void ValidateDdsChannelTopic(std::string_view topic) {
  try {
    ValidatePathLikeName(topic, "DDS Channel topic");
    if (topic.starts_with("req/") || topic.starts_with("rsp/")) {
      throw std::invalid_argument("uses reserved RPC prefix");
    }
  } catch (const std::invalid_argument& error) {
    throw std::invalid_argument(
        "DDS Channel name validation failed: original_name='" + std::string(topic) +
        "', derived_topic='" + std::string(topic) + "', reason='" + error.what() + "'");
  }
}

DdsRpcTopicNames DeriveDdsRpcTopicNames(std::string_view function_name) {
  const auto separator = function_name.find(":/");
  DdsRpcTopicNames candidate{.request = "<unavailable>", .response = "<unavailable>"};
  if (separator != std::string_view::npos) {
    const std::string suffix = std::string(function_name.substr(0, separator)) + "/" +
                               std::string(function_name.substr(separator + 2));
    candidate = {.request = "req/" + suffix, .response = "rsp/" + suffix};
  }
  try {
    ValidatePathLikeName(function_name, "AimRT RPC function");
    if (separator == std::string_view::npos ||
        function_name.find(":/", separator + 2) != std::string_view::npos) {
      throw std::invalid_argument("must match <rpc_type>:/<service>/<method>");
    }
    const auto rpc_type = function_name.substr(0, separator);
    const auto path = function_name.substr(separator + 2);
    const auto method_separator = path.rfind('/');
    if (method_separator == std::string_view::npos || path.find('/') != method_separator) {
      throw std::invalid_argument("must contain exactly service and method path segments");
    }
    const auto service = path.substr(0, method_separator);
    const auto method = path.substr(method_separator + 1);
    if (rpc_type != "dds" && rpc_type != "pb" && rpc_type != "ros2") {
      throw std::invalid_argument("unsupported RPC function type '" + std::string(rpc_type) + "'");
    }
    ValidatePathLikeName(service, "AimRT RPC service");
    ValidatePathLikeName(method, "AimRT RPC method");
    if (rpc_type == "dds") {
      ValidateDdsServiceFqn(service);
      ValidateIdlIdentifier(method, "DDS RPC method");
    }
    return candidate;
  } catch (const std::invalid_argument& error) {
    throw std::invalid_argument(
        "DDS RPC name validation failed: original_name='" + std::string(function_name) +
        "', derived_request='" + candidate.request + "', derived_response='" +
        candidate.response + "', reason='" + error.what() + "'");
  }
}

void DdsNameRegistry::InsertPhysicalName(std::string_view physical_name, std::string_view owner,
                                         std::string_view original_name, std::string_view derived_topics) {
  const auto [iterator, inserted] = physical_names_.try_emplace(
      std::string(physical_name),
      PhysicalOwner{std::string(owner), std::string(original_name), std::string(derived_topics)});
  if (inserted) return;
  const auto& conflict = iterator->second;
  if (conflict.owner == owner && conflict.original_name == original_name) return;
  throw std::invalid_argument(
      "DDS physical topic collision: physical_topic='" + std::string(physical_name) +
      "', incoming_object='" + std::string(owner) + "', incoming_original_name='" +
      std::string(original_name) + "', incoming_derived_topics='" + std::string(derived_topics) +
      "', conflicting_object='" + conflict.owner + "', conflicting_original_name='" +
      conflict.original_name + "', conflicting_derived_topics='" + conflict.derived_topics + "'");
}

void DdsNameRegistry::RegisterChannel(std::string_view topic, std::string_view object) {
  ValidateDdsChannelTopic(topic);
  InsertPhysicalName(topic, object, topic, topic);
}

DdsRpcTopicNames DdsNameRegistry::RegisterRpc(std::string_view function_name) {
  auto names = DeriveDdsRpcTopicNames(function_name);
  const auto derived = "request='" + names.request + "', response='" + names.response + "'";
  const auto canonical_owner = "RPC function '" + std::string(function_name) + "'";
  InsertPhysicalName(names.request, canonical_owner, function_name, derived);
  InsertPhysicalName(names.response, canonical_owner, function_name, derived);
  return names;
}

void ValidateDdsRpcQos(const DdsQosSnapshot& qos, std::string_view function_name) {
  if (qos.rpc_writer.reliability().kind != RELIABLE_RELIABILITY_QOS) {
    throw std::invalid_argument("DDS RPC writer reliability must be reliable for function '" +
                                std::string(function_name) + "', source=" + qos.writer_source +
                                ", effective=" + ReliabilityName(qos.rpc_writer.reliability().kind));
  }
  if (qos.rpc_reader.reliability().kind != RELIABLE_RELIABILITY_QOS) {
    throw std::invalid_argument("DDS RPC reader reliability must be reliable for function '" +
                                std::string(function_name) + "', source=" + qos.reader_source +
                                ", effective=" + ReliabilityName(qos.rpc_reader.reliability().kind));
  }
  if (qos.rpc_request_writer_max_blocking_time_configured &&
      qos.rpc_request_writer_max_blocking_time_ns != 0) {
    throw std::invalid_argument(
        "DDS RPC request writer max_blocking_time must be 0 us for function '" +
        std::string(function_name) + "', endpoint_role=request_writer, profile='" +
        (qos.writer_profile.empty() ? std::string("<plugin-default>")
                                    : qos.writer_profile) +
        "', configured=" +
        std::to_string(qos.rpc_request_writer_max_blocking_time_ns) +
        "ns, required=0us");
  }
}

DdsParticipantContext::~DdsParticipantContext() {
  try {
    Shutdown();
  } catch (...) {
  }
}

void DdsParticipantContext::Initialize(const DdsPluginOptions& options) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kStopped || participant_ != nullptr) {
    throw std::logic_error("DDS participant context can only be initialized once before shutdown");
  }
  if (options.executor.type != "asio_thread") {
    throw std::invalid_argument("DDS executor type must be 'asio_thread', got: " + options.executor.type);
  }

  options_ = options;
  effective_executor_thread_num_ = ResolveDdsExecutorThreadNum(options.executor.thread_num,
                                                               std::thread::hardware_concurrency());
  qos_ = DdsQosSnapshot{};
  qos_.publisher = PUBLISHER_QOS_DEFAULT;
  qos_.subscriber = SUBSCRIBER_QOS_DEFAULT;
  qos_.topic = TOPIC_QOS_DEFAULT;
  qos_.channel_writer = MakeDefaultChannelWriterQos();
  qos_.channel_reader = MakeDefaultChannelReaderQos();
  qos_.rpc_writer = MakeDefaultRpcWriterQos();
  qos_.rpc_reader = MakeDefaultRpcReaderQos();

  auto* factory = DomainParticipantFactory::get_instance();
  Publisher* temporary_publisher = nullptr;
  Subscriber* temporary_subscriber = nullptr;
  std::string xml;
  XmlDefaults xml_defaults;
  participant_qos_ = DomainParticipantExtendedQos{};

  try {
    if (options.fastdds_xml.empty()) {
      participant_qos_.domainId() = options.domain_id;
      participant_qos_.name(options.participant_name.empty() ? GetDdsDefaultParticipantName() : options.participant_name);
    } else {
      xml = ReadFile(options.fastdds_xml);
      xml_defaults = InspectXml(options.fastdds_xml, xml);
      RequireOk(factory->load_XML_profiles_file(options.fastdds_xml), "load_XML_profiles_file",
                options.fastdds_xml, "profiles", "<all>");
      if (xml_defaults.participant) {
        RequireOk(factory->get_default_participant_extended_qos_from_xml(xml, participant_qos_),
                  "get_default_participant_extended_qos_from_xml", options.fastdds_xml, "participant",
                  *xml_defaults.participant);
        qos_.participant_source = "xml_default_profile";
        qos_.participant_profile = *xml_defaults.participant;
      } else {
        participant_qos_.domainId() = options.domain_id;
        participant_qos_.name(options.participant_name.empty() ? GetDdsDefaultParticipantName()
                                                               : options.participant_name);
      }
    }

    auto& process_registry = GetProcessParticipantRegistry();
    {
      std::lock_guard process_lock(process_registry.mutex);
      if (process_registry.owner != nullptr) {
        throw std::logic_error(
            "DDS process participant conflict: requested_domain=" +
            std::to_string(participant_qos_.domainId()) + ", requested_name='" +
            EffectiveParticipantName(participant_qos_) + "', conflicting_domain=" +
            std::to_string(process_registry.domain_id) + ", conflicting_name='" +
            process_registry.participant_name + "'");
      }
      process_registry.owner = this;
      process_registry.domain_id = participant_qos_.domainId();
      process_registry.participant_name = EffectiveParticipantName(participant_qos_);
      process_lease_ = true;
    }

    participant_ = factory->create_participant(participant_qos_);
    if (participant_ == nullptr) {
      throw std::runtime_error("Fast DDS participant creation failed for domain " +
                               std::to_string(participant_qos_.domainId()) + ", source=" +
                               qos_.participant_source);
    }

    if (!xml.empty()) {
      if (xml_defaults.writer) {
        RequireOk(participant_->get_default_publisher_qos_from_xml(xml, qos_.publisher),
                  "get_default_publisher_qos_from_xml", options.fastdds_xml, "publisher/data_writer",
                  *xml_defaults.writer);
        qos_.publisher_source = "xml_default_profile";
        qos_.writer_profile = *xml_defaults.writer;
      }
      if (xml_defaults.reader) {
        RequireOk(participant_->get_default_subscriber_qos_from_xml(xml, qos_.subscriber),
                  "get_default_subscriber_qos_from_xml", options.fastdds_xml, "subscriber/data_reader",
                  *xml_defaults.reader);
        qos_.subscriber_source = "xml_default_profile";
        qos_.reader_profile = *xml_defaults.reader;
      }
      if (xml_defaults.topic) {
        RequireOk(participant_->get_default_topic_qos_from_xml(xml, qos_.topic),
                  "get_default_topic_qos_from_xml", options.fastdds_xml, "topic", *xml_defaults.topic);
        qos_.topic_source = "xml_default_profile";
        qos_.topic_profile = *xml_defaults.topic;
      }

      temporary_publisher = participant_->create_publisher(qos_.publisher);
      temporary_subscriber = participant_->create_subscriber(qos_.subscriber);
      if (temporary_publisher == nullptr || temporary_subscriber == nullptr) {
        if (temporary_publisher != nullptr) participant_->delete_publisher(temporary_publisher);
        if (temporary_subscriber != nullptr) participant_->delete_subscriber(temporary_subscriber);
        temporary_publisher = nullptr;
        temporary_subscriber = nullptr;
        throw std::runtime_error("Fast DDS XML publisher/subscriber effective QoS is invalid for '" +
                                 options.fastdds_xml + "'");
      }
      if (xml_defaults.writer) {
        RequireOk(temporary_publisher->get_default_datawriter_qos_from_xml(xml, qos_.channel_writer),
                  "get_default_datawriter_qos_from_xml", options.fastdds_xml, "data_writer",
                  *xml_defaults.writer);
        qos_.rpc_writer = qos_.channel_writer;
        qos_.writer_source = "xml_default_profile";
        qos_.rpc_request_writer_max_blocking_time_configured =
            xml_defaults.writer_max_blocking_time_configured;
        if (xml_defaults.writer_max_blocking_time_configured) {
          qos_.rpc_request_writer_max_blocking_time_ns =
              qos_.rpc_writer.reliability().max_blocking_time.to_ns();
        }
      }
      if (xml_defaults.reader) {
        RequireOk(temporary_subscriber->get_default_datareader_qos_from_xml(xml, qos_.channel_reader),
                  "get_default_datareader_qos_from_xml", options.fastdds_xml, "data_reader",
                  *xml_defaults.reader);
        qos_.rpc_reader = qos_.channel_reader;
        qos_.reader_source = "xml_default_profile";
      }
      RequireOk(participant_->delete_publisher(temporary_publisher), "delete_publisher", options.fastdds_xml,
                "publisher", qos_.writer_profile);
      temporary_publisher = nullptr;
      RequireOk(participant_->delete_subscriber(temporary_subscriber), "delete_subscriber", options.fastdds_xml,
                "subscriber", qos_.reader_profile);
      temporary_subscriber = nullptr;
      SetFixedXcdr2(qos_.channel_writer);
      SetFixedXcdr2(qos_.channel_reader);
      SetFixedXcdr2(qos_.rpc_writer);
      SetFixedXcdr2(qos_.rpc_reader);
    }

    ValidateAllEndpointQos(qos_, options.fastdds_xml);

    state_ = State::kInitialized;
  } catch (...) {
    if (participant_ != nullptr) {
      if (temporary_publisher != nullptr) participant_->delete_publisher(temporary_publisher);
      if (temporary_subscriber != nullptr) participant_->delete_subscriber(temporary_subscriber);
      if (factory->delete_participant(participant_) == RETCODE_OK) participant_ = nullptr;
    }
    if (process_lease_) {
      auto& process_registry = GetProcessParticipantRegistry();
      std::lock_guard process_lock(process_registry.mutex);
      if (process_registry.owner == this) {
        process_registry.owner = nullptr;
        process_registry.participant_name.clear();
      }
      process_lease_ = false;
    }
    state_ = State::kStopped;
    throw;
  }
}

void DdsParticipantContext::Start() {
  std::lock_guard lock(mutex_);
  if (state_ == State::kRunning) return;
  if (state_ != State::kInitialized || participant_ == nullptr) {
    throw std::logic_error("DDS participant context must be initialized before Start");
  }
  state_ = State::kRunning;
}

void DdsParticipantContext::Shutdown() {
  std::lock_guard lock(mutex_);
  if (participant_ != nullptr) {
    const auto result = DomainParticipantFactory::get_instance()->delete_participant(participant_);
    if (result != RETCODE_OK) {
      throw std::runtime_error("Fast DDS delete_participant failed with return code " + std::to_string(result));
    }
    participant_ = nullptr;
  }
  if (process_lease_) {
    auto& process_registry = GetProcessParticipantRegistry();
    std::lock_guard process_lock(process_registry.mutex);
    if (process_registry.owner == this) {
      process_registry.owner = nullptr;
      process_registry.participant_name.clear();
    }
    process_lease_ = false;
  }
  state_ = State::kStopped;
}

DdsParticipantContext::State DdsParticipantContext::GetState() const {
  std::lock_guard lock(mutex_);
  return state_;
}

DomainParticipant* DdsParticipantContext::Participant() const {
  std::lock_guard lock(mutex_);
  return participant_;
}

const DdsPluginOptions& DdsParticipantContext::Options() const { return options_; }
const DdsQosSnapshot& DdsParticipantContext::Qos() const { return qos_; }
uint32_t DdsParticipantContext::DomainId() const { return participant_qos_.domainId(); }
std::string DdsParticipantContext::ParticipantName() const { return EffectiveParticipantName(participant_qos_); }

std::list<std::pair<std::string, std::string>> DdsParticipantContext::GenInitializationReport() const {
  std::lock_guard lock(mutex_);
  std::list<std::pair<std::string, std::string>> report;
  report.emplace_back("DDS Participant",
                      "domain=" + std::to_string(participant_qos_.domainId()) +
                          ", name=" + EffectiveParticipantName(participant_qos_) +
                          ", source=" + qos_.participant_source + ", count=1");
  report.emplace_back("DDS XML", options_.fastdds_xml.empty() ? "disabled" : options_.fastdds_xml);
  report.emplace_back("DDS Entity QoS Sources",
                      "participant=" + qos_.participant_source + ", publisher=" + qos_.publisher_source +
                          ", subscriber=" + qos_.subscriber_source + ", topic=" + qos_.topic_source +
                          ", data_writer=" + qos_.writer_source + ", data_reader=" + qos_.reader_source);
  report.emplace_back("DDS Executor", "type=asio_thread, configured_thread_num=" +
                                          std::to_string(options_.executor.thread_num) +
                                          ", effective_thread_num=" +
                                          std::to_string(effective_executor_thread_num_));
  report.emplace_back("DDS Channel QoS",
                      "writer_source=" + qos_.writer_source + ", reader_source=" + qos_.reader_source +
                          ", writer_reliability=" + ReliabilityName(qos_.channel_writer.reliability().kind) +
                          ", reader_reliability=" + ReliabilityName(qos_.channel_reader.reliability().kind) +
                          ", writer_durability=" + DurabilityName(qos_.channel_writer.durability().kind) +
                          ", reader_durability=" + DurabilityName(qos_.channel_reader.durability().kind) +
                          ", writer_history=" + HistoryName(qos_.channel_writer.history().kind, qos_.channel_writer.history().depth) +
                          ", reader_history=" + HistoryName(qos_.channel_reader.history().kind, qos_.channel_reader.history().depth) +
                          ", writer_deadline=" + DurationName(qos_.channel_writer.deadline().period) +
                          ", reader_deadline=" + DurationName(qos_.channel_reader.deadline().period) +
                          ", writer_liveliness=" + LivelinessName(qos_.channel_writer.liveliness().kind) + "/" +
                          DurationName(qos_.channel_writer.liveliness().lease_duration) +
                          ", reader_liveliness=" + LivelinessName(qos_.channel_reader.liveliness().kind) + "/" +
                          DurationName(qos_.channel_reader.liveliness().lease_duration) +
                          ", resource_limits=FastDDS_effective, writer_max_blocking_time=FastDDS_effective" +
                          ", representation=XCDR2");
  report.emplace_back("DDS RPC QoS",
                      "writer_source=" + qos_.writer_source + ", reader_source=" + qos_.reader_source +
                          ", writer_reliability=" + ReliabilityName(qos_.rpc_writer.reliability().kind) +
                          ", reader_reliability=" + ReliabilityName(qos_.rpc_reader.reliability().kind) +
                          ", writer_durability=" + DurabilityName(qos_.rpc_writer.durability().kind) +
                          ", reader_durability=" + DurabilityName(qos_.rpc_reader.durability().kind) +
                          ", writer_history=" + HistoryName(qos_.rpc_writer.history().kind, qos_.rpc_writer.history().depth) +
                          ", reader_history=" + HistoryName(qos_.rpc_reader.history().kind, qos_.rpc_reader.history().depth) +
                          ", writer_deadline=" + DurationName(qos_.rpc_writer.deadline().period) +
                          ", reader_deadline=" + DurationName(qos_.rpc_reader.deadline().period) +
                          ", writer_liveliness=" + LivelinessName(qos_.rpc_writer.liveliness().kind) + "/" +
                          DurationName(qos_.rpc_writer.liveliness().lease_duration) +
                          ", reader_liveliness=" + LivelinessName(qos_.rpc_reader.liveliness().kind) + "/" +
                          DurationName(qos_.rpc_reader.liveliness().lease_duration) +
                          ", resource_limits=FastDDS_effective, request_writer_max_blocking_time=0us" +
                          ", request_writer_max_blocking_time_source=fixed_rpc_zero" +
                          ", response_writer_max_blocking_time=" +
                          DurationName(qos_.rpc_writer.reliability().max_blocking_time) +
                          ", representation=XCDR2");
  report.emplace_back("DDS XML unsupported fields", "DataRepresentation is fixed to XCDR2 and is not read from XML");
  return report;
}

}  // namespace aimrt::plugins::dds_plugin
