// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <string_view>

#include "aimrt_module_cpp_interface/channel/channel_handle.h"

namespace aimrt::examples::cpp::ros2_chn {

inline std::string_view LoanStatusName(aimrt::channel::LoanStatus status) {
  switch (status) {
    case AIMRT_CHANNEL_LOAN_STATUS_OK:
      return "OK";
    case AIMRT_CHANNEL_LOAN_STATUS_INVALID_BACKEND_COUNT:
      return "INVALID_BACKEND_COUNT";
    case AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_BACKEND:
      return "UNSUPPORTED_BACKEND";
    case AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE:
      return "UNSUPPORTED_MESSAGE_TYPE";
    case AIMRT_CHANNEL_LOAN_STATUS_INCOMPATIBLE_BACKEND_CONFIG:
      return "INCOMPATIBLE_BACKEND_CONFIG";
    case AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN:
      return "RUNTIME_CANNOT_LOAN";
    case AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE:
      return "LOAN_UNAVAILABLE";
    case AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR:
      return "BACKEND_ERROR";
    case AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
    case AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE:
      return "INVALID_STATE";
    case AIMRT_CHANNEL_LOAN_STATUS_UNREGISTERED_MESSAGE_TYPE:
      return "UNREGISTERED_MESSAGE_TYPE";
    default:
      return "UNKNOWN";
  }
}

}  // namespace aimrt::examples::cpp::ros2_chn
