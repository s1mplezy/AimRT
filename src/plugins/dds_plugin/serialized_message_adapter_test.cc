// Copyright (c) 2026 The AimRT Authors.
// AimRT is licensed under Mulan PSL v2.

#include "dds_plugin/serialized_message_adapter.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>

#include "aimrt_module_cpp_interface/util/buffer.h"
#include "detail/serialized_message/serialized_messagePubSubTypes.hpp"

#if defined(AIMRT_DDS_ENABLE_XTYPES)
  #include "detail/serialized_message/serialized_messageTypeObjectSupport.hpp"
#endif

namespace aimrt::plugins::dds_plugin {
namespace {

std::string Bytes(size_t size) { return std::string(size, 'x'); }

TEST(DdsWrapperBounds, AcceptsEveryHardBoundary) {
  const auto maximum_key = Bytes(kSerializedMessageKeyMaxBytes);
  const auto maximum_value = Bytes(kSerializedMessageValueMaxBytes);
  const std::array metadata{
      SerializedMetadataView{.key = maximum_key, .value = maximum_value}};
  const auto result = ValidateSerializedMessageInput(
      Bytes(kSerializedMessageTypeNameMaxBytes),
      Bytes(kSerializedMessageSerializationTypeMaxBytes), metadata,
      kSerializedMessageDataMaxBytes, SerializedMessageUsage::kChannel);
  EXPECT_TRUE(result.ok) << result.reason;

  std::vector<SerializedMetadataView> maximum_metadata(
      kSerializedMessageMetadataMaxEntries,
      SerializedMetadataView{.key = "k", .value = "v"});
  const auto metadata_result = ValidateSerializedMessageInput(
      "pb:test.Message", "pb", maximum_metadata, 0,
      SerializedMessageUsage::kChannel);
  EXPECT_TRUE(metadata_result.ok) << metadata_result.reason;
}

TEST(DdsWrapperBounds, RejectsEveryBoundaryPlusOne) {
  EXPECT_FALSE(ValidateSerializedMessageInput(
                   Bytes(kSerializedMessageTypeNameMaxBytes + 1), "pb", {}, 0,
                   SerializedMessageUsage::kChannel)
                   .ok);
  EXPECT_FALSE(ValidateSerializedMessageInput(
                   "pb:test.Message",
                   Bytes(kSerializedMessageSerializationTypeMaxBytes + 1), {},
                   0, SerializedMessageUsage::kChannel)
                   .ok);

  std::vector<SerializedMetadataView> too_many_metadata(
      kSerializedMessageMetadataMaxEntries + 1,
      SerializedMetadataView{.key = "k", .value = "v"});
  EXPECT_FALSE(ValidateSerializedMessageInput(
                   "pb:test.Message", "pb", too_many_metadata, 0,
                   SerializedMessageUsage::kChannel)
                   .ok);

  const auto too_long_key = Bytes(kSerializedMessageKeyMaxBytes + 1);
  const std::array long_key{
      SerializedMetadataView{.key = too_long_key, .value = "v"}};
  EXPECT_FALSE(ValidateSerializedMessageInput(
                   "pb:test.Message", "pb", long_key, 0,
                   SerializedMessageUsage::kChannel)
                   .ok);

  const auto too_long_value = Bytes(kSerializedMessageValueMaxBytes + 1);
  const std::array long_value{
      SerializedMetadataView{.key = "k", .value = too_long_value}};
  EXPECT_FALSE(ValidateSerializedMessageInput(
                   "pb:test.Message", "pb", long_value, 0,
                   SerializedMessageUsage::kChannel)
                   .ok);

  EXPECT_FALSE(ValidateSerializedMessageInput(
                   "pb:test.Message", "pb", {},
                   kSerializedMessageDataMaxBytes + 1,
                   SerializedMessageUsage::kChannel)
                   .ok);
}

TEST(DdsWrapperBounds, RejectsAllocationBeforeCrossingPayloadLimit) {
  BoundedBufferArrayAllocator allocator(8);
  aimrt::util::BufferArray buffers(allocator.NativeHandle());
  const auto first = buffers.NewBuffer(8);
  ASSERT_NE(first.data, nullptr);
  EXPECT_EQ(allocator.AllocatedBytes(), 8U);

  const auto rejected = buffers.NewBuffer(1);
  EXPECT_EQ(rejected.data, nullptr);
  EXPECT_EQ(rejected.len, 0U);
  EXPECT_TRUE(allocator.LimitExceeded());
  EXPECT_EQ(allocator.AllocatedBytes(), 8U);
  EXPECT_EQ(buffers.BufferSize(), 8U);
}

TEST(DdsWrapperBounds, RejectsOversizedSequencesAtTypeSupportBoundaries) {
  using eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION;
  using eprosima::fastdds::rtps::SerializedPayload_t;

  const auto& bounded_type = GetSerializedMessageTypeSupport();
  SerializedPayload_t payload(1024);
  aimrt::dds::SerializedMessage message;
  message.metadata().resize(kSerializedMessageMetadataMaxEntries + 1);
  const auto serialize_rejects_before =
      SerializedMessageSerializeBoundRejectCount();
  EXPECT_EQ(bounded_type->calculate_serialized_size(
                &message, XCDR2_DATA_REPRESENTATION),
            0U);
  EXPECT_FALSE(bounded_type->serialize(
      &message, payload, XCDR2_DATA_REPRESENTATION));
  EXPECT_EQ(SerializedMessageSerializeBoundRejectCount(),
            serialize_rejects_before + 1);

  message.metadata().clear();
  message.data().resize(kSerializedMessageDataMaxBytes + 1);
  EXPECT_EQ(bounded_type->calculate_serialized_size(
                &message, XCDR2_DATA_REPRESENTATION),
            0U);
  EXPECT_FALSE(bounded_type->serialize(
      &message, payload, XCDR2_DATA_REPRESENTATION));
  EXPECT_EQ(SerializedMessageSerializeBoundRejectCount(),
            serialize_rejects_before + 2);
}

TEST(DdsWrapperBounds, RejectsGeneratedInvalidWireBeforeVectorMaterialization) {
  using eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION;
  using eprosima::fastdds::rtps::SerializedPayload_t;

  aimrt::dds::SerializedMessagePubSubType generated_type;
  const auto& bounded_type = GetSerializedMessageTypeSupport();
  auto verify_wire_rejection = [&](const aimrt::dds::SerializedMessage& invalid) {
    const auto payload_size = generated_type.calculate_serialized_size(
        &invalid, XCDR2_DATA_REPRESENTATION);
    ASSERT_GT(payload_size, 0U);
    SerializedPayload_t payload(payload_size);
    ASSERT_TRUE(generated_type.serialize(
        &invalid, payload, XCDR2_DATA_REPRESENTATION));

    aimrt::dds::SerializedMessage decoded;
    const auto rejects_before = SerializedMessageDeserializeBoundRejectCount();
    ASSERT_TRUE(bounded_type->deserialize(payload, &decoded));
    EXPECT_EQ(SerializedMessageDeserializeBoundRejectCount(),
              rejects_before + 1);
    EXPECT_EQ(decoded.type_name(), "__aimrt_dds_wire_bound_violation__");
    EXPECT_TRUE(decoded.metadata().empty());
    EXPECT_TRUE(decoded.data().empty());
  };

  aimrt::dds::SerializedMessage excessive_metadata;
  excessive_metadata.metadata().resize(kSerializedMessageMetadataMaxEntries + 1);
  verify_wire_rejection(excessive_metadata);

  aimrt::dds::SerializedMessage excessive_data;
  excessive_data.data().resize(kSerializedMessageDataMaxBytes + 1);
  verify_wire_rejection(excessive_data);

  aimrt::dds::SerializedMessage valid;
  valid.type_name("pb:test.Message");
  valid.serialization_type("pb");
  valid.metadata().resize(1);
  valid.metadata().front().key("trace-id");
  valid.metadata().front().value("42");
  valid.data() = {'o', 'k'};
  const auto valid_size = generated_type.calculate_serialized_size(
      &valid, XCDR2_DATA_REPRESENTATION);
  SerializedPayload_t valid_payload(valid_size);
  ASSERT_TRUE(generated_type.serialize(
      &valid, valid_payload, XCDR2_DATA_REPRESENTATION));
  aimrt::dds::SerializedMessage decoded;
  ASSERT_TRUE(bounded_type->deserialize(valid_payload, &decoded));
  EXPECT_EQ(decoded, valid);
}

TEST(DdsWrapperValidation, RejectsWrongTypeSerializationAndRpcMetadata) {
  EXPECT_FALSE(ValidateSerializedMessageSemantics(
                   "pb:other.Message", "pb", {}, 1,
                   SerializedMessageValidationOptions{
                       .usage = SerializedMessageUsage::kChannel,
                       .expected_type_name = "pb:test.Message",
                       .expected_serialization_type = "pb"})
                   .ok);
  EXPECT_FALSE(ValidateSerializedMessageSemantics(
                   "pb:test.Message", "ros2", {}, 1,
                   SerializedMessageValidationOptions{
                       .usage = SerializedMessageUsage::kChannel,
                       .expected_type_name = "pb:test.Message",
                       .expected_serialization_type = "pb"})
                   .ok);

  const std::array metadata{
      SerializedMetadataView{.key = "trace-id", .value = "123"}};
  EXPECT_FALSE(ValidateSerializedMessageSemantics(
                   "pb:test.Request", "pb", metadata, 1,
                   SerializedMessageValidationOptions{
                       .usage = SerializedMessageUsage::kRpc,
                       .expected_type_name = "pb:test.Request",
                       .expected_serialization_type = "pb"})
                   .ok);
  EXPECT_TRUE(ValidateSerializedMessageSemantics(
                  "ros2:test/msg/Request", "ros2", {}, 1,
                  SerializedMessageValidationOptions{
                      .usage = SerializedMessageUsage::kRpc,
                      .expected_type_name = "ros2:test/msg/Request",
                      .expected_serialization_type = "ros2"})
                  .ok);
}

TEST(DdsWrapperValidation, AdapterSelectionIsExactAndNeverLoanable) {
  EXPECT_EQ(WrapperSerializationType("pb:test.Message"), "pb");
  EXPECT_EQ(WrapperSerializationType("ros2:test/msg/Message"), "ros2");
  EXPECT_TRUE(WrapperSerializationType("dds:test::Message").empty());
  EXPECT_TRUE(WrapperSerializationType("pb").empty());
  EXPECT_FALSE(CanLoanSerializedMessageWrapper());
}

TEST(DdsXtypesWrapperScope, ExposesOnlyTheFixedWrapperSchema) {
#if defined(AIMRT_DDS_ENABLE_XTYPES)
  namespace xtypes = eprosima::fastdds::dds::xtypes;
  xtypes::TypeIdentifierPair identifiers;
  aimrt::dds::register_SerializedMessage_type_identifier(identifiers);
  xtypes::CompleteTypeObject type_object;
  ASSERT_EQ(eprosima::fastdds::dds::DomainParticipantFactory::get_instance()
                ->type_object_registry()
                .get_complete_type_object(identifiers, type_object),
            eprosima::fastdds::dds::RETCODE_OK);

  std::vector<std::string> member_names;
  for (const auto& member : type_object.struct_type().member_seq())
    member_names.emplace_back(member.detail().name());
  EXPECT_EQ(member_names,
            (std::vector<std::string>{"type_name", "serialization_type",
                                      "metadata", "data"}));
  EXPECT_TRUE(std::ranges::none_of(member_names, [](const auto& name) {
    return name.find("business") != std::string::npos ||
           name.find("protobuf") != std::string::npos ||
           name.find("ros2_message") != std::string::npos;
  }));
#else
  GTEST_SKIP() << "XTypes support is disabled in this build";
#endif
}

}  // namespace
}  // namespace aimrt::plugins::dds_plugin
