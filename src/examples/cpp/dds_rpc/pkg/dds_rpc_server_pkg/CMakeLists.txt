# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

add_library(aimrt_examples_cpp_dds_rpc_server_pkg SHARED pkg_main.cc)
add_library(aimrt::examples::cpp::dds_rpc::dds_rpc_server_pkg ALIAS aimrt_examples_cpp_dds_rpc_server_pkg)
target_link_libraries(
  aimrt_examples_cpp_dds_rpc_server_pkg
  PRIVATE aimrt::interface::aimrt_pkg_c_interface
          aimrt::examples::cpp::dds_rpc::normal_rpc_sync_server_module)
set_target_properties(aimrt_examples_cpp_dds_rpc_server_pkg PROPERTIES OUTPUT_NAME dds_rpc_server_pkg)
