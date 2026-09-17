// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <yaml-cpp/yaml.h>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantExtendedQos.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/publisher/qos/PublisherQos.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/subscriber/qos/SubscriberQos.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>

namespace aimrt::plugins::dds_plugin {

struct DdsExecutorOptions {
  std::string type = "asio_thread";
  uint32_t thread_num = 0;
};

struct DdsPluginOptions {
  uint32_t domain_id = 0;
  std::string participant_name;
  std::string fastdds_xml;
  DdsExecutorOptions executor;
};

DdsPluginOptions DecodeDdsPluginOptions(const YAML::Node& node);
YAML::Node EncodeDdsPluginOptions(const DdsPluginOptions& options);
uint32_t ResolveDdsExecutorThreadNum(uint32_t configured, uint32_t hardware_concurrency);
std::string GetDdsDefaultParticipantName();

struct DdsRpcTopicNames {
  std::string request;
  std::string response;
};

void ValidateDdsChannelTopic(std::string_view topic);
DdsRpcTopicNames DeriveDdsRpcTopicNames(std::string_view function_name);

class DdsNameRegistry {
 public:
  void RegisterChannel(std::string_view topic, std::string_view object = "Channel registration");
  DdsRpcTopicNames RegisterRpc(std::string_view function_name);

 private:
  void InsertPhysicalName(std::string_view physical_name, std::string_view owner,
                          std::string_view original_name, std::string_view derived_topics);

  struct PhysicalOwner {
    std::string owner;
    std::string original_name;
    std::string derived_topics;
  };
  std::map<std::string, PhysicalOwner, std::less<>> physical_names_;
};

struct DdsQosSnapshot {
  eprosima::fastdds::dds::PublisherQos publisher;
  eprosima::fastdds::dds::SubscriberQos subscriber;
  eprosima::fastdds::dds::TopicQos topic;
  eprosima::fastdds::dds::DataWriterQos channel_writer;
  eprosima::fastdds::dds::DataReaderQos channel_reader;
  eprosima::fastdds::dds::DataWriterQos rpc_writer;
  eprosima::fastdds::dds::DataReaderQos rpc_reader;
  std::string participant_source = "plugin_default";
  std::string publisher_source = "plugin_default";
  std::string subscriber_source = "plugin_default";
  std::string topic_source = "plugin_default";
  std::string writer_source = "plugin_default";
  std::string reader_source = "plugin_default";
  std::string participant_profile;
  std::string writer_profile;
  std::string reader_profile;
  std::string topic_profile;
  bool rpc_request_writer_max_blocking_time_configured = false;
  int64_t rpc_request_writer_max_blocking_time_ns = 0;
};

void ValidateDdsRpcQos(const DdsQosSnapshot& qos, std::string_view function_name);

class DdsParticipantContext {
 public:
  enum class State : uint8_t { kStopped,
                               kInitialized,
                               kRunning };

  DdsParticipantContext() = default;
  ~DdsParticipantContext();
  DdsParticipantContext(const DdsParticipantContext&) = delete;
  DdsParticipantContext& operator=(const DdsParticipantContext&) = delete;

  void Initialize(const DdsPluginOptions& options);
  void Start();
  void Shutdown();

  State GetState() const;
  eprosima::fastdds::dds::DomainParticipant* Participant() const;
  const DdsPluginOptions& Options() const;
  const DdsQosSnapshot& Qos() const;
  uint32_t DomainId() const;
  std::string ParticipantName() const;
  std::list<std::pair<std::string, std::string>> GenInitializationReport() const;

 private:
  mutable std::mutex mutex_;
  State state_ = State::kStopped;
  DdsPluginOptions options_;
  uint32_t effective_executor_thread_num_ = 0;
  eprosima::fastdds::dds::DomainParticipantExtendedQos participant_qos_;
  DdsQosSnapshot qos_;
  eprosima::fastdds::dds::DomainParticipant* participant_ = nullptr;
  bool process_lease_ = false;
};

}  // namespace aimrt::plugins::dds_plugin
