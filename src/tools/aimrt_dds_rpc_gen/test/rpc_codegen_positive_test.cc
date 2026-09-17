// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "Positive.h"

#include <type_traits>

int main() {
  static_assert(aimrt::DdsMessageType<shared::Request>);
  static_assert(std::is_base_of_v<aimrt::rpc::ServiceBase,
                                  dds_codegen_gate::ArithmeticSyncService>);
  dds_codegen_gate::ArithmeticSyncService service;
  const auto* type_support =
      aimrt::GetDdsMessageTypeSupport<shared::Request>();
  auto* register_client = static_cast<bool (*)(aimrt::rpc::RpcHandleRef)>(
      &dds_codegen_gate::RegisterArithmeticClientFunc);
  return service.RpcType() == "dds" && type_support != nullptr &&
                 register_client != nullptr
             ? 0
             : 1;
}
