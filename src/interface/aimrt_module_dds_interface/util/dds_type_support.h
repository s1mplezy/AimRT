// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <concepts>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>

#include "aimrt_module_c_interface/util/type_support_base.h"
#include "aimrt_module_cpp_interface/context/details/type_support.h"
#include "aimrt_module_cpp_interface/util/string.h"

namespace aimrt {

template <typename MsgType>
struct DdsTypeSupportTraits;

template <typename MsgType>
concept DdsMessageType = requires {
                           typename DdsTypeSupportTraits<MsgType>::PubSubType;
                           requires std::derived_from<typename DdsTypeSupportTraits<MsgType>::PubSubType,
                                                      eprosima::fastdds::dds::TopicDataType>;
                           { DdsTypeSupportTraits<MsgType>::TypeName() } -> std::convertible_to<std::string_view>;
                         };

template <DdsMessageType MsgType>
const eprosima::fastdds::dds::TypeSupport& GetStaticDdsTypeSupportHandle() {
  using PubSubType = typename DdsTypeSupportTraits<MsgType>::PubSubType;
  static const eprosima::fastdds::dds::TypeSupport kTypeSupport(new PubSubType());
  return kTypeSupport;
}

template <DdsMessageType MsgType>
bool CanDdsMessageLoanPublish(eprosima::fastdds::dds::DataRepresentationId_t representation) {
  const auto& type_support = GetStaticDdsTypeSupportHandle<MsgType>();
  return type_support && type_support->is_plain(representation);
}

template <DdsMessageType MsgType>
const aimrt_type_support_base_t* GetDdsMessageTypeSupport() {
  static const aimrt_string_view_t kSerializationTypes[] = {
      aimrt::util::ToAimRTStringView("dds_xcdr2")};
  static const std::string kMsgTypeName =
      "dds:" + std::string(DdsTypeSupportTraits<MsgType>::TypeName());

  static const aimrt_native_loan_type_support_t kNativeLoanTypeSupport{
      .size = sizeof(MsgType),
      .alignment = alignof(MsgType),
      .construct = [](void* storage) -> bool {
        if (storage == nullptr) return false;
        try {
          std::construct_at(static_cast<MsgType*>(storage));
          return true;
        } catch (...) {
          return false;
        }
      },
      .destroy = [](void* object) {
        if (object == nullptr) return;
        std::destroy_at(static_cast<MsgType*>(object)); }};

  static const aimrt_type_support_base_t kTypeSupport{
      .type_name = [](void*) -> aimrt_string_view_t {
        return aimrt::util::ToAimRTStringView(kMsgTypeName);
      },
      .create = [](void*) -> void* {
        return new MsgType();
      },
      .destroy = [](void*, void* msg) { delete static_cast<MsgType*>(msg); },
      .copy = [](void*, const void* from, void* to) { *static_cast<MsgType*>(to) = *static_cast<const MsgType*>(from); },
      .move = [](void*, void* from, void* to) { *static_cast<MsgType*>(to) = std::move(*static_cast<MsgType*>(from)); },
      .serialize = [](void*, aimrt_string_view_t serialization_type, const void* msg,
                      const aimrt_buffer_array_allocator_t* allocator,
                      aimrt_buffer_array_t* buffer_array) -> bool {
        if (msg == nullptr || allocator == nullptr || allocator->allocate == nullptr || buffer_array == nullptr ||
            aimrt::util::ToStdStringView(serialization_type) != "dds_xcdr2") {
          return false;
        }
        try {
          const auto& dds_type_support = GetStaticDdsTypeSupportHandle<MsgType>();
          const auto representation = eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION;
          const uint32_t capacity = dds_type_support->calculate_serialized_size(msg, representation);
          eprosima::fastdds::rtps::SerializedPayload_t payload(capacity);
          if (!dds_type_support->serialize(msg, payload, representation)) return false;
          const auto output = allocator->allocate(allocator->impl, buffer_array, payload.length);
          if (output.data == nullptr || output.len < payload.length) return false;
          std::memcpy(output.data, payload.data, payload.length);
          return true;
        } catch (...) {
          return false;
        }
      },
      .deserialize = [](void*, aimrt_string_view_t serialization_type,
                        aimrt_buffer_array_view_t buffer_array_view, void* msg) -> bool {
        if (msg == nullptr || (buffer_array_view.len != 0 && buffer_array_view.data == nullptr) ||
            aimrt::util::ToStdStringView(serialization_type) != "dds_xcdr2") {
          return false;
        }
        try {
          size_t total_size = 0;
          for (size_t index = 0; index < buffer_array_view.len; ++index) {
            if (buffer_array_view.data[index].len >
                std::numeric_limits<uint32_t>::max() - total_size) {
              return false;
            }
            total_size += buffer_array_view.data[index].len;
          }
          eprosima::fastdds::rtps::SerializedPayload_t payload(static_cast<uint32_t>(total_size));
          size_t offset = 0;
          for (size_t index = 0; index < buffer_array_view.len; ++index) {
            const auto& input = buffer_array_view.data[index];
            if (input.len != 0 && input.data == nullptr) return false;
            if (input.len != 0) std::memcpy(payload.data + offset, input.data, input.len);
            offset += input.len;
          }
          payload.length = static_cast<uint32_t>(total_size);
          return GetStaticDdsTypeSupportHandle<MsgType>()->deserialize(payload, msg);
        } catch (...) {
          return false;
        }
      },
      .serialization_types_supported_num = [](void*) -> size_t {
        return std::size(kSerializationTypes);
      },
      .serialization_types_supported_list = [](void*) -> const aimrt_string_view_t* {
        return kSerializationTypes;
      },
      .custom_type_support_ptr = [](void*) -> const void* {
        return &GetStaticDdsTypeSupportHandle<MsgType>();
      },
      .impl = nullptr,
      .native_loan_type_support = [](void*) -> const aimrt_native_loan_type_support_t* {
        return CanDdsMessageLoanPublish<MsgType>(
                   eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION)
                   ? &kNativeLoanTypeSupport
                   : nullptr;
      }};
  return &kTypeSupport;
}

template <DdsMessageType MsgType>
struct MessageTypeSupportTraits<MsgType> {
  static const aimrt_type_support_base_t* Get() {
    return GetDdsMessageTypeSupport<MsgType>();
  }
};

}  // namespace aimrt
