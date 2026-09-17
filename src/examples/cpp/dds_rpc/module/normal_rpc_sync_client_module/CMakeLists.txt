# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

add_library(aimrt_examples_cpp_dds_rpc_normal_rpc_sync_client_module STATIC normal_rpc_sync_client_module.cc)
add_library(aimrt::examples::cpp::dds_rpc::normal_rpc_sync_client_module ALIAS aimrt_examples_cpp_dds_rpc_normal_rpc_sync_client_module)
target_include_directories(
  aimrt_examples_cpp_dds_rpc_normal_rpc_sync_client_module
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(
  aimrt_examples_cpp_dds_rpc_normal_rpc_sync_client_module
  PRIVATE yaml-cpp::yaml-cpp
  PUBLIC aimrt::interface::aimrt_module_cpp_interface
         aimrt::protocols::dds_example)
