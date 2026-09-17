// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "dds_plugin/dds_config.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>
#include <fastdds/dds/core/policy/QosPolicies.hpp>

#include "core/aimrt_core.h"

namespace aimrt::plugins::dds_plugin {
namespace {

using namespace eprosima::fastdds::dds;

class TemporaryXml {
 public:
  TemporaryXml(std::string_view name, std::string_view contents)
      : path_(std::filesystem::temp_directory_path() /
              ("aimrt_dds_" + std::to_string(getpid()) + "_" + std::string(name) + ".xml")) {
    std::ofstream output(path_, std::ios::binary);
    output << contents;
    output.close();
  }

  ~TemporaryXml() { std::filesystem::remove(path_); }
  std::string Path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

constexpr std::string_view kReliableXml = R"xml(<?xml version="1.0" encoding="utf-8"?>
<dds xmlns="http://www.eprosima.com">
  <profiles>
    <participant profile_name="aimrt_participant" is_default_profile="true">
      <domainId>231</domainId>
      <rtps><name>aimrt_xml_participant</name></rtps>
    </participant>
    <data_writer profile_name="aimrt_writer" is_default_profile="true">
      <qos>
        <reliability><kind>RELIABLE</kind></reliability>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <partition><names><name>aimrt_xml_partition</name></names></partition>
      </qos>
      <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
    </data_writer>
    <data_reader profile_name="aimrt_reader" is_default_profile="true">
      <qos>
        <reliability><kind>RELIABLE</kind></reliability>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <partition><names><name>aimrt_xml_partition</name></names></partition>
      </qos>
      <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
    </data_reader>
    <topic profile_name="aimrt_topic" is_default_profile="true">
      <historyQos><kind>KEEP_ALL</kind></historyQos>
    </topic>
  </profiles>
</dds>)xml";

constexpr std::string_view kRpcZeroBlockingXml = R"xml(
<dds xmlns="http://www.eprosima.com"><profiles>
  <data_writer profile_name="rpc_zero_writer" is_default_profile="true">
    <qos><reliability><kind>RELIABLE</kind>
      <max_blocking_time><sec>0</sec><nanosec>0</nanosec></max_blocking_time>
    </reliability></qos>
    <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
  </data_writer>
  <data_reader profile_name="rpc_zero_reader" is_default_profile="true">
    <qos><reliability><kind>RELIABLE</kind></reliability></qos>
    <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
  </data_reader>
</profiles></dds>)xml";

constexpr std::string_view kRpcNonzeroBlockingXml = R"xml(
<dds xmlns="http://www.eprosima.com"><profiles>
  <data_writer profile_name="rpc_nonzero_writer" is_default_profile="true">
    <qos><reliability><kind>RELIABLE</kind>
      <max_blocking_time><sec>0</sec><nanosec>100000000</nanosec></max_blocking_time>
    </reliability></qos>
    <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
  </data_writer>
  <data_reader profile_name="rpc_nonzero_reader" is_default_profile="true">
    <qos><reliability><kind>RELIABLE</kind></reliability></qos>
    <topic><historyQos><kind>KEEP_ALL</kind></historyQos></topic>
  </data_reader>
</profiles></dds>)xml";

