// Copyright (c) 2026 The AimRT Authors.
// AimRT is licensed under Mulan PSL v2.

#include "dds_plugin/serialized_message_adapter.h"

#include <atomic>
#include <stdexcept>

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>

#include "aimrt_module_cpp_interface/util/buffer_array_allocator.h"
#include "detail/serialized_message/serialized_messagePubSubTypes.hpp"

namespace aimrt::plugins::dds_plugin {
namespace {

SerializedMessageValidationResult Failure(std::string reason) {
  return {.ok = false, .reason = std::move(reason)};
}

std::atomic_uint64_t g_serialize_bound_reject_total = 0;
std::atomic_uint64_t g_deserialize_bound_reject_total = 0;

SerializedMessageValidationResult ValidateWireBounds(
    const aimrt::dds::SerializedMessage& message) {
  if (message.type_name().size() > kSerializedMessageTypeNameMaxBytes)
    return Failure("type_name exceeds 256 UTF-8 bytes");
  if (message.serialization_type().size() >
      kSerializedMessageSerializationTypeMaxBytes)
    return Failure("serialization_type exceeds 32 UTF-8 bytes");
  if (message.metadata().size() > kSerializedMessageMetadataMaxEntries)
    return Failure("metadata exceeds 64 entries");
  for (const auto& entry : message.metadata()) {
    if (entry.key().size() > kSerializedMessageKeyMaxBytes)
      return Failure("metadata key exceeds 256 UTF-8 bytes");
    if (entry.value().size() > kSerializedMessageValueMaxBytes)
      return Failure("metadata value exceeds 4096 UTF-8 bytes");
  }
  if (message.data().size() > kSerializedMessageDataMaxBytes)
    return Failure("data exceeds 16 MiB");
  return {.ok = true};
}

class WireBoundViolation : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void SkipBoundedString(eprosima::fastcdr::Cdr& cdr, size_t max_bytes,
                       const char* reason) {
  uint32_t encoded_length = 0;
  cdr.deserialize(encoded_length);
  const size_t string_bytes = encoded_length == 0 ? 0 : encoded_length - 1;
  if (string_bytes > max_bytes) throw WireBoundViolation(reason);
  if (!cdr.jump(encoded_length))
    throw std::runtime_error("truncated bounded string");
}

eprosima::fastcdr::EncodingAlgorithmFlag TypeEncoding(
    const eprosima::fastcdr::Cdr& cdr) {
  return cdr.get_cdr_version() == eprosima::fastcdr::CdrVersion::XCDRv2
             ? eprosima::fastcdr::EncodingAlgorithmFlag::DELIMIT_CDR2
             : eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR;
}

void PreflightMetadataEntry(eprosima::fastcdr::Cdr& cdr) {
  cdr.deserialize_type(
      TypeEncoding(cdr),
      [](eprosima::fastcdr::Cdr& member_cdr,
         const eprosima::fastcdr::MemberId& member_id) {
        switch (member_id.id) {
          case 0:
            SkipBoundedString(member_cdr, kSerializedMessageKeyMaxBytes,
                              "metadata key exceeds 256 UTF-8 bytes");
            return true;
          case 1:
            SkipBoundedString(member_cdr, kSerializedMessageValueMaxBytes,
                              "metadata value exceeds 4096 UTF-8 bytes");
            return true;
          default:
            return false;
        }
      });
}

void PreflightMetadata(eprosima::fastcdr::Cdr& cdr) {
  uint32_t delimited_size = 0;
  char* delimited_begin = nullptr;
  if (cdr.get_cdr_version() == eprosima::fastcdr::CdrVersion::XCDRv2) {
    cdr.deserialize(delimited_size);
    delimited_begin = cdr.get_current_position();
  }

  uint32_t entry_count = 0;
  cdr.deserialize(entry_count);
  if (entry_count > kSerializedMessageMetadataMaxEntries)
    throw WireBoundViolation("metadata exceeds 64 entries");
  for (uint32_t index = 0; index < entry_count; ++index)
    PreflightMetadataEntry(cdr);

  if (delimited_begin != nullptr &&
      static_cast<size_t>(cdr.get_current_position() - delimited_begin) !=
          delimited_size) {
    throw std::runtime_error("metadata DHEADER size mismatch");
  }
}

void PreflightData(eprosima::fastcdr::Cdr& cdr) {
  uint32_t data_size = 0;
  cdr.deserialize(data_size);
  if (data_size > kSerializedMessageDataMaxBytes)
    throw WireBoundViolation("data exceeds 16 MiB");
  if (!cdr.jump(data_size)) throw std::runtime_error("truncated wrapper data");
}

void PreflightSerializedMessage(
    eprosima::fastdds::rtps::SerializedPayload_t& payload) {
  // The generated deserializer resizes std::vector fields from their wire
  // lengths. Inspect the CDR first so no over-bound sequence is materialized.
  eprosima::fastcdr::FastBuffer buffer(
      reinterpret_cast<char*>(payload.data), payload.length);
  eprosima::fastcdr::Cdr cdr(buffer,
                             eprosima::fastcdr::Cdr::DEFAULT_ENDIAN);
  cdr.read_encapsulation();
  payload.encapsulation =
      cdr.endianness() == eprosima::fastcdr::Cdr::BIG_ENDIANNESS ? CDR_BE
                                                                 : CDR_LE;
  cdr.deserialize_type(
      TypeEncoding(cdr),
      [](eprosima::fastcdr::Cdr& member_cdr,
         const eprosima::fastcdr::MemberId& member_id) {
        switch (member_id.id) {
          case 0:
            SkipBoundedString(member_cdr,
                              kSerializedMessageTypeNameMaxBytes,
                              "type_name exceeds 256 UTF-8 bytes");
            return true;
          case 1:
            SkipBoundedString(
                member_cdr, kSerializedMessageSerializationTypeMaxBytes,
                "serialization_type exceeds 32 UTF-8 bytes");
            return true;
          case 2:
            PreflightMetadata(member_cdr);
            return true;
          case 3:
            PreflightData(member_cdr);
            return true;
          default:
            return false;
        }
      });
}

class BoundedSerializedMessagePubSubType final
    : public aimrt::dds::SerializedMessagePubSubType {
 public:
  bool serialize(
      const void* const data,
      eprosima::fastdds::rtps::SerializedPayload_t& payload,
      eprosima::fastdds::dds::DataRepresentationId_t representation) override {
    const auto& message =
        *static_cast<const aimrt::dds::SerializedMessage*>(data);
    if (!ValidateWireBounds(message).ok) {
      ++g_serialize_bound_reject_total;
      return false;
    }
    return aimrt::dds::SerializedMessagePubSubType::serialize(
        data, payload, representation);
  }

  bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& payload,
                   void* data) override {
    try {
      PreflightSerializedMessage(payload);
    } catch (const WireBoundViolation&) {
      ++g_deserialize_bound_reject_total;
      auto& message = *static_cast<aimrt::dds::SerializedMessage*>(data);
      message = {};
      // Return a fixed-size invalid sample so the existing receive path records
      // its stable wrapper diagnostic without retaining attacker-sized state.
      message.type_name("__aimrt_dds_wire_bound_violation__");
      return true;
    } catch (const std::exception&) {
      return false;
    }
    return aimrt::dds::SerializedMessagePubSubType::deserialize(payload, data);
  }

