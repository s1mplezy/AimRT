# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

# Set file collection
file(GLOB_RECURSE src ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
list(REMOVE_ITEM src ${test_files})

# Add target
add_library(${CUR_TARGET_NAME} SHARED)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Set source file of target
target_sources(${CUR_TARGET_NAME} PRIVATE ${src})

# Set include path of target
target_include_directories(
  ${CUR_TARGET_NAME}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)

# Set link libraries of target
target_link_libraries(
  ${CUR_TARGET_NAME}
  PRIVATE aimrt::interface::aimrt_core_plugin_interface
          aimrt::runtime::core
          iceoryx_posh::iceoryx_posh)

# Add -Werror option
include(AddWerror)
add_werror(${CUR_TARGET_NAME})

# Set installation of target
if(AIMRT_INSTALL)
  install(TARGETS ${CUR_TARGET_NAME} LIBRARY DESTINATION bin)
endif()

# Set test of target
if(AIMRT_BUILD_TESTS AND test_files)
  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${test_files})
  if(UNIX AND TARGET iox-roudi)
    add_dependencies(${CUR_TARGET_NAME}_test iox-roudi)
    target_compile_definitions(${CUR_TARGET_NAME}_test PRIVATE AIMRT_ICEORYX_ROUDI_PATH="$<TARGET_FILE:iox-roudi>")
  endif()
  if(AIMRT_BUILD_WITH_DDS)
    aimrt_add_dds_idl_codegen(
      TARGET_NAME
      aimrt_iceoryx_loan_fixture
      IDL_FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/IceoryxLoanTypes.idl
      OUTPUT_DIR
      ${CMAKE_CURRENT_BINARY_DIR}/loan-generated
      ATTACH_TO_TARGET
      ${CUR_TARGET_NAME}_test)
    target_compile_definitions(${CUR_TARGET_NAME}_test PRIVATE AIMRT_ICEORYX_TEST_WITH_DDS=1)
    target_link_libraries(${CUR_TARGET_NAME}_test PRIVATE aimrt::interface::aimrt_module_dds_interface aimrt::deps::fastdds)
  endif()
  set_tests_properties(${CUR_TARGET_NAME}_test PROPERTIES TIMEOUT 30)
endif()

# Set misc of target
set_target_properties(${CUR_TARGET_NAME} PROPERTIES OUTPUT_NAME "aimrt_${CUR_DIR}")
