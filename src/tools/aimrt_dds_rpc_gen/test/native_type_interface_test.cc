// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "Calculator.h"

#include <string>

#include <gtest/gtest.h>
#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "aimrt_module_cpp_interface/util/buffer_array_allocator.h"
#include "aimrt_module_cpp_interface/util/type_support.h"

namespace {

struct RegisteredFunction {
  std::string name;
  const void* function_type_support = nullptr;
  const aimrt_type_support_base_t* request_type_support = nullptr;
  const aimrt_type_support_base_t* response_type_support = nullptr;
};

struct FakeRpcHandle {
  RegisteredFunction service;
  RegisteredFunction client;
  aimrt_rpc_handle_base_t native{
      .register_service_func = [](void* impl, aimrt_string_view_t name,
                                  const void* custom_type_support,
                                  const aimrt_type_support_base_t* request_type_support,
                                  const aimrt_type_support_base_t* response_type_support,
                                  aimrt_function_base_t*) -> bool {
        auto& registration = static_cast<FakeRpcHandle*>(impl)->service;
        registration = {
            .name = std::string(aimrt::util::ToStdStringView(name)),
            .function_type_support = custom_type_support,
            .request_type_support = request_type_support,
            .response_type_support = response_type_support};
        return true;
      },
      .register_client_func = [](void* impl, aimrt_string_view_t name,
                                 const void* custom_type_support,
                                 const aimrt_type_support_base_t* request_type_support,
                                 const aimrt_type_support_base_t* response_type_support) -> bool {
        auto& registration = static_cast<FakeRpcHandle*>(impl)->client;
        registration = {
            .name = std::string(aimrt::util::ToStdStringView(name)),
            .function_type_support = custom_type_support,
            .request_type_support = request_type_support,
            .response_type_support = response_type_support};
        return true;
      },
      .invoke = nullptr,
      .merge_server_context_to_client_context = nullptr,
      .impl = this};
};

TEST(DdsTypeSupportTest, ExposesStableNamesHandleAndXcdr2Serialization) {
  const auto* native = aimrt::GetDdsMessageTypeSupport<example::AddRequest>();
  aimrt::util::TypeSupportRef type_support(native);

  EXPECT_EQ(type_support.TypeName(), "dds:example::AddRequest");
  EXPECT_EQ(type_support.DefaultSerializationType(), "dds_xcdr2");
  EXPECT_TRUE(type_support.CheckSerializationTypeSupported("dds_xcdr2"));
  EXPECT_FALSE(type_support.CheckSerializationTypeSupported("pb"));

  const auto* static_handle = static_cast<const eprosima::fastdds::dds::TypeSupport*>(
      type_support.CustomTypeSupportPtr());
  ASSERT_NE(static_handle, nullptr);
  EXPECT_EQ(static_handle, &aimrt::GetStaticDdsTypeSupportHandle<example::AddRequest>());
  const eprosima::fastdds::dds::TypeSupport copied_handle = *static_handle;
  ASSERT_TRUE(copied_handle);
  EXPECT_EQ(copied_handle->get_name(), "example::AddRequest");

  example::AddRequest source;
  source.lhs(41);
  source.rhs(1);
  auto* copied = static_cast<example::AddRequest*>(type_support.Create());
  ASSERT_NE(copied, nullptr);
  type_support.Copy(&source, copied);
  EXPECT_EQ(*copied, source);

  example::AddRequest moved;
  type_support.Move(copied, &moved);
  EXPECT_EQ(moved, source);
  type_support.Destroy(copied);

  aimrt_buffer_array_t buffers{};
  const auto* allocator = aimrt::util::SimpleBufferArrayAllocator::NativeHandle();
  ASSERT_TRUE(type_support.Serialize("dds_xcdr2", &source, allocator, &buffers));
  ASSERT_EQ(buffers.len, 1u);
  ASSERT_GT(buffers.data[0].len, 0u);

  const aimrt_buffer_view_t view{buffers.data[0].data, buffers.data[0].len};
  const aimrt_buffer_array_view_t views{&view, 1};
  example::AddRequest roundtrip;
  EXPECT_TRUE(type_support.Deserialize("dds_xcdr2", views, &roundtrip));
  EXPECT_EQ(roundtrip, source);
  EXPECT_FALSE(type_support.Deserialize("pb", views, &roundtrip));
  allocator->release(allocator->impl, &buffers);

  EXPECT_EQ(
      aimrt::CanDdsMessageLoanPublish<example::AddRequest>(
          eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION),
      static_handle->get()->is_plain(eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION));
}

TEST(DdsTypeSupportTest, GeneratedRpcUsesNullFunctionSupportAndMessageLevelHandles) {
  FakeRpcHandle handle;
  aimrt::rpc::RpcHandleRef handle_ref(&handle.native);

  example::CalculatorSyncService service;
  ASSERT_TRUE(handle_ref.RegisterService(&service));
  EXPECT_EQ(handle.service.name, "dds:/example::Calculator/Add");
  EXPECT_EQ(handle.service.function_type_support, nullptr);
  EXPECT_EQ(handle.service.request_type_support, aimrt::GetDdsMessageTypeSupport<example::AddRequest>());
  EXPECT_EQ(handle.service.response_type_support, aimrt::GetDdsMessageTypeSupport<example::AddResponse>());

  ASSERT_TRUE(example::RegisterCalculatorClientFunc(handle_ref));
  EXPECT_EQ(handle.client.name, "dds:/example::Calculator/Add");
  EXPECT_EQ(handle.client.function_type_support, nullptr);
  EXPECT_EQ(handle.client.request_type_support, aimrt::GetDdsMessageTypeSupport<example::AddRequest>());
  EXPECT_EQ(handle.client.response_type_support, aimrt::GetDdsMessageTypeSupport<example::AddResponse>());
}

}  // namespace
