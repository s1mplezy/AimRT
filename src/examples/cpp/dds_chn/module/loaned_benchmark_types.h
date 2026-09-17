// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <cstdint>
#include <utility>

#include "Benchmark.h"

namespace aimrt::examples::cpp::dds_chn {

inline bool IsSupportedLoanedBenchmarkSize(uint32_t size) {
  return size == 1024U || size == 65536U || size == 1048576U ||
         size == 16777216U;
}

template <typename Func>
bool DispatchLoanedBenchmarkType(uint32_t size, Func&& func) {
  switch (size) {
    case 1024U:
      return std::forward<Func>(func)
          .template operator()<aimrt_examples::dds::LoanedBenchmarkMessage1K>();
    case 65536U:
      return std::forward<Func>(func)
          .template operator()<aimrt_examples::dds::LoanedBenchmarkMessage64K>();
    case 1048576U:
      return std::forward<Func>(func)
          .template operator()<aimrt_examples::dds::LoanedBenchmarkMessage1M>();
    case 16777216U:
      return std::forward<Func>(func)
          .template operator()<aimrt_examples::dds::LoanedBenchmarkMessage16M>();
    default:
      return false;
  }
}

}  // namespace aimrt::examples::cpp::dds_chn
