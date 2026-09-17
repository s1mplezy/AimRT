# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

include_guard(GLOBAL)
include(FastDdsCodegenTool)
set(AIMRT_DDS_CODEGEN_RUNNER "${CMAKE_CURRENT_LIST_DIR}/RunFastDdsCodegen.cmake")

# Generate Fast DDS C++ types and AimRT bindings from one or more IDL files.
#
# The generated root is the sole public include directory. AimRT entry points
# are <stem>.h/.cc at that root; native Fast DDS files are private layout
# details under detail/<stem>/.
function(aimrt_add_dds_idl_codegen)
  cmake_parse_arguments(ARG "" "TARGET_NAME;OUTPUT_DIR;ATTACH_TO_TARGET" "IDL_FILES;INCLUDE_DIRS" ${ARGN})

  if(NOT ARG_TARGET_NAME)
    message(FATAL_ERROR "aimrt_add_dds_idl_codegen requires TARGET_NAME")
  endif()
  if(NOT ARG_IDL_FILES)
    message(FATAL_ERROR "aimrt_add_dds_idl_codegen requires at least one IDL file")
  endif()
  if(TARGET ${ARG_TARGET_NAME})
    message(FATAL_ERROR "DDS codegen target already exists: ${ARG_TARGET_NAME}")
  endif()
  if(ARG_ATTACH_TO_TARGET AND NOT TARGET ${ARG_ATTACH_TO_TARGET})
    message(FATAL_ERROR "ATTACH_TO_TARGET does not exist: ${ARG_ATTACH_TO_TARGET}")
  endif()

  if(NOT ARG_OUTPUT_DIR)
    set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/dds/${ARG_TARGET_NAME}")
  endif()
  cmake_path(
    ABSOLUTE_PATH
    ARG_OUTPUT_DIR
    BASE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}"
    NORMALIZE
    OUTPUT_VARIABLE
    ARG_OUTPUT_DIR)

  set(AIMRT_DDS_BUILD_ROOT "${CMAKE_BINARY_DIR}")
  cmake_path(IS_PREFIX AIMRT_DDS_BUILD_ROOT "${ARG_OUTPUT_DIR}" NORMALIZE AIMRT_DDS_OUTPUT_IN_BUILD_TREE)
  if(NOT AIMRT_DDS_OUTPUT_IN_BUILD_TREE)
    set(AIMRT_DDS_SOURCE_ROOT "${PROJECT_SOURCE_DIR}")
    cmake_path(IS_PREFIX AIMRT_DDS_SOURCE_ROOT "${ARG_OUTPUT_DIR}" NORMALIZE AIMRT_DDS_OUTPUT_IN_SOURCE_TREE)
    if(AIMRT_DDS_OUTPUT_IN_SOURCE_TREE)
      message(FATAL_ERROR "DDS generated output must be in the build tree, not the source tree: ${ARG_OUTPUT_DIR}")
    endif()
    message(FATAL_ERROR "DDS generated output must be within the current build tree (${CMAKE_BINARY_DIR}): ${ARG_OUTPUT_DIR}")
  endif()

  if(NOT FASTDDSGEN_EXECUTABLE OR NOT IS_ABSOLUTE "${FASTDDSGEN_EXECUTABLE}")
    message(FATAL_ERROR "FASTDDSGEN_EXECUTABLE must be the validated absolute Fast DDS-Gen 4.3.0 path")
  endif()
  if(NOT Java_JAVA_EXECUTABLE)
    find_package(Java REQUIRED COMPONENTS Runtime)
  endif()
  aimrt_validate_dds_codegen_tools(FASTDDSGEN_EXECUTABLE "${FASTDDSGEN_EXECUTABLE}" JAVA_EXECUTABLE "${Java_JAVA_EXECUTABLE}")

  get_property(AIMRT_DDS_RPC_GEN_COMMAND GLOBAL PROPERTY AIMRT_DDS_RPC_GEN_COMMAND_PROPERTY)
  if(NOT AIMRT_DDS_RPC_GEN_COMMAND)
    find_program(AIMRT_DDS_RPC_GEN_EXECUTABLE NAMES aimrt_dds_rpc_gen REQUIRED)
    set(AIMRT_DDS_RPC_GEN_COMMAND "${AIMRT_DDS_RPC_GEN_EXECUTABLE}")
  endif()

  set(FASTDDSGEN_INCLUDE_ARGS)
  set(AIMRT_RPC_INCLUDE_ARGS)
  foreach(INCLUDE_DIR IN LISTS ARG_INCLUDE_DIRS)
    get_filename_component(ABS_INCLUDE_DIR "${INCLUDE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND FASTDDSGEN_INCLUDE_ARGS -I "${ABS_INCLUDE_DIR}")
    list(APPEND AIMRT_RPC_INCLUDE_ARGS --include-dir "${ABS_INCLUDE_DIR}")
  endforeach()

  set(FASTDDSGEN_XTYPES_ARGS)
  if(NOT AIMRT_DDS_ENABLE_XTYPES)
    list(APPEND FASTDDSGEN_XTYPES_ARGS -no-typeobjectsupport)
  endif()

  # Resolve the complete union graph before declaring any output. Canonical
  # paths are de-duplicated, sorted, and each node is generated exactly once.
  set(AIMRT_DDS_GRAPH_IDLS)
  foreach(IDL_FILE IN LISTS ARG_IDL_FILES)
    get_filename_component(ABS_IDL_FILE "${IDL_FILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${ABS_IDL_FILE}")
      message(FATAL_ERROR "DDS IDL file does not exist: ${ABS_IDL_FILE}")
    endif()
    get_filename_component(ABS_IDL_FILE "${ABS_IDL_FILE}" REALPATH)
    execute_process(
      COMMAND ${AIMRT_DDS_RPC_GEN_COMMAND} --idl "${ABS_IDL_FILE}" --output-dir "${ARG_OUTPUT_DIR}" --print-dependencies ${AIMRT_RPC_INCLUDE_ARGS}
      RESULT_VARIABLE IDL_GRAPH_RESULT
      OUTPUT_VARIABLE IDL_GRAPH_OUTPUT
      ERROR_VARIABLE IDL_GRAPH_ERROR)
    if(NOT IDL_GRAPH_RESULT EQUAL 0)
      message(FATAL_ERROR "AimRT DDS IDL dependency scan failed for ${ABS_IDL_FILE}:\n${IDL_GRAPH_ERROR}")
    endif()
    string(REPLACE "\r" "" IDL_GRAPH_OUTPUT "${IDL_GRAPH_OUTPUT}")
    string(REPLACE "\n" ";" IDL_GRAPH "${IDL_GRAPH_OUTPUT}")
    list(FILTER IDL_GRAPH EXCLUDE REGEX "^$")
    if(NOT IDL_GRAPH)
      message(FATAL_ERROR "AimRT DDS IDL dependency scan returned an empty graph for ${ABS_IDL_FILE}")
    endif()
    list(APPEND AIMRT_DDS_GRAPH_IDLS ${IDL_GRAPH})
  endforeach()
  list(REMOVE_DUPLICATES AIMRT_DDS_GRAPH_IDLS)
  list(SORT AIMRT_DDS_GRAPH_IDLS)
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS ${AIMRT_DDS_GRAPH_IDLS})

  set(AIMRT_DDS_GRAPH_STEMS)
  set(AIMRT_DDS_GRAPH_PATHS)
  set(AIMRT_DDS_COLLISIONS)
  foreach(GRAPH_IDL IN LISTS AIMRT_DDS_GRAPH_IDLS)
    get_filename_component(GRAPH_STEM "${GRAPH_IDL}" NAME_WE)
    if(GRAPH_STEM IN_LIST AIMRT_DDS_GRAPH_STEMS)
      list(FIND AIMRT_DDS_GRAPH_STEMS "${GRAPH_STEM}" GRAPH_STEM_INDEX)
      list(GET AIMRT_DDS_GRAPH_PATHS ${GRAPH_STEM_INDEX} GRAPH_STEM_EXISTING_PATH)
      list(APPEND AIMRT_DDS_COLLISIONS "${GRAPH_STEM_EXISTING_PATH}" "${GRAPH_IDL}")
    else()
      list(APPEND AIMRT_DDS_GRAPH_STEMS "${GRAPH_STEM}")
      list(APPEND AIMRT_DDS_GRAPH_PATHS "${GRAPH_IDL}")
    endif()
  endforeach()
  if(AIMRT_DDS_COLLISIONS)
    list(REMOVE_DUPLICATES AIMRT_DDS_COLLISIONS)
    list(SORT AIMRT_DDS_COLLISIONS)
    string(JOIN "\n  " AIMRT_DDS_COLLISION_LIST ${AIMRT_DDS_COLLISIONS})
    message(FATAL_ERROR "DDS IDL graph has duplicate stems or colliding output paths:\n  ${AIMRT_DDS_COLLISION_LIST}")
  endif()

  set(ALL_GENERATED_FILES)
  set(ALL_GENERATED_SOURCES)
  set(ALL_GENERATED_HEADERS)
  foreach(GRAPH_IDL GRAPH_STEM IN ZIP_LISTS AIMRT_DDS_GRAPH_IDLS AIMRT_DDS_GRAPH_STEMS)
    set(NATIVE_OUTPUT_DIR "${ARG_OUTPUT_DIR}/detail/${GRAPH_STEM}")
    set(STAGING_DIR "${ARG_OUTPUT_DIR}/.aimrt-codegen-staging/${GRAPH_STEM}")
    set(FASTDDS_SOURCES "${NATIVE_OUTPUT_DIR}/${GRAPH_STEM}PubSubTypes.cxx")
    set(FASTDDS_HEADERS "${NATIVE_OUTPUT_DIR}/${GRAPH_STEM}.hpp" "${NATIVE_OUTPUT_DIR}/${GRAPH_STEM}CdrAux.hpp" "${NATIVE_OUTPUT_DIR}/${GRAPH_STEM}CdrAux.ipp"
                        "${NATIVE_OUTPUT_DIR}/${GRAPH_STEM}PubSubTypes.hpp")
    if(AIMRT_DDS_ENABLE_XTYPES)
      list(APPEND FASTDDS_SOURCES "${NATIVE_OUTPUT_DIR}/${GRAPH_STEM}TypeObjectSupport.cxx")
      list(APPEND FASTDDS_HEADERS "${NATIVE_OUTPUT_DIR}/${GRAPH_STEM}TypeObjectSupport.hpp")
    endif()
    set(AIMRT_BINDING_SOURCE "${ARG_OUTPUT_DIR}/${GRAPH_STEM}.cc")
    set(AIMRT_BINDING_HEADER "${ARG_OUTPUT_DIR}/${GRAPH_STEM}.h")
    set(CURRENT_GENERATED_FILES ${FASTDDS_SOURCES} ${FASTDDS_HEADERS} "${AIMRT_BINDING_SOURCE}" "${AIMRT_BINDING_HEADER}")

    execute_process(
      COMMAND ${AIMRT_DDS_RPC_GEN_COMMAND} --idl "${GRAPH_IDL}" --output-dir "${ARG_OUTPUT_DIR}" --print-dependencies ${AIMRT_RPC_INCLUDE_ARGS}
      RESULT_VARIABLE NODE_GRAPH_RESULT
      OUTPUT_VARIABLE NODE_GRAPH_OUTPUT
      ERROR_VARIABLE NODE_GRAPH_ERROR)
    if(NOT NODE_GRAPH_RESULT EQUAL 0)
      message(FATAL_ERROR "AimRT DDS IDL dependency scan failed for graph node ${GRAPH_IDL}:\n${NODE_GRAPH_ERROR}")
    endif()
    string(REPLACE "\r" "" NODE_GRAPH_OUTPUT "${NODE_GRAPH_OUTPUT}")
    string(REPLACE "\n" ";" NODE_GRAPH "${NODE_GRAPH_OUTPUT}")
    list(FILTER NODE_GRAPH EXCLUDE REGEX "^$")

    set(CODEGEN_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${ARG_TARGET_NAME}-${GRAPH_STEM}-dds-codegen.cmake")
    file(WRITE "${CODEGEN_MANIFEST}" "set(AIMRT_DDS_FASTDDSGEN_EXECUTABLE [==[${FASTDDSGEN_EXECUTABLE}]==])\n")
    file(APPEND "${CODEGEN_MANIFEST}" "set(AIMRT_DDS_ROOT_IDL [==[${GRAPH_IDL}]==])\n")
    file(APPEND "${CODEGEN_MANIFEST}" "set(AIMRT_DDS_OUTPUT_DIR [==[${ARG_OUTPUT_DIR}]==])\n")
    file(APPEND "${CODEGEN_MANIFEST}" "set(AIMRT_DDS_STAGING_DIR [==[${STAGING_DIR}]==])\n")
    file(APPEND "${CODEGEN_MANIFEST}" "set(AIMRT_DDS_STAGING_DEPFILE [==[${STAGING_DIR}/${GRAPH_STEM}.aimrt_rpc.d]==])\n")
    file(APPEND "${CODEGEN_MANIFEST}" "set(AIMRT_DDS_FINAL_DEPFILE [==[${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${ARG_TARGET_NAME}-${GRAPH_STEM}.aimrt_rpc.d]==])\n")
    foreach(CODEGEN_VALUE IN LISTS FASTDDSGEN_INCLUDE_ARGS)
      file(APPEND "${CODEGEN_MANIFEST}" "list(APPEND AIMRT_DDS_FASTDDSGEN_INCLUDE_ARGS [==[${CODEGEN_VALUE}]==])\n")
    endforeach()
    foreach(CODEGEN_VALUE IN LISTS FASTDDSGEN_XTYPES_ARGS)
      file(APPEND "${CODEGEN_MANIFEST}" "list(APPEND AIMRT_DDS_FASTDDSGEN_XTYPES_ARGS [==[${CODEGEN_VALUE}]==])\n")
    endforeach()
    foreach(CODEGEN_VALUE IN LISTS AIMRT_DDS_RPC_GEN_COMMAND)
      file(APPEND "${CODEGEN_MANIFEST}" "list(APPEND AIMRT_DDS_RPC_GEN_COMMAND [==[${CODEGEN_VALUE}]==])\n")
    endforeach()
    foreach(CODEGEN_VALUE IN LISTS AIMRT_RPC_INCLUDE_ARGS)
      file(APPEND "${CODEGEN_MANIFEST}" "list(APPEND AIMRT_DDS_RPC_INCLUDE_ARGS [==[${CODEGEN_VALUE}]==])\n")
    endforeach()
    foreach(MAP_STEM IN LISTS AIMRT_DDS_GRAPH_STEMS)
      foreach(MAP_SUFFIX IN ITEMS ".hpp" "CdrAux.hpp" "CdrAux.ipp" "PubSubTypes.hpp" "TypeObjectSupport.hpp")
        file(APPEND "${CODEGEN_MANIFEST}" "list(APPEND AIMRT_DDS_INCLUDE_BASENAMES [==[${MAP_STEM}${MAP_SUFFIX}]==])\n")
        file(APPEND "${CODEGEN_MANIFEST}" "list(APPEND AIMRT_DDS_INCLUDE_PATHS [==[detail/${MAP_STEM}/${MAP_STEM}${MAP_SUFFIX}]==])\n")
      endforeach()
      file(APPEND "${CODEGEN_MANIFEST}" "list(APPEND AIMRT_DDS_INCLUDE_BASENAMES [==[${MAP_STEM}.h]==])\n")
      file(APPEND "${CODEGEN_MANIFEST}" "list(APPEND AIMRT_DDS_INCLUDE_PATHS [==[${MAP_STEM}.h]==])\n")
    endforeach()
    foreach(GENERATED_FILE IN LISTS CURRENT_GENERATED_FILES)
      get_filename_component(GENERATED_NAME "${GENERATED_FILE}" NAME)
      file(APPEND "${CODEGEN_MANIFEST}" "list(APPEND AIMRT_DDS_PUBLISH_SOURCES [==[${STAGING_DIR}/${GENERATED_NAME}]==])\n")
      file(APPEND "${CODEGEN_MANIFEST}" "list(APPEND AIMRT_DDS_PUBLISH_DESTINATIONS [==[${GENERATED_FILE}]==])\n")
    endforeach()

    add_custom_command(
      OUTPUT ${CURRENT_GENERATED_FILES}
      COMMAND "${CMAKE_COMMAND}" "-DAIMRT_DDS_CODEGEN_MANIFEST=${CODEGEN_MANIFEST}" -P "${AIMRT_DDS_CODEGEN_RUNNER}"
      DEPENDS ${NODE_GRAPH} "${CODEGEN_MANIFEST}" "${AIMRT_DDS_CODEGEN_RUNNER}"
      COMMENT "Generating Fast DDS types and AimRT binding for ${GRAPH_IDL}"
      VERBATIM COMMAND_EXPAND_LISTS)

    list(APPEND ALL_GENERATED_FILES ${CURRENT_GENERATED_FILES})
    list(APPEND ALL_GENERATED_SOURCES ${FASTDDS_SOURCES} "${AIMRT_BINDING_SOURCE}")
    list(APPEND ALL_GENERATED_HEADERS ${FASTDDS_HEADERS} "${AIMRT_BINDING_HEADER}")
  endforeach()

  add_custom_target(${ARG_TARGET_NAME} DEPENDS ${ALL_GENERATED_FILES})
  set_target_properties(
    ${ARG_TARGET_NAME}
    PROPERTIES AIMRT_DDS_GENERATED_SOURCES "${ALL_GENERATED_SOURCES}"
               AIMRT_DDS_GENERATED_HEADERS "${ALL_GENERATED_HEADERS}"
               AIMRT_DDS_GENERATED_INCLUDE_DIR "${ARG_OUTPUT_DIR}"
               AIMRT_DDS_GENERATED_INCLUDE_DIRS "${ARG_OUTPUT_DIR}")

  if(ARG_ATTACH_TO_TARGET)
    add_dependencies(${ARG_ATTACH_TO_TARGET} ${ARG_TARGET_NAME})
    target_sources(${ARG_ATTACH_TO_TARGET} PRIVATE ${ALL_GENERATED_SOURCES})
    target_sources(${ARG_ATTACH_TO_TARGET} PUBLIC FILE_SET HEADERS BASE_DIRS "${ARG_OUTPUT_DIR}" FILES ${ALL_GENERATED_HEADERS})
    target_include_directories(${ARG_ATTACH_TO_TARGET} PUBLIC "$<BUILD_INTERFACE:${ARG_OUTPUT_DIR}>")
  endif()
endfunction()
