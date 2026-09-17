# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/aimrt_dds_rpc_gen.in ${CMAKE_CURRENT_BINARY_DIR}/aimrt_dds_rpc_gen @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/aimrt_dds_rpc_gen_install.in ${CMAKE_CURRENT_BINARY_DIR}/aimrt_dds_rpc_gen.install @ONLY)
file(
  CHMOD
  ${CMAKE_CURRENT_BINARY_DIR}/aimrt_dds_rpc_gen
  PERMISSIONS
  OWNER_READ
  OWNER_WRITE
  OWNER_EXECUTE
  GROUP_READ
  GROUP_EXECUTE
  WORLD_READ
  WORLD_EXECUTE)
file(
  CHMOD
  ${CMAKE_CURRENT_BINARY_DIR}/aimrt_dds_rpc_gen.install
  PERMISSIONS
  OWNER_READ
  OWNER_WRITE
  OWNER_EXECUTE
  GROUP_READ
  GROUP_EXECUTE
  WORLD_READ
  WORLD_EXECUTE)

set_property(GLOBAL PROPERTY AIMRT_DDS_RPC_GEN_COMMAND_PROPERTY "${CMAKE_CURRENT_BINARY_DIR}/aimrt_dds_rpc_gen")

if(AIMRT_BUILD_TESTS)
  FetchContent_GetProperties(libunifex)
  FetchContent_GetProperties(fmt)
  FetchContent_GetProperties(googletest)
  FetchContent_GetProperties(asio)
  FetchContent_GetProperties(gflags)
  FetchContent_GetProperties(yaml-cpp)
  FetchContent_GetProperties(tbb)
  FetchContent_GetProperties(backward)
  set(AIMRT_DDS_VARIANT_TEST_COMMON_ARGS
      --cmake
      ${CMAKE_COMMAND}
      --source-dir
      ${PROJECT_SOURCE_DIR}
      --fastddsgen
      ${FASTDDSGEN_EXECUTABLE}
      --fastdds-prefix
      ${AIMRT_FASTDDS_PREFIX}
      --libunifex-source
      ${libunifex_SOURCE_DIR}
      --fmt-source
      ${fmt_SOURCE_DIR}
      --googletest-source
      ${googletest_SOURCE_DIR}
      --asio-source
      ${asio_SOURCE_DIR}
      --gflags-source
      ${gflags_SOURCE_DIR}
      --yaml-cpp-source
      ${yaml-cpp_SOURCE_DIR}
      --tbb-source
      ${tbb_SOURCE_DIR}
      --backward-source
      ${backward_SOURCE_DIR})
  foreach(AIMRT_DDS_DEPENDENCY_PREFIX IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND AIMRT_DDS_VARIANT_TEST_COMMON_ARGS "--dependency-prefix=${AIMRT_DDS_DEPENDENCY_PREFIX}")
  endforeach()

  add_test(NAME aimrt_dds_build_default COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_build_default.py ${AIMRT_DDS_VARIANT_TEST_COMMON_ARGS} --work-dir
                                                ${CMAKE_CURRENT_BINARY_DIR}/build-default-test)
  set_tests_properties(aimrt_dds_build_default PROPERTIES TIMEOUT 120)

  add_test(NAME aimrt_dds_build_disabled COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_build_disabled.py ${AIMRT_DDS_VARIANT_TEST_COMMON_ARGS} --work-dir
                                                 ${CMAKE_CURRENT_BINARY_DIR}/build-disabled-test)
  set_tests_properties(aimrt_dds_build_disabled PROPERTIES TIMEOUT 120)

  add_test(NAME aimrt_dds_build_xtypes_off COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_build_xtypes_off.py ${AIMRT_DDS_VARIANT_TEST_COMMON_ARGS} --work-dir
                                                   ${CMAKE_CURRENT_BINARY_DIR}/build-xtypes-off-test)
  set_tests_properties(aimrt_dds_build_xtypes_off PROPERTIES TIMEOUT 120)

  add_test(NAME aimrt_dds_build_interface_only COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_build_interface_only.py ${AIMRT_DDS_VARIANT_TEST_COMMON_ARGS}
                                                       --work-dir ${CMAKE_CURRENT_BINARY_DIR}/build-interface-only-test)
  set_tests_properties(aimrt_dds_build_interface_only PROPERTIES TIMEOUT 120)

  add_test(NAME aimrt_dds_build_plugin_without_capability_rejected
           COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_build_plugin_without_capability.py ${AIMRT_DDS_VARIANT_TEST_COMMON_ARGS} --work-dir
                   ${CMAKE_CURRENT_BINARY_DIR}/build-plugin-without-capability-test)
  set_tests_properties(aimrt_dds_build_plugin_without_capability_rejected PROPERTIES TIMEOUT 120)

  add_test(NAME aimrt_dds_preseeded_dependency_cache COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_preseeded_dependency_cache.py
                                                             ${AIMRT_DDS_VARIANT_TEST_COMMON_ARGS} --work-dir ${CMAKE_CURRENT_BINARY_DIR}/preseeded-cache-test)
  set_tests_properties(aimrt_dds_preseeded_dependency_cache PROPERTIES TIMEOUT 120)

  add_library(aimrt_dds_rpc_codegen_fixture_lib STATIC test/codegen_consumer.cc)
  target_link_libraries(aimrt_dds_rpc_codegen_fixture_lib PUBLIC aimrt::interface::aimrt_module_dds_interface aimrt::deps::fastdds)
  aimrt_add_dds_idl_codegen(
    TARGET_NAME
    aimrt_dds_rpc_codegen_fixture
    IDL_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test/Calculator.idl
    OUTPUT_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/test-generated
    ATTACH_TO_TARGET
    aimrt_dds_rpc_codegen_fixture_lib)

  add_executable(aimrt_dds_native_type_interface_test test/native_type_interface_test.cc)
  target_link_libraries(aimrt_dds_native_type_interface_test PRIVATE aimrt_dds_rpc_codegen_fixture_lib GTest::gtest GTest::gtest_main)
  add_test(NAME aimrt_dds_native_type_interface COMMAND aimrt_dds_native_type_interface_test)
  set_tests_properties(aimrt_dds_native_type_interface PROPERTIES TIMEOUT 120)

  set(AIMRT_DDS_HEADER_ONLY_TEST_ARGS
      --cmake
      ${CMAKE_COMMAND}
      --fastddsgen
      ${FASTDDSGEN_EXECUTABLE}
      --rpc-generator
      ${CMAKE_CURRENT_BINARY_DIR}/aimrt_dds_rpc_gen
      --build-dir
      ${CMAKE_BINARY_DIR}
      --fastdds-prefix
      ${AIMRT_FASTDDS_PREFIX}
      --work-dir
      ${CMAKE_CURRENT_BINARY_DIR}/header-only-install-consumer-test)
  foreach(AIMRT_DDS_DEPENDENCY_PREFIX IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND AIMRT_DDS_HEADER_ONLY_TEST_ARGS "--dependency-prefix=${AIMRT_DDS_DEPENDENCY_PREFIX}")
  endforeach()
  add_test(NAME aimrt_dds_header_only_api COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_header_only_install_consumer.py ${AIMRT_DDS_HEADER_ONLY_TEST_ARGS})
  set_tests_properties(aimrt_dds_header_only_api PROPERTIES TIMEOUT 120 RESOURCE_LOCK aimrt_dds_primary_build_tree)

  add_executable(aimrt_dds_native_no_optional_adapters_test test/native_no_optional_adapters_test.cc)
  target_link_libraries(aimrt_dds_native_no_optional_adapters_test PRIVATE aimrt_dds_rpc_codegen_fixture_lib)
  add_test(NAME aimrt_dds_native_no_optional_adapters COMMAND aimrt_dds_native_no_optional_adapters_test)
  set_tests_properties(aimrt_dds_native_no_optional_adapters PROPERTIES TIMEOUT 120)

  add_test(NAME aimrt_dds_rpc_codegen_rejections COMMAND Python3::Interpreter -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/test -p "test_*.py")
  set_tests_properties(aimrt_dds_rpc_codegen_rejections PROPERTIES ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

  add_test(NAME aimrt_dds_rpc_codegen_differential_rejections
           COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_generic_differential.py --fastddsgen ${FASTDDSGEN_EXECUTABLE} --rpc-generator
                   ${CMAKE_CURRENT_BINARY_DIR}/aimrt_dds_rpc_gen --work-dir ${CMAKE_CURRENT_BINARY_DIR}/generic-differential-test)
  set_tests_properties(aimrt_dds_rpc_codegen_differential_rejections PROPERTIES TIMEOUT 30)

  add_executable(aimrt_dds_rpc_codegen_positive_test test/rpc_codegen_positive_test.cc)
  target_link_libraries(aimrt_dds_rpc_codegen_positive_test PRIVATE aimrt::interface::aimrt_module_dds_interface aimrt::deps::fastdds)
  aimrt_add_dds_idl_codegen(
    TARGET_NAME
    aimrt_dds_rpc_codegen_positive_fixture
    IDL_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test/Positive.idl
    INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/test
    OUTPUT_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/positive-generated
    ATTACH_TO_TARGET
    aimrt_dds_rpc_codegen_positive_test)
  add_test(NAME aimrt_dds_rpc_codegen_positive COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_codegen_positive.py --cmake ${CMAKE_COMMAND} --build-dir
                                                       ${CMAKE_BINARY_DIR})
  set_tests_properties(aimrt_dds_rpc_codegen_positive PROPERTIES TIMEOUT 120 RESOURCE_LOCK aimrt_dds_primary_build_tree)

  add_test(
    NAME aimrt_dds_codegen_incremental
    COMMAND
      Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_codegen_incremental.py --cmake ${CMAKE_COMMAND} --module-dir ${PROJECT_SOURCE_DIR}/cmake --work-dir
      ${CMAKE_CURRENT_BINARY_DIR}/incremental-test --fastddsgen ${FASTDDSGEN_EXECUTABLE} --rpc-generator ${CMAKE_CURRENT_BINARY_DIR}/aimrt_dds_rpc_gen --xtypes
      ${AIMRT_DDS_ENABLE_XTYPES} --aimrt-source ${PROJECT_SOURCE_DIR} --fastdds-prefix ${AIMRT_FASTDDS_PREFIX} --libunifex-source ${libunifex_SOURCE_DIR}
      "--dependency-prefix=${CMAKE_PREFIX_PATH}")
  set_tests_properties(aimrt_dds_codegen_incremental PROPERTIES TIMEOUT 120)

  add_test(NAME aimrt_dds_codegen_tool_lock COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_codegen_tool_lock.py --cmake ${CMAKE_COMMAND} --module-dir
                                                    ${PROJECT_SOURCE_DIR}/cmake --java ${Java_JAVA_EXECUTABLE} --work-dir ${CMAKE_CURRENT_BINARY_DIR}/tool-lock-test)
  set_tests_properties(aimrt_dds_codegen_tool_lock PROPERTIES TIMEOUT 120)

  add_test(
    NAME aimrt_dds_rpc_codegen_install_consumer
    COMMAND
      Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/test_codegen_install_consumer.py --cmake ${CMAKE_COMMAND} --build-dir ${CMAKE_BINARY_DIR} --fastddsgen
      ${FASTDDSGEN_EXECUTABLE} --fastdds-prefix ${AIMRT_FASTDDS_PREFIX} "--dependency-prefix=${CMAKE_PREFIX_PATH}" --work-dir ${CMAKE_CURRENT_BINARY_DIR}/install-consumer-test)
  set_tests_properties(aimrt_dds_rpc_codegen_install_consumer PROPERTIES TIMEOUT 120 RESOURCE_LOCK aimrt_dds_primary_build_tree)
endif()

if(AIMRT_INSTALL)
  install(
    PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/aimrt_dds_rpc_gen.install
    DESTINATION bin
    RENAME aimrt_dds_rpc_gen
    COMPONENT aimrt_dds_codegen)
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/aimrt_dds_rpc_gen
    DESTINATION lib/aimrt/python
    COMPONENT aimrt_dds_codegen
    PATTERN "__pycache__" EXCLUDE)
endif()
