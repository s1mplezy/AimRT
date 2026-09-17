# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

add_library(aimrt_examples_cpp_dds_chn_benchmark_subscriber_module STATIC benchmark_subscriber_module.cc)
add_library(aimrt::examples::cpp::dds_chn::benchmark_subscriber_module ALIAS aimrt_examples_cpp_dds_chn_benchmark_subscriber_module)
target_include_directories(
  aimrt_examples_cpp_dds_chn_benchmark_subscriber_module
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(
  aimrt_examples_cpp_dds_chn_benchmark_subscriber_module
  PRIVATE yaml-cpp::yaml-cpp
  PUBLIC aimrt::interface::aimrt_module_cpp_interface
         aimrt::interface::aimrt_module_dds_interface
         aimrt::protocols::dds_example)