  uint32_t calculate_serialized_size(
      const void* const data,
      eprosima::fastdds::dds::DataRepresentationId_t representation) override {
    const auto& message =
        *static_cast<const aimrt::dds::SerializedMessage*>(data);
    if (!ValidateWireBounds(message).ok) return 0;
    return aimrt::dds::SerializedMessagePubSubType::calculate_serialized_size(
        data, representation);
  }
};

}  // namespace

SerializedMessageValidationResult ValidateSerializedMessageInput(
    std::string_view type_name, std::string_view serialization_type,
    std::span<const SerializedMetadataView> metadata, size_t data_size,
    SerializedMessageUsage usage) {
  if (type_name.size() > kSerializedMessageTypeNameMaxBytes)
    return Failure("type_name exceeds 256 UTF-8 bytes");
  if (serialization_type.size() > kSerializedMessageSerializationTypeMaxBytes)
    return Failure("serialization_type exceeds 32 UTF-8 bytes");
  if (metadata.size() > kSerializedMessageMetadataMaxEntries)
    return Failure("metadata exceeds 64 entries");
  for (const auto& entry : metadata) {
    if (entry.key.size() > kSerializedMessageKeyMaxBytes)
      return Failure("metadata key exceeds 256 UTF-8 bytes");
    if (entry.value.size() > kSerializedMessageValueMaxBytes)
      return Failure("metadata value exceeds 4096 UTF-8 bytes");
  }
  if (data_size > kSerializedMessageDataMaxBytes)
    return Failure("data exceeds 16 MiB");
  if (usage == SerializedMessageUsage::kRpc && !metadata.empty())
    return Failure("RPC wrapper metadata must be empty");
  return {.ok = true};
}

