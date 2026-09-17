# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

include_guard(GLOBAL)

function(aimrt_find_dds_codegen_tool)
  cmake_parse_arguments(ARG "" "FASTDDSGEN_EXECUTABLE;OUTPUT_VARIABLE" "" ${ARGN})

  if(NOT ARG_OUTPUT_VARIABLE)
    message(FATAL_ERROR "aimrt_find_dds_codegen_tool requires OUTPUT_VARIABLE")
  endif()

  if(ARG_FASTDDSGEN_EXECUTABLE)
    set(FASTDDSGEN_EXECUTABLE_RESOLVED "${ARG_FASTDDSGEN_EXECUTABLE}")
  else()
    find_program(FASTDDSGEN_EXECUTABLE_RESOLVED NAMES fastddsgen)
    if(NOT FASTDDSGEN_EXECUTABLE_RESOLVED)
      message(FATAL_ERROR "AIMRT_BUILD_WITH_DDS is ON, but Fast DDS-Gen was not found in PATH. " "Install Fast DDS-Gen 4.3.0 and add fastddsgen to PATH, or set "
                          "FASTDDSGEN_EXECUTABLE to its absolute path.")
    endif()
    message(STATUS "DDS Fast DDS-Gen executable discovered from PATH: ${FASTDDSGEN_EXECUTABLE_RESOLVED}")
  endif()

  set(${ARG_OUTPUT_VARIABLE}
      "${FASTDDSGEN_EXECUTABLE_RESOLVED}"
      PARENT_SCOPE)
endfunction()

function(aimrt_validate_dds_codegen_tools)
  cmake_parse_arguments(ARG "" "FASTDDSGEN_EXECUTABLE;JAVA_EXECUTABLE" "" ${ARGN})

  if(NOT ARG_FASTDDSGEN_EXECUTABLE)
    message(FATAL_ERROR "AIMRT_BUILD_WITH_DDS is ON, but FASTDDSGEN_EXECUTABLE is empty. Set it to the absolute path of Fast DDS-Gen 4.3.0.")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_FASTDDSGEN_EXECUTABLE}")
    message(FATAL_ERROR "FASTDDSGEN_EXECUTABLE must be an absolute path: ${ARG_FASTDDSGEN_EXECUTABLE}")
  endif()
  if(NOT EXISTS "${ARG_FASTDDSGEN_EXECUTABLE}" OR IS_DIRECTORY "${ARG_FASTDDSGEN_EXECUTABLE}")
    message(FATAL_ERROR "FASTDDSGEN_EXECUTABLE does not name an executable file: ${ARG_FASTDDSGEN_EXECUTABLE}")
  endif()
  if(NOT ARG_JAVA_EXECUTABLE)
    message(FATAL_ERROR "Java runtime executable is required for Fast DDS-Gen")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_JAVA_EXECUTABLE}"
     OR NOT EXISTS "${ARG_JAVA_EXECUTABLE}"
     OR IS_DIRECTORY "${ARG_JAVA_EXECUTABLE}")
    message(FATAL_ERROR "JAVA_EXECUTABLE must name an existing absolute executable path: ${ARG_JAVA_EXECUTABLE}")
  endif()

  execute_process(
    COMMAND "${ARG_FASTDDSGEN_EXECUTABLE}" -version
    RESULT_VARIABLE FASTDDSGEN_VERSION_RESULT
    OUTPUT_VARIABLE FASTDDSGEN_VERSION_STDOUT
    ERROR_VARIABLE FASTDDSGEN_VERSION_STDERR)
  set(FASTDDSGEN_VERSION_OUTPUT "${FASTDDSGEN_VERSION_STDOUT}${FASTDDSGEN_VERSION_STDERR}")
  if(NOT FASTDDSGEN_VERSION_RESULT EQUAL 0)
    message(FATAL_ERROR "Fast DDS-Gen version command failed (${FASTDDSGEN_VERSION_RESULT}):\n${FASTDDSGEN_VERSION_OUTPUT}")
  endif()
  string(REPLACE "\r" "" FASTDDSGEN_VERSION_NORMALIZED "${FASTDDSGEN_VERSION_OUTPUT}")
  string(REPLACE "\n" ";" FASTDDSGEN_VERSION_LINES "${FASTDDSGEN_VERSION_NORMALIZED}")
  set(FASTDDSGEN_VERSION_LINE_COUNT 0)
  set(FASTDDSGEN_VERSION_IS_4_3_0 FALSE)
  foreach(VERSION_LINE IN LISTS FASTDDSGEN_VERSION_LINES)
    string(STRIP "${VERSION_LINE}" VERSION_LINE)
    if(VERSION_LINE MATCHES "^fastddsgen version ")
      math(EXPR FASTDDSGEN_VERSION_LINE_COUNT "${FASTDDSGEN_VERSION_LINE_COUNT} + 1")
      if(VERSION_LINE STREQUAL "fastddsgen version 4.3.0")
        set(FASTDDSGEN_VERSION_IS_4_3_0 TRUE)
      endif()
    endif()
  endforeach()
  if(NOT FASTDDSGEN_VERSION_LINE_COUNT EQUAL 1 OR NOT FASTDDSGEN_VERSION_IS_4_3_0)
    message(FATAL_ERROR "Fast DDS-Gen must report exactly version 4.3.0. Raw output:\n${FASTDDSGEN_VERSION_OUTPUT}")
  endif()

  execute_process(
    COMMAND "${ARG_JAVA_EXECUTABLE}" -version
    RESULT_VARIABLE JAVA_VERSION_RESULT
    OUTPUT_VARIABLE JAVA_VERSION_STDOUT
    ERROR_VARIABLE JAVA_VERSION_STDERR)
  set(JAVA_VERSION_OUTPUT "${JAVA_VERSION_STDOUT}${JAVA_VERSION_STDERR}")
  if(NOT JAVA_VERSION_RESULT EQUAL 0)
    message(FATAL_ERROR "Java version command failed (${JAVA_VERSION_RESULT}):\n${JAVA_VERSION_OUTPUT}")
  endif()

  message(STATUS "DDS Fast DDS-Gen executable: ${ARG_FASTDDSGEN_EXECUTABLE}")
  message(STATUS "DDS Fast DDS-Gen raw version output:\n${FASTDDSGEN_VERSION_OUTPUT}")
  message(STATUS "DDS Java executable: ${ARG_JAVA_EXECUTABLE}")
  message(STATUS "DDS Java raw version output:\n${JAVA_VERSION_OUTPUT}")

  set(AIMRT_FASTDDSGEN_VERSION_OUTPUT
      "${FASTDDSGEN_VERSION_OUTPUT}"
      PARENT_SCOPE)
  set(AIMRT_JAVA_VERSION_OUTPUT
      "${JAVA_VERSION_OUTPUT}"
      PARENT_SCOPE)
endfunction()
