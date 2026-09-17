# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

add_library(aimrt_examples_cpp_dds_chn_pub_pkg SHARED pkg_main.cc)
add_library(aimrt::examples::cpp::dds_chn::dds_chn_pub_pkg ALIAS aimrt_examples_cpp_dds_chn_pub_pkg)
target_link_libraries(
  aimrt_examples_cpp_dds_chn_pub_pkg
  PRIVATE aimrt::interface::aimrt_pkg_c_interface
          aimrt::examples::cpp::dds_chn::normal_publisher_module
          aimrt::examples::cpp::dds_chn::benchmark_publisher_module
          aimrt::examples::cpp::dds_chn::loaned_benchmark_publisher_module)
set_target_properties(aimrt_examples_cpp_dds_chn_pub_pkg PROPERTIES OUTPUT_NAME dds_chn_pub_pkg)
