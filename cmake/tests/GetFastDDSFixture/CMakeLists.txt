cmake_minimum_required(VERSION 3.24)
project(aimrt_get_fastdds_fixture LANGUAGES CXX)

if(NOT AIMRT_GET_FASTDDS_MODULE_DIR)
  message(FATAL_ERROR "AIMRT_GET_FASTDDS_MODULE_DIR is required")
endif()

set(AIMRT_INSTALL OFF)
if(AIMRT_GET_FASTDDS_PREDEFINE_EPROSIMA_ATOMIC)
  add_library(eProsima_atomic INTERFACE IMPORTED)
  add_library(foonathan_memory INTERFACE IMPORTED)
  set(foonathan_memory_DIR "${CMAKE_BINARY_DIR}/fake-foonathan-package")
endif()
list(APPEND CMAKE_MODULE_PATH "${AIMRT_GET_FASTDDS_MODULE_DIR}")
include(GetFastDDS)

if(NOT TARGET aimrt::deps::fastdds)
  message(FATAL_ERROR "GetFastDDS.cmake did not define aimrt::deps::fastdds")
endif()

file(
  GENERATE
  OUTPUT "${CMAKE_BINARY_DIR}/get-fastdds-report.txt"
  CONTENT
    "source=${AIMRT_FASTDDS_SELECTION_SOURCE}\nversion=${AIMRT_FASTDDS_VERSION}\nprefix=${AIMRT_FASTDDS_PREFIX}\nprivate_target=${AIMRT_FASTDDS_PRIVATE_TARGET}\nlink_mode=${AIMRT_FASTDDS_LINK_MODE}\nisolation=${AIMRT_FASTDDS_ROS2_ISOLATION_MODE}\nruntime=${AIMRT_FASTDDS_RUNTIME_LIBRARY_PATH}\nfastcdr_runtime=${AIMRT_FASTDDS_FASTCDR_RUNTIME_LIBRARY_PATH}\nfastcdr_version=${AIMRT_FASTDDS_FASTCDR_VERSION}\nfastcdr_soname=${AIMRT_FASTDDS_FASTCDR_SONAME}\nfastcdr_compatibility_proven=${AIMRT_FASTDDS_FASTCDR_COMPATIBILITY_PROVEN}\n"
)
