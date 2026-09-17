# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

set_namespace()

add_subdirectory(module/normal_rpc_sync_client_module)
add_subdirectory(module/normal_rpc_sync_server_module)
add_subdirectory(pkg/dds_rpc_client_pkg)
add_subdirectory(pkg/dds_rpc_server_pkg)

add_custom_target(
  aimrt_examples_cpp_dds_rpc_build_all ALL
  DEPENDS aimrt::runtime::main
          aimrt::examples::cpp::dds_rpc::dds_rpc_client_pkg
          aimrt::examples::cpp::dds_rpc::dds_rpc_server_pkg)
