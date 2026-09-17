// Copyright (c) 2026 The AimRT Authors.
// AimRT is licensed under Mulan PSL v2.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <fastdds/dds/topic/TypeSupport.hpp>

#include "aimrt_module_c_interface/util/buffer_base.h"
#include "detail/serialized_message/serialized_message.hpp"

namespace aimrt::plugins::dds_plugin {

inline constexpr size_t kSerializedMessageTypeNameMaxBytes = 256;
inline constexpr size_t kSerializedMessageSerializationTypeMaxBytes = 32;
inline constexpr size_t kSerializedMessageMetadataMaxEntries = 64;
inline constexpr size_t kSerializedMessageKeyMaxBytes = 256;
inline constexpr size_t kSerializedMessageValueMaxBytes = 4096;
inline constexpr size_t kSerializedMessageDataMaxBytes = 16 * 1024 * 1024;

enum class SerializedMessageUsage { kChannel,
                                    kRpc };

struct SerializedMetadataView {
  std::string_view key;
  std::string_view value;
};

struct SerializedMessageValidationOptions {
  SerializedMessageUsage usage = SerializedMessageUsage::kChannel;
  std::string_view expected_type_name;
  std::string_view expected_serialization_type;
};

struct SerializedMessageValidationResult {
  bool ok = false;
  std::string reason;
};

SerializedMessageValidationResult ValidateSerializedMessageInput(
    std::string_view type_name, std::string_view serialization_type,
    std::span<const SerializedMetadataView> metadata, size_t data_size,
    SerializedMessageUsage usage);

SerializedMessageValidationResult ValidateSerializedMessageSemantics(
    std::string_view type_name, std::string_view serialization_type,
    std::span<const SerializedMetadataView> metadata, size_t data_size,
    const SerializedMessageValidationOptions& options);

SerializedMessageValidationResult ValidateSerializedMessage(
    const aimrt::dds::SerializedMessage& message,
    const SerializedMessageValidationOptions& options);

std::string_view WrapperSerializationType(std::string_view aimrt_type_name) noexcept;
bool WrapperAdapterEnabled(std::string_view aimrt_type_name) noexcept;
constexpr bool CanLoanSerializedMessageWrapper() noexcept { return false; }

class BoundedBufferArrayAllocator {
 public:
  explicit BoundedBufferArrayAllocator(size_t byte_limit) noexcept;

  const aimrt_buffer_array_allocator_t* NativeHandle() const noexcept {
    return &base_;
  }
  size_t AllocatedBytes() const noexcept { return allocated_bytes_; }
  bool LimitExceeded() const noexcept { return limit_exceeded_; }

 private:
  bool Reserve(aimrt_buffer_array_t* buffer_array, size_t new_capacity);
  aimrt_buffer_t Allocate(aimrt_buffer_array_t* buffer_array, size_t size);
  void Release(aimrt_buffer_array_t* buffer_array) noexcept;

  size_t byte_limit_;
  size_t allocated_bytes_ = 0;
  bool limit_exceeded_ = false;
  aimrt_buffer_array_allocator_t base_;
};

const eprosima::fastdds::dds::TypeSupport& GetSerializedMessageTypeSupport();

uint64_t SerializedMessageSerializeBoundRejectCount() noexcept;
uint64_t SerializedMessageDeserializeBoundRejectCount() noexcept;

}  // namespace aimrt::plugins::dds_plugin
