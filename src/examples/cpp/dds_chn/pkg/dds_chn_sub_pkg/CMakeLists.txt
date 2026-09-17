# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

add_library(aimrt_examples_cpp_dds_chn_sub_pkg SHARED pkg_main.cc)
add_library(aimrt::examples::cpp::dds_chn::dds_chn_sub_pkg ALIAS aimrt_examples_cpp_dds_chn_sub_pkg)
target_link_libraries(
  aimrt_examples_cpp_dds_chn_sub_pkg
  PRIVATE aimrt::interface::aimrt_pkg_c_interface
          aimrt::examples::cpp::dds_chn::normal_subscriber_module
          aimrt::examples::cpp::dds_chn::benchmark_subscriber_module
          aimrt::examples::cpp::dds_chn::loaned_benchmark_subscriber_module)
set_target_properties(aimrt_examples_cpp_dds_chn_sub_pkg PROPERTIES OUTPUT_NAME dds_chn_sub_pkg)