TEST(DdsConfigDefaults, UsesSingleParticipantAndFrozenDefaults) {
  const auto options = DecodeDdsPluginOptions(YAML::Load("{}"));
  EXPECT_EQ(options.domain_id, 0U);
  EXPECT_TRUE(options.participant_name.empty());
  EXPECT_TRUE(options.fastdds_xml.empty());
  EXPECT_EQ(options.executor.type, "asio_thread");
  EXPECT_EQ(options.executor.thread_num, 0U);
  EXPECT_EQ(ResolveDdsExecutorThreadNum(0, 0), 2U);
  EXPECT_EQ(ResolveDdsExecutorThreadNum(0, 2), 2U);
  EXPECT_EQ(ResolveDdsExecutorThreadNum(0, 16), 4U);
  EXPECT_EQ(ResolveDdsExecutorThreadNum(7, 16), 7U);

  DdsParticipantContext context;
  context.Initialize(DdsPluginOptions{.domain_id = 230});
  ASSERT_NE(context.Participant(), nullptr);
  EXPECT_EQ(context.DomainId(), 230U);
  EXPECT_FALSE(context.ParticipantName().empty());
  EXPECT_EQ(context.Qos().channel_writer.reliability().kind, BEST_EFFORT_RELIABILITY_QOS);
  EXPECT_EQ(context.Qos().channel_writer.history().kind, KEEP_LAST_HISTORY_QOS);
  EXPECT_EQ(context.Qos().channel_writer.history().depth, 20);
  EXPECT_EQ(context.Qos().rpc_writer.reliability().kind, RELIABLE_RELIABILITY_QOS);
  EXPECT_EQ(context.Qos().rpc_writer.history().kind, KEEP_ALL_HISTORY_QOS);
  EXPECT_EQ(context.Qos().rpc_writer.data_sharing().kind(), OFF);
  EXPECT_EQ(context.Qos().rpc_reader.data_sharing().kind(), OFF);
  EXPECT_EQ(context.Qos().channel_writer.representation().m_value,
            (std::vector<DataRepresentationId_t>{XCDR2_DATA_REPRESENTATION}));
  const auto report = context.GenInitializationReport();
  EXPECT_EQ(report.size(), 7U);
  const auto participant_report = std::ranges::find_if(report, [](const auto& entry) {
    return entry.first == "DDS Participant";
  });
  ASSERT_NE(participant_report, report.end());
  EXPECT_NE(participant_report->second.find("source=plugin_default"), std::string::npos);
  EXPECT_NE(participant_report->second.find("count=1"), std::string::npos);
  const auto channel_report = std::ranges::find_if(report, [](const auto& entry) {
    return entry.first == "DDS Channel QoS";
  });
  ASSERT_NE(channel_report, report.end());
  EXPECT_NE(channel_report->second.find("representation=XCDR2"), std::string::npos);
  context.Shutdown();
}

TEST(DdsConfigXmlDefaults, UsesWholeFastDdsDefaultProfiles) {
  TemporaryXml xml("reliable_defaults", kReliableXml);
  DdsParticipantContext context;
  context.Initialize(DdsPluginOptions{.domain_id = 17,
                                      .participant_name = "must_not_override_xml",
                                      .fastdds_xml = xml.Path()});
  EXPECT_EQ(context.DomainId(), 231U);
  EXPECT_EQ(context.ParticipantName(), "aimrt_xml_participant");
  EXPECT_EQ(context.Qos().participant_source, "xml_default_profile");
  EXPECT_EQ(context.Qos().writer_source, "xml_default_profile");
  EXPECT_EQ(context.Qos().reader_source, "xml_default_profile");
  ASSERT_EQ(context.Qos().publisher.partition().names().size(), 1U);
  EXPECT_EQ(context.Qos().publisher.partition().names().front(), "aimrt_xml_partition");
  ASSERT_EQ(context.Qos().subscriber.partition().names().size(), 1U);
  EXPECT_EQ(context.Qos().subscriber.partition().names().front(), "aimrt_xml_partition");
  EXPECT_EQ(context.Qos().channel_writer.reliability().kind, RELIABLE_RELIABILITY_QOS);
  EXPECT_EQ(context.Qos().channel_writer.durability().kind, TRANSIENT_LOCAL_DURABILITY_QOS);
  EXPECT_EQ(context.Qos().channel_writer.history().kind, KEEP_ALL_HISTORY_QOS);
  EXPECT_EQ(context.Qos().rpc_writer, context.Qos().channel_writer);
  EXPECT_EQ(context.Qos().rpc_reader, context.Qos().channel_reader);
  context.Shutdown();
}

