# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

if(NOT CMAKE_SYSTEM_NAME MATCHES "Linux")
  message(FATAL_ERROR "DDS plugin examples currently support Linux only")
endif()

set(AIMRT_DDS_EXAMPLE_INSTALL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/install/linux/bin)
add_custom_target(
  aimrt_examples_plugins_dds_plugin_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${AIMRT_DDS_EXAMPLE_INSTALL_DIR} ${CMAKE_BINARY_DIR}
  DEPENDS aimrt::runtime::main
          aimrt::plugins::dds_plugin
          aimrt::examples::cpp::dds_chn::dds_chn_pub_pkg
          aimrt::examples::cpp::dds_chn::dds_chn_sub_pkg
          aimrt::examples::cpp::dds_rpc::dds_rpc_client_pkg
          aimrt::examples::cpp::dds_rpc::dds_rpc_server_pkg)

if(AIMRT_BUILD_TESTS)
  add_test(
    NAME aimrt_dds_example_channel_smoke
    COMMAND
      Python3::Interpreter ${PROJECT_SOURCE_DIR}/src/plugins/dds_plugin/test_example_smoke.py --server-script ${CMAKE_BINARY_DIR}/start_examples_plugins_dds_plugin_dds_chn_sub.sh
      --client-script ${CMAKE_BINARY_DIR}/start_examples_plugins_dds_plugin_dds_chn_pub.sh --expect-stream server --expect-text "DDS_CHANNEL_RECEIVED sequence=" --work-dir
      ${CMAKE_CURRENT_BINARY_DIR}/channel-smoke)
  add_test(
    NAME aimrt_dds_example_rpc_smoke
    COMMAND
      Python3::Interpreter ${PROJECT_SOURCE_DIR}/src/plugins/dds_plugin/test_example_smoke.py --server-script
      ${CMAKE_BINARY_DIR}/start_examples_plugins_dds_plugin_dds_rpc_server.sh --client-script ${CMAKE_BINARY_DIR}/start_examples_plugins_dds_plugin_dds_rpc_client.sh
      --expect-stream client --expect-text "DDS_RPC_RESPONSE lhs=" --work-dir ${CMAKE_CURRENT_BINARY_DIR}/rpc-smoke)
  set_tests_properties(aimrt_dds_example_channel_smoke aimrt_dds_example_rpc_smoke PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()
