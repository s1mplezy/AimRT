// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include "aimrt_module_cpp_interface/logger/logger.h"

namespace aimrt::plugins::dds_plugin {

void SetLogger(aimrt::logger::LoggerRef logger);
aimrt::logger::LoggerRef GetLogger();

}  // namespace aimrt::plugins::dds_plugin
