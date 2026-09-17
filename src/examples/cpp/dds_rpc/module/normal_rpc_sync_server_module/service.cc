// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "normal_rpc_sync_server_module/service.h"

namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_server_module {

aimrt::rpc::Status CalculatorService::Add(
    aimrt::rpc::ContextRef,
    const aimrt_examples::dds::AddRequest& request,
    aimrt_examples::dds::AddResponse& response) {
  response.sum(request.lhs() + request.rhs());
  return aimrt::rpc::Status();
}

}  // namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_server_module