SerializedMessageValidationResult ValidateSerializedMessageSemantics(
    std::string_view type_name, std::string_view serialization_type,
    std::span<const SerializedMetadataView> metadata, size_t data_size,
    const SerializedMessageValidationOptions& options) {
  auto result = ValidateSerializedMessageInput(type_name, serialization_type,
                                               metadata, data_size,
                                               options.usage);
  if (!result.ok) return result;
  if (type_name.empty() || type_name != options.expected_type_name)
    return Failure("wrapper type_name does not match the registered endpoint type");
  if ((serialization_type != "pb" && serialization_type != "ros2") ||
      serialization_type != options.expected_serialization_type)
    return Failure("wrapper serialization_type does not match the registered adapter");
  return {.ok = true};
}

SerializedMessageValidationResult ValidateSerializedMessage(
    const aimrt::dds::SerializedMessage& message,
    const SerializedMessageValidationOptions& options) {
  auto result = ValidateWireBounds(message);
  if (!result.ok) return result;
  if (options.usage == SerializedMessageUsage::kRpc &&
      !message.metadata().empty())
    return Failure("RPC wrapper metadata must be empty");
  const auto type_name =
      std::string_view(message.type_name().c_str(), message.type_name().size());
  const auto serialization_type = std::string_view(
      message.serialization_type().c_str(),
      message.serialization_type().size());
  if (type_name.empty() || type_name != options.expected_type_name)
    return Failure(
        "wrapper type_name does not match the registered endpoint type");
  if ((serialization_type != "pb" && serialization_type != "ros2") ||
      serialization_type != options.expected_serialization_type)
    return Failure(
        "wrapper serialization_type does not match the registered adapter");
  return {.ok = true};
}

uint64_t SerializedMessageSerializeBoundRejectCount() noexcept {
  return g_serialize_bound_reject_total.load();
}

uint64_t SerializedMessageDeserializeBoundRejectCount() noexcept {
  return g_deserialize_bound_reject_total.load();
}

std::string_view WrapperSerializationType(
    std::string_view aimrt_type_name) noexcept {
  if (aimrt_type_name.starts_with("pb:") && aimrt_type_name.size() > 3)
    return "pb";
  if (aimrt_type_name.starts_with("ros2:") && aimrt_type_name.size() > 5)
    return "ros2";
  return {};
}

bool WrapperAdapterEnabled(std::string_view aimrt_type_name) noexcept {
  const auto serialization_type = WrapperSerializationType(aimrt_type_name);
  if (serialization_type == "pb") {
#if defined(AIMRT_BUILD_WITH_PROTOBUF)
    return true;
#else
    return false;
#endif
  }
  if (serialization_type == "ros2") {
#if defined(AIMRT_BUILD_WITH_ROS2)
    return true;
#else
    return false;
#endif
  }
  return false;
}

BoundedBufferArrayAllocator::BoundedBufferArrayAllocator(
    size_t byte_limit) noexcept
    : byte_limit_(byte_limit),
      base_{
          .reserve = [](void* impl, aimrt_buffer_array_t* buffer_array,
                        size_t new_capacity) { return static_cast<BoundedBufferArrayAllocator*>(impl)->Reserve(
                                                   buffer_array, new_capacity); },
          .allocate = [](void* impl, aimrt_buffer_array_t* buffer_array,
                         size_t size) { return static_cast<BoundedBufferArrayAllocator*>(impl)->Allocate(
                                            buffer_array, size); },
          .release = [](void* impl, aimrt_buffer_array_t* buffer_array) { static_cast<BoundedBufferArrayAllocator*>(impl)->Release(
                                                                              buffer_array); },
          .impl = this} {}

bool BoundedBufferArrayAllocator::Reserve(
    aimrt_buffer_array_t* buffer_array, size_t new_capacity) {
  return aimrt::util::SimpleBufferArrayAllocator::Reserve(buffer_array,
                                                          new_capacity);
}

aimrt_buffer_t BoundedBufferArrayAllocator::Allocate(
    aimrt_buffer_array_t* buffer_array, size_t size) {
  if (size > byte_limit_ - allocated_bytes_) {
    limit_exceeded_ = true;
    return {nullptr, 0};
  }
  auto buffer =
      aimrt::util::SimpleBufferArrayAllocator::Allocate(buffer_array, size);
  if (buffer.data != nullptr) allocated_bytes_ += buffer.len;
  return buffer;
}

void BoundedBufferArrayAllocator::Release(
    aimrt_buffer_array_t* buffer_array) noexcept {
  aimrt::util::SimpleBufferArrayAllocator::Release(buffer_array);
  allocated_bytes_ = 0;
}

const eprosima::fastdds::dds::TypeSupport& GetSerializedMessageTypeSupport() {
  static const eprosima::fastdds::dds::TypeSupport kTypeSupport(
      new BoundedSerializedMessagePubSubType());
  return kTypeSupport;
}

}  // namespace aimrt::plugins::dds_plugin
