// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "Calculator.h"

#include <type_traits>

static_assert(aimrt::DdsMessageType<example::AddRequest>);
static_assert(aimrt::DdsMessageType<example::AddResponse>);
static_assert(std::is_base_of_v<aimrt::rpc::ServiceBase, example::CalculatorSyncService>);
static_assert(std::is_base_of_v<aimrt::rpc::SyncProxyBase, example::CalculatorSyncProxy>);
