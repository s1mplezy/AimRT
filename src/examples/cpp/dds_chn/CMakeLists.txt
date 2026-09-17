# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

set_namespace()

add_subdirectory(module/normal_publisher_module)
add_subdirectory(module/normal_subscriber_module)
add_subdirectory(module/benchmark_publisher_module)
add_subdirectory(module/benchmark_subscriber_module)
add_subdirectory(module/loaned_benchmark_publisher_module)
add_subdirectory(module/loaned_benchmark_subscriber_module)
add_subdirectory(pkg/dds_chn_pub_pkg)
add_subdirectory(pkg/dds_chn_sub_pkg)

add_custom_target(
  aimrt_examples_cpp_dds_chn_build_all ALL
  DEPENDS aimrt::runtime::main
          aimrt::examples::cpp::dds_chn::dds_chn_pub_pkg
          aimrt::examples::cpp::dds_chn::dds_chn_sub_pkg)
