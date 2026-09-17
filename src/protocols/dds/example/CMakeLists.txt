# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

add_library(aimrt_protocols_dds_example STATIC)
add_library(aimrt::protocols::dds_example ALIAS aimrt_protocols_dds_example)
set_target_properties(aimrt_protocols_dds_example PROPERTIES POSITION_INDEPENDENT_CODE ON)

aimrt_add_dds_idl_codegen(
  TARGET_NAME
  aimrt_protocols_dds_example_codegen
  IDL_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/Benchmark.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/Example.idl
  OUTPUT_DIR
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ATTACH_TO_TARGET
  aimrt_protocols_dds_example)

target_link_libraries(
  aimrt_protocols_dds_example
  PUBLIC aimrt::interface::aimrt_module_dds_interface
         aimrt::deps::fastdds)
