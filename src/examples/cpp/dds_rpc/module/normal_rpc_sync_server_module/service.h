// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include "Example.h"

namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_server_module {

class CalculatorService final : public aimrt_examples::dds::CalculatorSyncService {
 public:
  aimrt::rpc::Status Add(
      aimrt::rpc::ContextRef context,
      const aimrt_examples::dds::AddRequest& request,
      aimrt_examples::dds::AddResponse& response) override;
};

}  // namespace aimrt::examples::cpp::dds_rpc::normal_rpc_sync_server_module