TEST(DdsConfigXmlInvalid, RejectsMissingMalformedDuplicateAndInvalidQos) {
  DdsParticipantContext missing;
  try {
    missing.Initialize(DdsPluginOptions{.fastdds_xml = "/tmp/aimrt_dds_missing_profile.xml"});
    FAIL() << "missing XML file was accepted";
  } catch (const std::runtime_error& error) {
    const std::string diagnostic = error.what();
    EXPECT_NE(diagnostic.find("file='/tmp/aimrt_dds_missing_profile.xml'"), std::string::npos);
    EXPECT_NE(diagnostic.find("entity='profiles'"), std::string::npos);
    EXPECT_NE(diagnostic.find("profile='<all>'"), std::string::npos);
  }

  TemporaryXml malformed("malformed", "<dds><profiles>");
  DdsParticipantContext malformed_context;
  try {
    malformed_context.Initialize(DdsPluginOptions{.fastdds_xml = malformed.Path()});
    FAIL() << "malformed XML was accepted";
  } catch (const std::runtime_error& error) {
    const std::string diagnostic = error.what();
    EXPECT_NE(diagnostic.find("file='" + malformed.Path() + "'"), std::string::npos);
    EXPECT_NE(diagnostic.find("entity='profiles'"), std::string::npos);
    EXPECT_NE(diagnostic.find("profile='<all>'"), std::string::npos);
  }

  TemporaryXml duplicate("duplicate", R"xml(
    <dds xmlns="http://www.eprosima.com"><profiles>
      <data_writer profile_name="first" is_default_profile="true"/>
      <data_writer profile_name="second" is_default_profile="true"/>
    </profiles></dds>)xml");
  DdsParticipantContext duplicate_context;
  try {
    duplicate_context.Initialize(DdsPluginOptions{.fastdds_xml = duplicate.Path()});
    FAIL() << "duplicate XML defaults were accepted";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find(duplicate.Path()), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("data_writer"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("first"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("second"), std::string::npos);
  }

  TemporaryXml invalid_qos("invalid_qos", R"xml(
    <dds xmlns="http://www.eprosima.com"><profiles>
      <data_writer profile_name="invalid" is_default_profile="true">
        <qos><reliability><kind>NOT_A_RELIABILITY_KIND</kind></reliability></qos>
      </data_writer>
    </profiles></dds>)xml");
  DdsParticipantContext invalid_context;
  EXPECT_THROW(invalid_context.Initialize(DdsPluginOptions{.fastdds_xml = invalid_qos.Path()}),
               std::runtime_error);

  TemporaryXml conflicting_qos("conflicting_qos", R"xml(
    <dds xmlns="http://www.eprosima.com"><profiles>
      <data_writer profile_name="conflicting_writer" is_default_profile="true">
        <qos><reliability><kind>RELIABLE</kind></reliability></qos>
        <topic>
          <historyQos><kind>KEEP_LAST</kind><depth>8</depth></historyQos>
          <resourceLimitsQos>
            <max_samples>4</max_samples><max_instances>1</max_instances>
            <max_samples_per_instance>4</max_samples_per_instance>
          </resourceLimitsQos>
        </topic>
      </data_writer>
    </profiles></dds>)xml");
  DdsParticipantContext conflicting_context;
  try {
    conflicting_context.Initialize(DdsPluginOptions{.fastdds_xml = conflicting_qos.Path()});
    FAIL() << "internally inconsistent effective QoS was accepted";
  } catch (const std::invalid_argument& error) {
    const std::string diagnostic = error.what();
    EXPECT_NE(diagnostic.find(conflicting_qos.Path()), std::string::npos);
    EXPECT_NE(diagnostic.find("entity='channel_writer'"), std::string::npos);
    EXPECT_NE(diagnostic.find("profile='conflicting_writer'"), std::string::npos);
    EXPECT_NE(diagnostic.find("depth exceeds max_samples_per_instance"), std::string::npos);
  }
}

TEST(DdsConfigNameValidation, RejectsNonCanonicalAndReservedNames) {
  try {
    ValidateDdsChannelTopic("req/business");
    FAIL() << "reserved Channel topic was accepted";
  } catch (const std::invalid_argument& error) {
    const std::string diagnostic = error.what();
    EXPECT_NE(diagnostic.find("original_name='req/business'"), std::string::npos);
    EXPECT_NE(diagnostic.find("derived_topic='req/business'"), std::string::npos);
  }
  EXPECT_THROW(ValidateDdsChannelTopic(""), std::invalid_argument);
  EXPECT_THROW(ValidateDdsChannelTopic("rsp/business"), std::invalid_argument);
  EXPECT_THROW(ValidateDdsChannelTopic("business/../topic"), std::invalid_argument);
  EXPECT_THROW(ValidateDdsChannelTopic("business topic"), std::invalid_argument);
  EXPECT_NO_THROW(ValidateDdsChannelTopic("business/topic"));

  EXPECT_THROW(DeriveDdsRpcTopicNames("dds:/example::Calculator"), std::invalid_argument);
  EXPECT_THROW(DeriveDdsRpcTopicNames("dds:/::example::Calculator/Add"), std::invalid_argument);
  EXPECT_THROW(DeriveDdsRpcTopicNames("dds:/example::Calculator/Add/Extra"), std::invalid_argument);
  EXPECT_THROW(DeriveDdsRpcTopicNames("unknown:/example/Call"), std::invalid_argument);
  try {
    DeriveDdsRpcTopicNames("dds:/example::Calculator/Add Method");
    FAIL() << "invalid RPC function was accepted";
  } catch (const std::invalid_argument& error) {
    const std::string diagnostic = error.what();
    EXPECT_NE(diagnostic.find("original_name='dds:/example::Calculator/Add Method'"),
              std::string::npos);
    EXPECT_NE(diagnostic.find("derived_request='req/dds/example::Calculator/Add Method'"),
              std::string::npos);
    EXPECT_NE(diagnostic.find("derived_response='rsp/dds/example::Calculator/Add Method'"),
              std::string::npos);
  }
  EXPECT_THROW(DeriveDdsRpcTopicNames("dds:/example::Calculator/Add.Method"), std::invalid_argument);
}

TEST(DdsConfigBackendAndFixedTopics, DerivesOnlyTheFrozenWireNames) {
  const auto native = DeriveDdsRpcTopicNames("dds:/example::Calculator/Add");
  EXPECT_EQ(native.request, "req/dds/example::Calculator/Add");
  EXPECT_EQ(native.response, "rsp/dds/example::Calculator/Add");
  const auto protobuf = DeriveDdsRpcTopicNames("pb:/example.Calculator/Add");
  EXPECT_EQ(protobuf.request, "req/pb/example.Calculator/Add");
  const auto ros2 = DeriveDdsRpcTopicNames("ros2:/example_msgs.srv.Calculator/Add");
  EXPECT_EQ(ros2.response, "rsp/ros2/example_msgs.srv.Calculator/Add");

  DdsNameRegistry names;
  names.RegisterChannel("business/topic");
  EXPECT_NO_THROW(names.RegisterRpc("dds:/example::Calculator/Add"));
  EXPECT_NO_THROW(names.RegisterRpc("dds:/example::Calculator/Add"));

  DdsNameRegistry collision;
  collision.RegisterChannel("business/topic", "Channel object type dds:example::First");
  try {
    collision.RegisterChannel("business/topic", "Channel object type dds:example::Second");
    FAIL() << "conflicting physical topic objects were accepted";
  } catch (const std::invalid_argument& error) {
    const std::string diagnostic = error.what();
    EXPECT_NE(diagnostic.find("physical_topic='business/topic'"), std::string::npos);
    EXPECT_NE(diagnostic.find("incoming_original_name='business/topic'"), std::string::npos);
    EXPECT_NE(diagnostic.find("incoming_derived_topics='business/topic'"), std::string::npos);
    EXPECT_NE(diagnostic.find("conflicting_derived_topics='business/topic'"), std::string::npos);
    EXPECT_NE(diagnostic.find("dds:example::First"), std::string::npos);
    EXPECT_NE(diagnostic.find("dds:example::Second"), std::string::npos);
  }
}

TEST(DdsConfigProcessUniqueness, RejectsASecondParticipantAndDomainProcessWide) {
  DdsParticipantContext first;
  first.Initialize(DdsPluginOptions{.domain_id = 225, .participant_name = "first_process_owner"});
  DdsParticipantContext second;
  try {
    second.Initialize(DdsPluginOptions{.domain_id = 226, .participant_name = "second_process_owner"});
    FAIL() << "a second process participant was accepted";
  } catch (const std::logic_error& error) {
    const std::string diagnostic = error.what();
    EXPECT_NE(diagnostic.find("requested_domain=226"), std::string::npos);
    EXPECT_NE(diagnostic.find("requested_name='second_process_owner'"), std::string::npos);
    EXPECT_NE(diagnostic.find("conflicting_domain=225"), std::string::npos);
    EXPECT_NE(diagnostic.find("conflicting_name='first_process_owner'"), std::string::npos);
  }
  EXPECT_EQ(second.Participant(), nullptr);
  first.Shutdown();
}

TEST(DdsRpcQosContract, RequiresReliableButPreservesWholeXmlHistory) {
  DdsParticipantContext defaults;
  defaults.Initialize(DdsPluginOptions{.domain_id = 232});
  EXPECT_NO_THROW(ValidateDdsRpcQos(defaults.Qos(), "dds:/example::Calculator/Add"));
  defaults.Shutdown();

  DdsQosSnapshot custom;
  custom.writer_source = "xml_default_profile";
  custom.reader_source = "xml_default_profile";
  custom.rpc_writer.reliability().kind = RELIABLE_RELIABILITY_QOS;
  custom.rpc_reader.reliability().kind = RELIABLE_RELIABILITY_QOS;
  custom.rpc_writer.history().kind = KEEP_LAST_HISTORY_QOS;
  custom.rpc_writer.history().depth = 7;
  EXPECT_NO_THROW(ValidateDdsRpcQos(custom, "dds:/example::Calculator/Add"));
  EXPECT_EQ(custom.rpc_writer.history().depth, 7);

  custom.rpc_writer.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
  EXPECT_THROW(ValidateDdsRpcQos(custom, "dds:/example::Calculator/Add"), std::invalid_argument);
  custom.rpc_writer.reliability().kind = RELIABLE_RELIABILITY_QOS;
  custom.rpc_reader.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
  EXPECT_THROW(ValidateDdsRpcQos(custom, "dds:/example::Calculator/Add"), std::invalid_argument);
}

TEST(DdsRpcQosContract, FixesRequestWriterAtZeroAndRejectsNonzeroXml) {
  TemporaryXml zero_xml("rpc_zero_blocking", kRpcZeroBlockingXml);
  DdsParticipantContext zero;
  zero.Initialize(DdsPluginOptions{.domain_id = 220, .fastdds_xml = zero_xml.Path()});
  EXPECT_TRUE(zero.Qos().rpc_request_writer_max_blocking_time_configured);
  EXPECT_EQ(zero.Qos().rpc_request_writer_max_blocking_time_ns, 0);
  EXPECT_NO_THROW(
      ValidateDdsRpcQos(zero.Qos(), "dds:/example::Calculator/Add"));
  zero.Shutdown();

  TemporaryXml nonzero_xml("rpc_nonzero_blocking", kRpcNonzeroBlockingXml);
  DdsParticipantContext nonzero;
  nonzero.Initialize(
      DdsPluginOptions{.domain_id = 221, .fastdds_xml = nonzero_xml.Path()});
  EXPECT_TRUE(nonzero.Qos().rpc_request_writer_max_blocking_time_configured);
  EXPECT_EQ(nonzero.Qos().rpc_request_writer_max_blocking_time_ns, 100000000);
  try {
    ValidateDdsRpcQos(nonzero.Qos(), "dds:/example::Calculator/Add");
    FAIL() << "nonzero RPC request writer max_blocking_time was accepted";
  } catch (const std::invalid_argument& error) {
    const std::string diagnostic = error.what();
    EXPECT_NE(diagnostic.find("function 'dds:/example::Calculator/Add'"),
              std::string::npos);
    EXPECT_NE(diagnostic.find("endpoint_role=request_writer"), std::string::npos);
    EXPECT_NE(diagnostic.find("profile='rpc_nonzero_writer'"), std::string::npos);
    EXPECT_NE(diagnostic.find("configured=100000000ns"), std::string::npos);
    EXPECT_NE(diagnostic.find("required=0us"), std::string::npos);
  }
  nonzero.Shutdown();
}

TEST(DdsLifecycleCycles, InitializeStartShutdownIsStrictAndRepeatable) {
  for (uint32_t cycle = 0; cycle < 3; ++cycle) {
    DdsParticipantContext context;
    context.Initialize(DdsPluginOptions{.domain_id = 229, .participant_name = "aimrt_dds_lifecycle"});
    EXPECT_EQ(context.GetState(), DdsParticipantContext::State::kInitialized);
    EXPECT_THROW(context.Initialize(DdsPluginOptions{}), std::logic_error);
    context.Start();
    context.Start();
    EXPECT_EQ(context.GetState(), DdsParticipantContext::State::kRunning);
    context.Shutdown();
    context.Shutdown();
    EXPECT_EQ(context.GetState(), DdsParticipantContext::State::kStopped);
    EXPECT_EQ(context.Participant(), nullptr);
  }

  const char* plugin_path = std::getenv("AIMRT_DDS_PLUGIN_PATH");
  ASSERT_NE(plugin_path, nullptr);
  for (uint32_t cycle = 0; cycle < 3; ++cycle) {
    TemporaryXml config(
        "core_lifecycle_" + std::to_string(cycle),
        "aimrt:\n"
        "  plugin:\n"
        "    plugins:\n"
        "      - name: dds_plugin\n"
        "        path: " +
            std::string(plugin_path) +
            "\n"
            "        options:\n"
            "          domain_id: 228\n"
            "          participant_name: aimrt_dds_core_lifecycle\n"
            "  channel:\n"
            "    backends:\n"
            "      - type: dds\n"
            "        options: {}\n"
            "  rpc:\n"
            "    backends:\n"
            "      - type: dds\n"
            "        options: {}\n"
            "  log:\n"
            "    core_lvl: Error\n"
            "    backends:\n"
            "      - type: console\n");
    runtime::core::AimRTCore core;
    core.Initialize({.cfg_file_path = config.Path()});
    auto completion = core.AsyncStart();
    core.Shutdown();
    EXPECT_EQ(completion.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  }
}

}  // namespace
}  // namespace aimrt::plugins::dds_plugin
