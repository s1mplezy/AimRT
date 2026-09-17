// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "aimrt_pkg_c_interface/pkg_macro.h"
#include "benchmark_publisher_module/benchmark_publisher_module.h"
#include "loaned_benchmark_publisher_module/loaned_benchmark_publisher_module.h"
#include "normal_publisher_module/normal_publisher_module.h"

static std::tuple<std::string_view, std::function<aimrt::ModuleBase*()>> aimrt_module_register_array[]{
    {"DdsNormalPublisherModule", []() -> aimrt::ModuleBase* {
       return new aimrt::examples::cpp::dds_chn::normal_publisher_module::NormalPublisherModule();
     }},
    {"BenchmarkPublisherModule", []() -> aimrt::ModuleBase* {
       return new aimrt::examples::cpp::dds_chn::benchmark_publisher_module::BenchmarkPublisherModule();
     }},
    {"LoanedBenchmarkPublisherModule", []() -> aimrt::ModuleBase* {
       return new aimrt::examples::cpp::dds_chn::loaned_benchmark_publisher_module::LoanedBenchmarkPublisherModule();
     }}};

AIMRT_PKG_MAIN(aimrt_module_register_array)
