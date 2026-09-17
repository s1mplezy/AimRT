# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

string(REGEX REPLACE ".*/(.*)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

find_package(tinyxml2 REQUIRED CONFIG)

add_library(aimrt_dds_serialized_message_type STATIC)
aimrt_add_dds_idl_codegen(
  TARGET_NAME
  aimrt_dds_serialized_message_codegen
  IDL_FILES
  ${PROJECT_SOURCE_DIR}/src/interface/aimrt_module_dds_interface/serialized_message.idl
  OUTPUT_DIR
  ${CMAKE_CURRENT_BINARY_DIR}/serialized-message-generated
  ATTACH_TO_TARGET
  aimrt_dds_serialized_message_type)
set_target_properties(aimrt_dds_serialized_message_type PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(
  aimrt_dds_serialized_message_type
  PUBLIC aimrt::interface::aimrt_module_dds_interface
         aimrt::deps::fastdds)

add_library(aimrt_dds_serialized_message_adapter STATIC ${CMAKE_CURRENT_SOURCE_DIR}/serialized_message_adapter.cc)
set_target_properties(aimrt_dds_serialized_message_adapter PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(
  aimrt_dds_serialized_message_adapter
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(
  aimrt_dds_serialized_message_adapter
  PUBLIC aimrt_dds_serialized_message_type
         aimrt::deps::fastdds)
if(AIMRT_BUILD_WITH_PROTOBUF)
  target_compile_definitions(aimrt_dds_serialized_message_adapter PRIVATE AIMRT_BUILD_WITH_PROTOBUF)
endif()
if(AIMRT_BUILD_WITH_ROS2)
  target_compile_definitions(aimrt_dds_serialized_message_adapter PRIVATE AIMRT_BUILD_WITH_ROS2)
endif()

file(GLOB_RECURSE src ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
list(REMOVE_ITEM src ${test_files})
list(REMOVE_ITEM src ${CMAKE_CURRENT_SOURCE_DIR}/serialized_message_adapter.cc)

add_library(${CUR_TARGET_NAME} SHARED ${src})
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})
target_include_directories(
  ${CUR_TARGET_NAME}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(
  ${CUR_TARGET_NAME}
  PRIVATE aimrt::interface::aimrt_core_plugin_interface
          aimrt::runtime::core
          aimrt_dds_serialized_message_adapter
          aimrt::deps::fastdds
          tinyxml2::tinyxml2)
if(AIMRT_FASTDDS_LINK_MODE STREQUAL "static_hidden" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(${CUR_TARGET_NAME} PRIVATE "LINKER:--version-script=${PROJECT_SOURCE_DIR}/cmake/FastDdsStaticHidden.map" "LINKER:--exclude-libs,ALL")
  set_property(
    TARGET ${CUR_TARGET_NAME}
    APPEND
    PROPERTY LINK_DEPENDS "${PROJECT_SOURCE_DIR}/cmake/FastDdsStaticHidden.map")
endif()

if(AIMRT_BUILD_WITH_PROTOBUF)
  target_compile_definitions(${CUR_TARGET_NAME} PRIVATE AIMRT_BUILD_WITH_PROTOBUF)
endif()
if(AIMRT_BUILD_WITH_ROS2)
  target_compile_definitions(${CUR_TARGET_NAME} PRIVATE AIMRT_BUILD_WITH_ROS2)
endif()

include(AddWerror)
add_werror(${CUR_TARGET_NAME})

if(AIMRT_INSTALL)
  install(
    TARGETS ${CUR_TARGET_NAME}
    LIBRARY DESTINATION bin
    RUNTIME DESTINATION bin)
endif()
set_target_properties(${CUR_TARGET_NAME} PROPERTIES OUTPUT_NAME "aimrt_${CUR_DIR}")

if(AIMRT_BUILD_TESTS)
  add_executable(aimrt_dds_serialized_message_test ${CMAKE_CURRENT_SOURCE_DIR}/serialized_message_adapter_test.cc)
  target_include_directories(aimrt_dds_serialized_message_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(aimrt_dds_serialized_message_test PRIVATE aimrt_dds_serialized_message_adapter aimrt::deps::fastdds GTest::gtest GTest::gtest_main)
  if(AIMRT_BUILD_WITH_PROTOBUF)
    target_compile_definitions(aimrt_dds_serialized_message_test PRIVATE AIMRT_BUILD_WITH_PROTOBUF)
  endif()
  if(AIMRT_BUILD_WITH_ROS2)
    target_compile_definitions(aimrt_dds_serialized_message_test PRIVATE AIMRT_BUILD_WITH_ROS2)
  endif()
  if(AIMRT_DDS_ENABLE_XTYPES)
    target_compile_definitions(aimrt_dds_serialized_message_test PRIVATE AIMRT_DDS_ENABLE_XTYPES)
  endif()
  add_test(NAME aimrt_dds_wrapper_bounds COMMAND aimrt_dds_serialized_message_test --gtest_filter=DdsWrapperBounds.*)
  add_test(NAME aimrt_dds_wrapper_validation_unit COMMAND aimrt_dds_serialized_message_test --gtest_filter=DdsWrapperValidation.*)
  add_test(NAME aimrt_dds_xtypes_wrapper_scope COMMAND aimrt_dds_serialized_message_test --gtest_filter=DdsXtypesWrapperScope.*)

  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/dds_config_test.cc)
  add_dependencies(${CUR_TARGET_NAME}_test ${CUR_TARGET_NAME})
  set_tests_properties(${CUR_TARGET_NAME}_test PROPERTIES DISABLED TRUE)
  add_test(NAME aimrt_dds_config_defaults COMMAND $<TARGET_FILE:${CUR_TARGET_NAME}_test> --gtest_filter=DdsConfigDefaults.*)
  add_test(NAME aimrt_dds_config_xml_defaults COMMAND $<TARGET_FILE:${CUR_TARGET_NAME}_test> --gtest_filter=DdsConfigXmlDefaults.*)
  add_test(NAME aimrt_dds_config_xml_invalid COMMAND $<TARGET_FILE:${CUR_TARGET_NAME}_test> --gtest_filter=DdsConfigXmlInvalid.*)
  add_test(NAME aimrt_dds_config_name_validation COMMAND $<TARGET_FILE:${CUR_TARGET_NAME}_test> --gtest_filter=DdsConfigNameValidation.*)
  add_test(NAME aimrt_dds_config_backend_and_fixed_topics COMMAND $<TARGET_FILE:${CUR_TARGET_NAME}_test> --gtest_filter=DdsConfigBackendAndFixedTopics.*)
  add_test(NAME aimrt_dds_config_process_uniqueness COMMAND $<TARGET_FILE:${CUR_TARGET_NAME}_test> --gtest_filter=DdsConfigProcessUniqueness.*)
  add_test(NAME aimrt_dds_rpc_qos_contract COMMAND $<TARGET_FILE:${CUR_TARGET_NAME}_test> --gtest_filter=DdsRpcQosContract.*)
  add_test(NAME aimrt_dds_lifecycle_core_cycles COMMAND $<TARGET_FILE:${CUR_TARGET_NAME}_test> --gtest_filter=DdsLifecycleCycles.*)
  set_tests_properties(aimrt_dds_lifecycle_core_cycles PROPERTIES ENVIRONMENT "AIMRT_DDS_PLUGIN_PATH=$<TARGET_FILE:${CUR_TARGET_NAME}>")

  add_executable(aimrt_dds_rpc_type_contract_test ${CMAKE_CURRENT_SOURCE_DIR}/dds_rpc_type_contract_test.cc ${CMAKE_CURRENT_SOURCE_DIR}/dds_backend.cc
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/dds_config.cc ${CMAKE_CURRENT_SOURCE_DIR}/dds_runtime.cc ${CMAKE_CURRENT_SOURCE_DIR}/global.cc)
  target_include_directories(aimrt_dds_rpc_type_contract_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PROJECT_SOURCE_DIR}/src/runtime)
  target_link_libraries(
    aimrt_dds_rpc_type_contract_test
    PRIVATE aimrt_dds_rpc_codegen_fixture_lib
            aimrt_dds_serialized_message_adapter
            aimrt::interface::aimrt_core_plugin_interface
            aimrt::runtime::core
            aimrt::deps::fastdds
            tinyxml2::tinyxml2
            GTest::gtest
            GTest::gtest_main)
  add_test(NAME aimrt_dds_rpc_type_contract COMMAND aimrt_dds_rpc_type_contract_test)
  add_test(NAME aimrt_dds_core_backend_integration COMMAND aimrt_dds_rpc_type_contract_test --gtest_filter=DdsCoreBackendIntegration.*)
  set_tests_properties(aimrt_dds_rpc_type_contract aimrt_dds_core_backend_integration PROPERTIES TIMEOUT 120 ENVIRONMENT "AIMRT_DDS_PLUGIN_PATH=$<TARGET_FILE:${CUR_TARGET_NAME}>")

  add_executable(aimrt_dds_rpc_test ${CMAKE_CURRENT_SOURCE_DIR}/dds_rpc_test.cc ${CMAKE_CURRENT_SOURCE_DIR}/dds_backend.cc ${CMAKE_CURRENT_SOURCE_DIR}/dds_config.cc
                                    ${CMAKE_CURRENT_SOURCE_DIR}/dds_runtime.cc ${CMAKE_CURRENT_SOURCE_DIR}/global.cc)
  if(AIMRT_BUILD_WITH_PROTOBUF)
    add_protobuf_gencode_target_for_one_proto_file(
      TARGET_NAME aimrt_dds_rpc_pb_fixture
      PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/DdsRpcWrapper.proto
      GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/rpc-pb-generated)
  endif()
  target_include_directories(aimrt_dds_rpc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PROJECT_SOURCE_DIR}/src/runtime)
  target_compile_definitions(aimrt_dds_rpc_test PRIVATE BUILD_TESTING=1)
  target_link_libraries(
    aimrt_dds_rpc_test
    PRIVATE aimrt_dds_rpc_codegen_fixture_lib
            aimrt_dds_serialized_message_adapter
            aimrt::interface::aimrt_core_plugin_interface
            aimrt::runtime::core
            aimrt::deps::fastdds
            tinyxml2::tinyxml2
            GTest::gtest)
  if(AIMRT_FASTDDS_LINK_MODE STREQUAL "static_hidden" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(aimrt_dds_rpc_test PRIVATE "LINKER:--version-script=${PROJECT_SOURCE_DIR}/cmake/FastDdsStaticHidden.map" "LINKER:--exclude-libs,ALL")
    set_property(
      TARGET aimrt_dds_rpc_test
      APPEND
      PROPERTY LINK_DEPENDS "${PROJECT_SOURCE_DIR}/cmake/FastDdsStaticHidden.map")
  endif()
  if(AIMRT_BUILD_WITH_PROTOBUF)
    target_compile_definitions(aimrt_dds_rpc_test PRIVATE AIMRT_BUILD_WITH_PROTOBUF)
    target_link_libraries(aimrt_dds_rpc_test PRIVATE aimrt_dds_rpc_pb_fixture aimrt::interface::aimrt_module_protobuf_interface)
  endif()
  if(AIMRT_BUILD_WITH_ROS2)
    find_package(std_msgs REQUIRED)
    target_compile_definitions(aimrt_dds_rpc_test PRIVATE AIMRT_BUILD_WITH_ROS2)
    target_link_libraries(aimrt_dds_rpc_test PRIVATE aimrt::interface::aimrt_module_ros2_interface std_msgs::std_msgs__rosidl_typesupport_cpp)
  endif()
  add_test(NAME aimrt_dds_rpc_roundtrip COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcRoundtrip.*)
  add_test(NAME aimrt_dds_rpc_correlation COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcCorrelation.*)
  add_test(NAME aimrt_dds_rpc_concurrent_correlation
           COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcCorrelation.ConcurrentCallsCompleteWithTheirOwnResponses:DdsRpcCorrelation.ReplyWaitsForPublicationGateWithoutTaking)
  add_test(NAME aimrt_dds_rpc_multi_client
           COMMAND aimrt_dds_rpc_test
                   --gtest_filter=DdsRpcCorrelation.ForeignClientReplyTokenCannotCompleteAnotherClient:DdsRpcEndpointOwnership.*:DdsRpcMultiProcess.*:DdsProcessGroupHelper.*)
  if(AIMRT_BUILD_WITH_PROTOBUF OR AIMRT_BUILD_WITH_ROS2)
    add_test(NAME aimrt_dds_rpc_wrapper_roundtrip COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcWrapperRoundtrip.*)
  endif()
  add_test(NAME aimrt_dds_rpc_timeout COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcTimeout.*)
  add_test(NAME aimrt_dds_rpc_timeout_no_retry COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcTimeout.*)
  add_test(NAME aimrt_dds_rpc_multiple_servers COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcMultipleServers.*)
  add_test(NAME aimrt_dds_rpc_response_anomalies COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcResponseAnomalies.*)
  add_test(NAME aimrt_dds_rpc_pending_limit COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcPendingLimit.*)
  add_test(NAME aimrt_dds_rpc_server_non_ok COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcServerNonOk.*)
  add_test(NAME aimrt_dds_rpc_shutdown COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcShutdown.*)
  add_test(NAME aimrt_dds_rpc_take_atomicity COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcTakeAtomicity.*)
  add_test(NAME aimrt_dds_rpc_bad_request COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcBadRequest.*)
  if(AIMRT_BUILD_WITH_PROTOBUF)
    add_test(NAME aimrt_dds_rpc_bad_response COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcBadResponse.*)
  endif()
  add_test(NAME aimrt_dds_rpc_send_failure COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcSendFailure.*)
  add_test(NAME aimrt_dds_rpc_reply_match_wait COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcReplyMatchWait.*)
  add_test(NAME aimrt_dds_rpc_real_discovery COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcRealDiscovery.*)
  add_test(NAME aimrt_dds_rpc_readiness COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcReadiness.*)
  add_test(NAME aimrt_dds_rpc_not_ready COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcReadiness.*)
  add_test(NAME aimrt_dds_rpc_identity COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcIdentity.*)
  add_test(NAME aimrt_dds_rpc_terminalization COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcTerminalization.*)
  add_test(NAME aimrt_dds_rpc_reentrancy COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcReentrancy.*)
  add_test(NAME aimrt_dds_rpc_endpoint_ownership COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcEndpointOwnership.*)
  add_test(NAME aimrt_dds_rpc_endpoint_rollback COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcEndpointRollback.*)
  add_test(NAME aimrt_dds_rpc_lifecycle_race COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcLifecycleRace.*)
  add_test(NAME aimrt_dds_rpc_handler_concurrency COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcHandlerConcurrency.*)
  add_test(NAME aimrt_dds_rpc_requester_disconnect COMMAND aimrt_dds_rpc_test --gtest_filter=DdsRpcRequesterDisconnect.*)
  add_test(
    NAME aimrt_dds_rpc_fastdds_behavior_parity
    COMMAND
      aimrt_dds_rpc_test
      --gtest_filter=DdsRpcIdentity.*:DdsRpcCorrelation.ForeignClientReplyTokenCannotCompleteAnotherClient:DdsRpcReplyMatchWait.*:DdsRpcRealDiscovery.*:DdsRpcRequesterDisconnect.*:DdsRpcEndpointOwnership.*:DdsRpcEndpointRollback.*
  )
  add_test(NAME aimrt_dds_generated_rpc_roundtrip COMMAND aimrt_dds_rpc_type_contract_test --gtest_filter=DdsCoreBackendIntegration.*)
  set_tests_properties(
    aimrt_dds_rpc_roundtrip
    aimrt_dds_rpc_correlation
    aimrt_dds_rpc_concurrent_correlation
    aimrt_dds_rpc_multi_client
    aimrt_dds_rpc_timeout
    aimrt_dds_rpc_timeout_no_retry
    aimrt_dds_rpc_multiple_servers
    aimrt_dds_rpc_response_anomalies
    aimrt_dds_rpc_pending_limit
    aimrt_dds_rpc_server_non_ok
    aimrt_dds_rpc_shutdown
    aimrt_dds_rpc_take_atomicity
    aimrt_dds_rpc_bad_request
    aimrt_dds_rpc_send_failure
    aimrt_dds_rpc_reply_match_wait
    aimrt_dds_rpc_real_discovery
    aimrt_dds_rpc_readiness
    aimrt_dds_rpc_not_ready
    aimrt_dds_rpc_identity
    aimrt_dds_rpc_terminalization
    aimrt_dds_rpc_reentrancy
    aimrt_dds_rpc_endpoint_ownership
    aimrt_dds_rpc_endpoint_rollback
    aimrt_dds_rpc_lifecycle_race
    aimrt_dds_rpc_handler_concurrency
    aimrt_dds_rpc_requester_disconnect
    aimrt_dds_rpc_fastdds_behavior_parity
    PROPERTIES TIMEOUT 30)
  if(AIMRT_BUILD_WITH_PROTOBUF)
    set_tests_properties(aimrt_dds_rpc_bad_response PROPERTIES TIMEOUT 30)
  endif()
  set_tests_properties(aimrt_dds_generated_rpc_roundtrip PROPERTIES TIMEOUT 120 ENVIRONMENT "AIMRT_DDS_PLUGIN_PATH=$<TARGET_FILE:${CUR_TARGET_NAME}>")
  set_tests_properties(aimrt_dds_rpc_fastdds_behavior_parity PROPERTIES TIMEOUT 60)
  if(AIMRT_BUILD_WITH_PROTOBUF OR AIMRT_BUILD_WITH_ROS2)
    set_tests_properties(aimrt_dds_rpc_wrapper_roundtrip PROPERTIES TIMEOUT 30)
  endif()

  add_executable(aimrt_dds_runtime_test ${CMAKE_CURRENT_SOURCE_DIR}/dds_runtime_test.cc ${CMAKE_CURRENT_SOURCE_DIR}/dds_config.cc ${CMAKE_CURRENT_SOURCE_DIR}/dds_runtime.cc
                                        ${CMAKE_CURRENT_SOURCE_DIR}/global.cc)
  target_include_directories(aimrt_dds_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PROJECT_SOURCE_DIR}/src/runtime)
  target_compile_definitions(aimrt_dds_runtime_test PRIVATE BUILD_TESTING=1)
  target_link_libraries(
    aimrt_dds_runtime_test
    PRIVATE aimrt_dds_rpc_codegen_fixture_lib
            aimrt::interface::aimrt_core_plugin_interface
            aimrt::runtime::core
            aimrt::deps::fastdds
            tinyxml2::tinyxml2
            GTest::gtest
            GTest::gtest_main)

  add_test(NAME aimrt_dds_config_shared_qos_conflict COMMAND aimrt_dds_runtime_test --gtest_filter=DdsConfigSharedQosConflict.*)
  add_test(NAME aimrt_dds_config_topic_collisions COMMAND aimrt_dds_runtime_test --gtest_filter=DdsConfigTopicCollisions.*)
  add_test(NAME aimrt_dds_executor_asio_auto COMMAND aimrt_dds_runtime_test --gtest_filter=DdsExecutorAsioAuto.*)
  add_test(NAME aimrt_dds_executor_explicit COMMAND aimrt_dds_runtime_test --gtest_filter=DdsExecutorExplicit.*)
  add_test(NAME aimrt_dds_executor_invalid_type COMMAND aimrt_dds_runtime_test --gtest_filter=DdsExecutorInvalidType.*)
  add_test(NAME aimrt_dds_listener_matches COMMAND aimrt_dds_runtime_test --gtest_filter=DdsListenerMatches.*)
  add_test(NAME aimrt_dds_listener_incompatible_qos COMMAND aimrt_dds_runtime_test --gtest_filter=DdsListenerIncompatibleQos.*)
  add_test(NAME aimrt_dds_listener_entity_deletion COMMAND aimrt_dds_runtime_test --gtest_filter=DdsListenerEntityDeletion.*)
  add_test(NAME aimrt_dds_lifecycle_cycles COMMAND aimrt_dds_runtime_test --gtest_filter=DdsLifecycleCycles.*)
  set_tests_properties(
    aimrt_dds_config_shared_qos_conflict
    aimrt_dds_config_topic_collisions
    aimrt_dds_executor_asio_auto
    aimrt_dds_executor_explicit
    aimrt_dds_executor_invalid_type
    aimrt_dds_listener_matches
    aimrt_dds_listener_incompatible_qos
    aimrt_dds_listener_entity_deletion
    aimrt_dds_lifecycle_cycles
    PROPERTIES TIMEOUT 60)

  add_executable(aimrt_dds_channel_test ${CMAKE_CURRENT_SOURCE_DIR}/dds_channel_test.cc ${CMAKE_CURRENT_SOURCE_DIR}/dds_backend.cc ${CMAKE_CURRENT_SOURCE_DIR}/dds_config.cc
                                        ${CMAKE_CURRENT_SOURCE_DIR}/dds_runtime.cc ${CMAKE_CURRENT_SOURCE_DIR}/global.cc)
  aimrt_add_dds_idl_codegen(
    TARGET_NAME
    aimrt_dds_loan_fixture
    IDL_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/DdsLoanTypes.idl
    OUTPUT_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/loan-generated
    ATTACH_TO_TARGET
    aimrt_dds_channel_test)
  target_include_directories(aimrt_dds_channel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PROJECT_SOURCE_DIR}/src/runtime)
  target_compile_definitions(aimrt_dds_channel_test PRIVATE BUILD_TESTING=1)
  if(AIMRT_DDS_ENABLE_XTYPES)
    target_compile_definitions(aimrt_dds_channel_test PRIVATE AIMRT_DDS_ENABLE_XTYPES)
  endif()
  if(AIMRT_BUILD_WITH_PROTOBUF)
    target_compile_definitions(aimrt_dds_channel_test PRIVATE AIMRT_BUILD_WITH_PROTOBUF)
  endif()
  if(AIMRT_BUILD_WITH_ROS2)
    target_compile_definitions(aimrt_dds_channel_test PRIVATE AIMRT_BUILD_WITH_ROS2)
  endif()
  target_link_libraries(
    aimrt_dds_channel_test
    PRIVATE aimrt_dds_rpc_codegen_fixture_lib
            aimrt_dds_serialized_message_adapter
            aimrt::interface::aimrt_core_plugin_interface
            aimrt::runtime::core
            aimrt::deps::fastdds
            tinyxml2::tinyxml2
            GTest::gtest)
  add_test(NAME aimrt_dds_channel_roundtrip COMMAND aimrt_dds_channel_test --gtest_filter=DdsChannelRoundtrip.*)
  add_test(NAME aimrt_dds_xtypes_cross_process COMMAND aimrt_dds_channel_test --gtest_filter=DdsXtypesCrossProcess.*)
  add_test(NAME aimrt_dds_stress COMMAND aimrt_dds_channel_test --gtest_filter=DdsStress.*)
  add_test(NAME aimrt_dds_wrapper_validation COMMAND aimrt_dds_channel_test --gtest_filter=DdsWrapperValidation.*)
  add_test(NAME aimrt_dds_channel_multi_module COMMAND aimrt_dds_channel_test --gtest_filter=DdsChannelMultiModule.*)
  add_test(NAME aimrt_dds_channel_order COMMAND aimrt_dds_channel_test --gtest_filter=DdsChannelOrder.*)
  add_test(NAME aimrt_dds_channel_reader_parallelism COMMAND aimrt_dds_channel_test --gtest_filter=DdsChannelReaderParallelism.*)
  add_test(NAME aimrt_dds_channel_start_boundary COMMAND aimrt_dds_channel_test --gtest_filter=DdsChannelStartBoundary.*)
  add_test(NAME aimrt_dds_channel_write_failure COMMAND aimrt_dds_channel_test --gtest_filter=DdsChannelWriteFailure.*)
  add_test(NAME aimrt_dds_listener_loss_rejection COMMAND aimrt_dds_channel_test --gtest_filter=DdsListenerLossRejection.*)
  add_test(NAME aimrt_dds_listener_deadline_liveliness COMMAND aimrt_dds_channel_test --gtest_filter=DdsListenerDeadlineLiveliness.*)
  add_test(NAME aimrt_dds_loan_publisher_plain COMMAND aimrt_dds_channel_test --gtest_filter=DdsLoanPublisherPlain.*)
  add_test(NAME aimrt_dds_loan_publisher_non_plain COMMAND aimrt_dds_channel_test --gtest_filter=DdsLoanPublisherNonPlain.*)
  add_test(NAME aimrt_dds_loan_publisher_runtime_failure COMMAND aimrt_dds_channel_test --gtest_filter=DdsLoanPublisherRuntimeFailure.*)
  add_test(NAME aimrt_dds_loan_subscriber COMMAND aimrt_dds_channel_test --gtest_filter=DdsLoanSubscriber.*)
  add_test(NAME aimrt_dds_loan_mixed_subscription COMMAND aimrt_dds_channel_test --gtest_filter=DdsLoanMixedSubscription.*)
  add_test(NAME aimrt_dds_loan_core_restrictions COMMAND aimrt_runtime_core_test --gtest_filter=ChannelBackendManagerLoanRouteTest.*)
  add_test(NAME aimrt_dds_loan_shutdown COMMAND aimrt_dds_channel_test --gtest_filter=DdsLoanShutdown.*)
  add_test(NAME aimrt_dds_listener_entity_deletion_loan COMMAND aimrt_dds_channel_test --gtest_filter=DdsListenerEntityDeletionLoan.*)
  set_tests_properties(
    aimrt_dds_channel_roundtrip
    aimrt_dds_wrapper_validation
    aimrt_dds_channel_multi_module
    aimrt_dds_channel_order
    aimrt_dds_channel_reader_parallelism
    aimrt_dds_channel_start_boundary
    aimrt_dds_channel_write_failure
    aimrt_dds_listener_loss_rejection
    aimrt_dds_listener_deadline_liveliness
    aimrt_dds_loan_publisher_plain
    aimrt_dds_loan_publisher_non_plain
    aimrt_dds_loan_publisher_runtime_failure
    aimrt_dds_loan_subscriber
    aimrt_dds_loan_mixed_subscription
    aimrt_dds_loan_core_restrictions
    aimrt_dds_loan_shutdown
    aimrt_dds_listener_entity_deletion_loan
    PROPERTIES TIMEOUT 30)
  set_tests_properties(aimrt_dds_listener_entity_deletion_loan PROPERTIES TIMEOUT 60)
  set_tests_properties(aimrt_dds_xtypes_cross_process PROPERTIES TIMEOUT 120)
  set_tests_properties(aimrt_dds_stress PROPERTIES TIMEOUT 60)
  set_tests_properties(aimrt_dds_loan_shutdown PROPERTIES ENVIRONMENT "AIMRT_RUNTIME_CORE_TEST_PATH=$<TARGET_FILE:aimrt_runtime_core_test>")
endif()
