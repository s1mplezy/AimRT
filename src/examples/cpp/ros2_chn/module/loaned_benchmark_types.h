// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <cstdint>
#include <utility>

#include "example_ros2/msg/loaned_benchmark_message16_m.hpp"
#include "example_ros2/msg/loaned_benchmark_message1_k.hpp"
#include "example_ros2/msg/loaned_benchmark_message1_m.hpp"
#include "example_ros2/msg/loaned_benchmark_message64_k.hpp"

namespace aimrt::examples::cpp::ros2_chn {

inline bool IsSupportedLoanedBenchmarkSize(uint32_t size) {
  return size == 1024U || size == 65536U || size == 1048576U ||
         size == 16777216U;
}

template <typename Func>
bool DispatchLoanedBenchmarkType(uint32_t size, Func&& func) {
  switch (size) {
    case 1024U:
      return std::forward<Func>(func)
          .template operator()<example_ros2::msg::LoanedBenchmarkMessage1K>();
    case 65536U:
      return std::forward<Func>(func)
          .template operator()<example_ros2::msg::LoanedBenchmarkMessage64K>();
    case 1048576U:
      return std::forward<Func>(func)
          .template operator()<example_ros2::msg::LoanedBenchmarkMessage1M>();
    case 16777216U:
      return std::forward<Func>(func)
          .template operator()<example_ros2::msg::LoanedBenchmarkMessage16M>();
    default:
      return false;
  }
}

}  // namespace aimrt::examples::cpp::ros2_chn
