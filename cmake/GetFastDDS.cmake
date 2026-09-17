# Copyright (c) 2023, AgiBot Inc.
# All rights reserved.

include_guard(GLOBAL)

include(FetchContent)
include(ExternalProject)

set(FastDDS_LOCAL_SOURCE
    ""
    CACHE PATH "Use a local Fast DDS source tree instead of an installed package or the reproducible default source.")
set(FastDDS_ROOT
    ""
    CACHE PATH "Root of an installed Fast DDS 3.6.x package selected for the AimRT DDS plugin.")
set(FastDDS_DEFAULT_GIT_REPOSITORY
    "https://github.com/eProsima/Fast-DDS.git"
    CACHE STRING "Repository used for the reproducible Fast DDS fallback.")
set(FastDDS_DEFAULT_GIT_TAG
    "dd66ef2aff7230c7d39862dc7384402b88156d6f"
    CACHE STRING "Exact commit used for the reproducible Fast DDS fallback.")
set(FastDDS_DEFAULT_SOURCE_MIRROR
    ""
    CACHE PATH "Optional local mirror of the reproducible Fast DDS fallback for offline builds and tests.")
set(AIMRT_FASTDDS_STRICT_EPROSIMA_PREFIX
    OFF
    CACHE BOOL "Reject every eProsima dependency that resolves outside the selected Fast DDS prefix.")
set(AIMRT_FASTDDS_EPROSIMA_ALLOWLIST
    ""
    CACHE STRING "Semicolon-separated prefixes explicitly approved for Fast DDS eProsima transitive dependencies.")

function(_aimrt_fastdds_target_location target_name output_var)
  if(NOT TARGET ${target_name})
    set(${output_var}
        ""
        PARENT_SCOPE)
    return()
  endif()

  get_target_property(target_is_imported ${target_name} IMPORTED)
  if(NOT target_is_imported)
    get_target_property(target_type ${target_name} TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY")
      set(${output_var}
          "<build-tree:${target_name}>"
          PARENT_SCOPE)
      return()
    endif()
    set(${output_var}
        "$<TARGET_FILE:${target_name}>"
        PARENT_SCOPE)
    return()
  endif()

  set(location_properties IMPORTED_LOCATION)
  get_target_property(imported_configurations ${target_name} IMPORTED_CONFIGURATIONS)
  foreach(configuration IN LISTS imported_configurations)
    string(TOUPPER "${configuration}" configuration_upper)
    list(APPEND location_properties "IMPORTED_LOCATION_${configuration_upper}")
  endforeach()
  list(APPEND location_properties IMPORTED_LOCATION_NOCONFIG IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_RELWITHDEBINFO IMPORTED_LOCATION_DEBUG)
  list(REMOVE_DUPLICATES location_properties)

  foreach(location_property IN LISTS location_properties)
    get_target_property(candidate_location ${target_name} ${location_property})
    if(candidate_location AND NOT candidate_location MATCHES "-NOTFOUND$")
      set(${output_var}
          "${candidate_location}"
          PARENT_SCOPE)
      return()
    endif()
  endforeach()

  set(${output_var}
      ""
      PARENT_SCOPE)
endfunction()

function(_aimrt_fastdds_path_is_under path root output_var)
  if(NOT path
     OR NOT root
     OR path MATCHES "^\\$<")
    set(${output_var}
        FALSE
        PARENT_SCOPE)
    return()
  endif()
  file(REAL_PATH "${path}" normalized_path EXPAND_TILDE)
  file(REAL_PATH "${root}" normalized_root EXPAND_TILDE)
  string(FIND "${normalized_path}/" "${normalized_root}/" prefix_position)
  if(prefix_position EQUAL 0)
    set(${output_var}
        TRUE
        PARENT_SCOPE)
  else()
    set(${output_var}
        FALSE
        PARENT_SCOPE)
  endif()
endfunction()

function(_aimrt_fastdds_parse_source_version source_dir output_var)
  if(NOT EXISTS "${source_dir}/CMakeLists.txt")
    set(${output_var}
        ""
        PARENT_SCOPE)
    return()
  endif()
  file(READ "${source_dir}/CMakeLists.txt" source_cmake)
  string(REGEX MATCH "project\\([ \t\r\n]*fastdds[ \t\r\n]+VERSION[ \t\r\n]+\"?([0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?)" version_match "${source_cmake}")
  set(${output_var}
      "${CMAKE_MATCH_1}"
      PARENT_SCOPE)
endfunction()

function(_aimrt_fastdds_parse_fastcdr_source_version source_dir output_var)
  if(NOT EXISTS "${source_dir}/thirdparty/fastcdr/CMakeLists.txt")
    set(${output_var}
        ""
        PARENT_SCOPE)
    return()
  endif()
  file(READ "${source_dir}/thirdparty/fastcdr/CMakeLists.txt" fastcdr_source_cmake)
  string(REGEX MATCH "project\\([ \t\r\n]*fastcdr[ \t\r\n]+VERSION[ \t\r\n]+\"?([0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?)" version_match "${fastcdr_source_cmake}")
  set(${output_var}
      "${CMAKE_MATCH_1}"
      PARENT_SCOPE)
endfunction()

function(_aimrt_fastdds_add_isolated_source source_dir source_version selection_source)
  if(NOT foonathan_memory_DIR)
    message(FATAL_ERROR "The isolated Fast DDS source build requires a resolved foonathan_memory package directory so its dependency closure can be reproduced.")
  endif()

  _aimrt_fastdds_parse_fastcdr_source_version("${source_dir}" bundled_fastcdr_version)
  if(NOT bundled_fastcdr_version MATCHES "^2\\.")
    message(FATAL_ERROR "The selected Fast DDS source does not provide a verifiable bundled Fast CDR 2.x source.")
  endif()

  set(isolated_root "${CMAKE_BINARY_DIR}/_deps/aimrt-fastdds-isolated")
  set(isolated_build "${isolated_root}/build")
  set(isolated_install "${isolated_root}/install")
  set(isolated_fastdds_library "${isolated_install}/lib/libfastdds.a")
  set(isolated_fastcdr_library "${isolated_install}/lib/libfastcdr.a")
  file(MAKE_DIRECTORY "${isolated_install}/include" "${isolated_install}/lib")

  string(REPLACE ";" "|" isolated_prefix_path "${CMAKE_PREFIX_PATH}")
  ExternalProject_Add(
    aimrt_fastdds_isolated_source_build
    SOURCE_DIR "${source_dir}"
    BINARY_DIR "${isolated_build}"
    INSTALL_DIR "${isolated_install}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    LIST_SEPARATOR "|"
    CMAKE_ARGS "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
               "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
               "-DCMAKE_INSTALL_LIBDIR=lib"
               "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
               "-DCMAKE_PREFIX_PATH=${isolated_prefix_path}"
               "-Dfoonathan_memory_DIR=${foonathan_memory_DIR}"
               "-DBUILD_SHARED_LIBS=OFF"
               "-DBUILD_TESTING=OFF"
               "-DCOMPILE_EXAMPLES=OFF"
               "-DCOMPILE_TOOLS=OFF"
               "-DTHIRDPARTY=OFF"
               "-DTHIRDPARTY_fastcdr=FORCE"
               "-DTHIRDPARTY_UPDATE=OFF"
               "-DTHIRDPARTY_Asio=OFF"
               "-DTHIRDPARTY_TinyXML2=OFF"
    BUILD_BYPRODUCTS "${isolated_fastdds_library}" "${isolated_fastcdr_library}"
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
    USES_TERMINAL_INSTALL TRUE)

  add_library(aimrt_fastdds_isolated_archive STATIC IMPORTED GLOBAL)
  set_target_properties(aimrt_fastdds_isolated_archive PROPERTIES IMPORTED_LOCATION "${isolated_fastdds_library}")
  add_dependencies(aimrt_fastdds_isolated_archive aimrt_fastdds_isolated_source_build)

  add_library(aimrt_fastdds_isolated_fastcdr STATIC IMPORTED GLOBAL)
  set_target_properties(aimrt_fastdds_isolated_fastcdr PROPERTIES IMPORTED_LOCATION "${isolated_fastcdr_library}")
  add_dependencies(aimrt_fastdds_isolated_fastcdr aimrt_fastdds_isolated_source_build)

  if(NOT TARGET eProsima_atomic)
    add_library(eProsima_atomic INTERFACE IMPORTED)
  endif()
  find_package(OpenSSL REQUIRED)
  find_package(tinyxml2 REQUIRED CONFIG)
  add_library(aimrt_fastdds_selected INTERFACE)
  add_dependencies(aimrt_fastdds_selected aimrt_fastdds_isolated_source_build)
  target_include_directories(aimrt_fastdds_selected INTERFACE "${isolated_install}/include")
  target_link_libraries(
    aimrt_fastdds_selected
    INTERFACE aimrt_fastdds_isolated_archive
              aimrt_fastdds_isolated_fastcdr
              foonathan_memory
              eProsima_atomic
              -lpthread
              dl
              tinyxml2::tinyxml2
              OpenSSL::SSL
              OpenSSL::Crypto
              rt)

  set(AIMRT_FASTDDS_SELECTED_TARGET
      aimrt_fastdds_selected
      PARENT_SCOPE)
  set(AIMRT_FASTDDS_SELECTION_SOURCE
      "${selection_source}"
      PARENT_SCOPE)
  set(AIMRT_FASTDDS_PREFIX
      "${isolated_install}"
      PARENT_SCOPE)
  set(AIMRT_FASTDDS_RUNTIME_LIBRARY_PATH
      "${isolated_fastdds_library}"
      PARENT_SCOPE)
  set(AIMRT_FASTDDS_FASTCDR_TARGET
      aimrt_fastdds_isolated_fastcdr
      PARENT_SCOPE)
  set(AIMRT_FASTDDS_SELECTED_FASTCDR_VERSION
      "${bundled_fastcdr_version}"
      PARENT_SCOPE)
  set(AIMRT_FASTDDS_ISOLATED_SOURCE_BUILD
      TRUE
      PARENT_SCOPE)
  set(AIMRT_FASTDDS_ISOLATED_FOONATHAN_DIR
      "${foonathan_memory_DIR}"
      PARENT_SCOPE)
  set(fastdds_VERSION
      "${source_version}"
      PARENT_SCOPE)
endfunction()

function(_aimrt_fastdds_read_prefix_metadata prefix output_config output_version output_library)
  set(config_candidates "${prefix}/share/fastdds/cmake/fastdds-config.cmake" "${prefix}/lib/cmake/fastdds/fastdds-config.cmake"
                        "${prefix}/lib64/cmake/fastdds/fastdds-config.cmake")
  set(package_config "")
  foreach(config_candidate IN LISTS config_candidates)
    if(EXISTS "${config_candidate}")
      set(package_config "${config_candidate}")
      break()
    endif()
  endforeach()
  if(NOT package_config)
    message(FATAL_ERROR "FastDDS_ROOT does not contain a supported fastdds-config.cmake: '${prefix}'")
  endif()

  file(READ "${package_config}" package_config_content)
  string(REGEX MATCH "set\\([ \\t\\r\\n]*fastdds_VERSION[ \\t\\r\\n]+\"?([0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?)" version_match "${package_config_content}")
  set(package_version "${CMAKE_MATCH_1}")

  set(library_candidates "${prefix}/lib/libfastdds.so.3.6" "${prefix}/lib64/libfastdds.so.3.6" "${prefix}/lib/libfastdds.dylib" "${prefix}/bin/fastdds.dll"
                         "${prefix}/lib/libfastdds.a" "${prefix}/lib64/libfastdds.a")
  set(runtime_library "")
  foreach(library_candidate IN LISTS library_candidates)
    if(EXISTS "${library_candidate}")
      set(runtime_library "${library_candidate}")
      break()
    endif()
  endforeach()
  if(NOT runtime_library)
    file(
      GLOB fallback_libraries
      LIST_DIRECTORIES FALSE
      "${prefix}/lib/libfastdds.so.*" "${prefix}/lib64/libfastdds.so.*")
    list(SORT fallback_libraries)
    if(fallback_libraries)
      list(GET fallback_libraries 0 runtime_library)
    endif()
  endif()
  if(NOT runtime_library)
    message(FATAL_ERROR "FastDDS_ROOT does not contain a Fast DDS 3.6 shared or static library: '${prefix}'")
  endif()
  if(NOT EXISTS "${prefix}/include/fastdds")
    message(FATAL_ERROR "FastDDS_ROOT does not contain Fast DDS headers below include/fastdds: '${prefix}'")
  endif()

  set(${output_config}
      "${package_config}"
      PARENT_SCOPE)
  set(${output_version}
      "${package_version}"
      PARENT_SCOPE)
  set(${output_library}
      "${runtime_library}"
      PARENT_SCOPE)
endfunction()

function(_aimrt_fastdds_path_is_approved path selected_prefix allowlist output_var)
  _aimrt_fastdds_path_is_under("${path}" "${selected_prefix}" path_is_selected)
  if(path_is_selected)
    set(${output_var}
        TRUE
        PARENT_SCOPE)
    return()
  endif()
  foreach(approved_prefix IN LISTS allowlist)
    _aimrt_fastdds_path_is_under("${path}" "${approved_prefix}" path_is_allowlisted)
    if(path_is_allowlisted)
      set(${output_var}
          TRUE
          PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${output_var}
      FALSE
      PARENT_SCOPE)
endfunction()

function(_aimrt_fastdds_read_elf_soname library_path output_var)
  if(NOT library_path
     OR NOT EXISTS "${library_path}"
     OR library_path MATCHES "[.]a$")
    set(${output_var}
        ""
        PARENT_SCOPE)
    return()
  endif()
  if(CMAKE_READELF)
    set(readelf_executable "${CMAKE_READELF}")
  else()
    find_program(readelf_executable NAMES readelf llvm-readelf)
  endif()
  if(NOT readelf_executable)
    set(${output_var}
        ""
        PARENT_SCOPE)
    return()
  endif()
  execute_process(
    COMMAND "${readelf_executable}" -d "${library_path}"
    RESULT_VARIABLE readelf_result
    OUTPUT_VARIABLE readelf_output
    ERROR_VARIABLE readelf_error)
  if(NOT readelf_result EQUAL 0)
    set(${output_var}
        ""
        PARENT_SCOPE)
    return()
  endif()
  string(REGEX MATCH "Library soname: \\[([^]]+)\\]" soname_match "${readelf_output}")
  set(${output_var}
      "${CMAKE_MATCH_1}"
      PARENT_SCOPE)
endfunction()

if(TARGET fastdds)
  message(FATAL_ERROR "GetFastDDS.cmake must be the only Fast DDS selection entry; target 'fastdds' already exists.")
endif()

set(AIMRT_FASTDDS_SELECTION_SOURCE "")
set(AIMRT_FASTDDS_PREFIX "")
set(AIMRT_FASTDDS_SELECTED_TARGET "fastdds")

if(FastDDS_LOCAL_SOURCE)
  if(NOT IS_ABSOLUTE "${FastDDS_LOCAL_SOURCE}" OR NOT EXISTS "${FastDDS_LOCAL_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "FastDDS_LOCAL_SOURCE must be an absolute Fast DDS source directory containing CMakeLists.txt: '${FastDDS_LOCAL_SOURCE}'")
  endif()
  _aimrt_fastdds_parse_source_version("${FastDDS_LOCAL_SOURCE}" fastdds_source_version)
  set(fastdds_VERSION "${fastdds_source_version}")
  set(AIMRT_FASTDDS_SELECTION_SOURCE local_source)
  set(AIMRT_FASTDDS_PREFIX "${FastDDS_LOCAL_SOURCE}")
  FetchContent_Declare(
    fastdds
    SOURCE_DIR "${FastDDS_LOCAL_SOURCE}"
    OVERRIDE_FIND_PACKAGE)
  if(AIMRT_BUILD_WITH_ROS2 AND AIMRT_BUILD_ROS2_PLUGIN)
    FetchContent_GetProperties(fastdds)
    if(NOT fastdds_POPULATED)
      FetchContent_Populate(fastdds)
    endif()
    _aimrt_fastdds_add_isolated_source("${FastDDS_LOCAL_SOURCE}" "${fastdds_source_version}" local_source)
  else()
    set(AIMRT_FASTDDS_PREFIX "${FastDDS_LOCAL_SOURCE}")
    FetchContent_MakeAvailable(fastdds)
  endif()
elseif(FastDDS_ROOT)
  if(NOT IS_ABSOLUTE "${FastDDS_ROOT}" OR NOT EXISTS "${FastDDS_ROOT}")
    message(FATAL_ERROR "FastDDS_ROOT must be an existing absolute install prefix: '${FastDDS_ROOT}'")
  endif()
  # Do not include the installed Fast DDS export set here. ROS 2 Jazzy's
  # fastrtps export defines eProsima_atomic, while Fast DDS 3.6 exports the
  # same target together with fastdds. Including that partially overlapping
  # export set makes an otherwise valid dual-plugin configuration impossible.
  # A private imported target also prevents the DDS targets from accidentally
  # resolving the ROS 2 fastrtps target by name.
  _aimrt_fastdds_read_prefix_metadata("${FastDDS_ROOT}" fastdds_config_file fastdds_VERSION fastdds_runtime_library)
  if(NOT TARGET fastcdr)
    find_package(
      fastcdr
      QUIET
      CONFIG
      PATHS
      "${FastDDS_ROOT}"
      "${FastDDS_ROOT}/lib/cmake/fastcdr"
      "${FastDDS_ROOT}/lib64/cmake/fastcdr"
      NO_DEFAULT_PATH)
    if(NOT fastcdr_FOUND)
      find_package(fastcdr REQUIRED CONFIG)
    endif()
  endif()
  if(NOT TARGET foonathan_memory)
    find_package(
      foonathan_memory
      QUIET
      CONFIG
      PATHS
      "${FastDDS_ROOT}"
      "${FastDDS_ROOT}/lib/cmake/foonathan_memory"
      "${FastDDS_ROOT}/lib64/cmake/foonathan_memory"
      NO_DEFAULT_PATH)
    if(NOT foonathan_memory_FOUND)
      find_package(foonathan_memory REQUIRED CONFIG)
    endif()
  endif()
  if(fastdds_runtime_library MATCHES "[.]a$")
    if(NOT TARGET eProsima_atomic)
      add_library(eProsima_atomic INTERFACE IMPORTED)
    endif()
    find_package(OpenSSL REQUIRED)
    find_package(tinyxml2 REQUIRED CONFIG)
    add_library(aimrt_fastdds_selected STATIC IMPORTED GLOBAL)
    set_target_properties(
      aimrt_fastdds_selected
      PROPERTIES IMPORTED_LOCATION "${fastdds_runtime_library}"
                 INTERFACE_INCLUDE_DIRECTORIES "${FastDDS_ROOT}/include"
                 INTERFACE_LINK_LIBRARIES "fastcdr;foonathan_memory;eProsima_atomic;-lpthread;dl;tinyxml2::tinyxml2;OpenSSL::SSL;OpenSSL::Crypto;rt")
  else()
    add_library(aimrt_fastdds_selected SHARED IMPORTED GLOBAL)
    set_target_properties(
      aimrt_fastdds_selected
      PROPERTIES IMPORTED_LOCATION "${fastdds_runtime_library}"
                 INTERFACE_COMPILE_DEFINITIONS "FASTDDS_DYN_LINK"
                 INTERFACE_INCLUDE_DIRECTORIES "${FastDDS_ROOT}/include"
                 INTERFACE_LINK_LIBRARIES "fastcdr;foonathan_memory")
  endif()
  set(AIMRT_FASTDDS_SELECTED_TARGET aimrt_fastdds_selected)
  set(AIMRT_FASTDDS_SELECTION_SOURCE explicit_prefix)
  set(AIMRT_FASTDDS_PREFIX "${FastDDS_ROOT}")
else()
  find_package(fastdds QUIET CONFIG)
  if(fastdds_FOUND)
    set(AIMRT_FASTDDS_SELECTION_SOURCE cmake_prefix_path)
    if(fastdds_DIR)
      cmake_path(GET fastdds_DIR PARENT_PATH fastdds_cmake_parent)
      cmake_path(GET fastdds_cmake_parent PARENT_PATH fastdds_share_parent)
      cmake_path(GET fastdds_share_parent PARENT_PATH AIMRT_FASTDDS_PREFIX)
    endif()
  else()
    set(AIMRT_FASTDDS_SELECTION_SOURCE default_reproducible_source)
    if(FastDDS_DEFAULT_SOURCE_MIRROR)
      if(NOT IS_ABSOLUTE "${FastDDS_DEFAULT_SOURCE_MIRROR}" OR NOT EXISTS "${FastDDS_DEFAULT_SOURCE_MIRROR}/CMakeLists.txt")
        message(FATAL_ERROR "FastDDS_DEFAULT_SOURCE_MIRROR must be an absolute Fast DDS source directory: '${FastDDS_DEFAULT_SOURCE_MIRROR}'")
      endif()
      FetchContent_Declare(
        fastdds
        SOURCE_DIR "${FastDDS_DEFAULT_SOURCE_MIRROR}"
        OVERRIDE_FIND_PACKAGE)
    else()
      FetchContent_Declare(
        fastdds
        GIT_REPOSITORY
        "${FastDDS_DEFAULT_GIT_REPOSITORY}"
        GIT_TAG
        "${FastDDS_DEFAULT_GIT_TAG}"
        GIT_SHALLOW
        FALSE
        OVERRIDE_FIND_PACKAGE)
    endif()
    if(FastDDS_DEFAULT_SOURCE_MIRROR)
      _aimrt_fastdds_parse_source_version("${FastDDS_DEFAULT_SOURCE_MIRROR}" fastdds_source_version)
      set(fastdds_VERSION "${fastdds_source_version}")
      set(fastdds_source_dir "${FastDDS_DEFAULT_SOURCE_MIRROR}")
    else()
      FetchContent_GetProperties(fastdds)
      if(NOT fastdds_POPULATED)
        FetchContent_Populate(fastdds)
      endif()
      FetchContent_GetProperties(fastdds SOURCE_DIR fastdds_source_dir)
      _aimrt_fastdds_parse_source_version("${fastdds_source_dir}" fastdds_source_version)
      set(fastdds_VERSION "${fastdds_source_version}")
    endif()
    if(AIMRT_BUILD_WITH_ROS2 AND AIMRT_BUILD_ROS2_PLUGIN)
      _aimrt_fastdds_add_isolated_source("${fastdds_source_dir}" "${fastdds_source_version}" default_reproducible_source)
    else()
      set(AIMRT_FASTDDS_PREFIX "${fastdds_source_dir}")
      FetchContent_MakeAvailable(fastdds)
    endif()
  endif()
endif()

if(NOT TARGET ${AIMRT_FASTDDS_SELECTED_TARGET})
  message(FATAL_ERROR "The selected Fast DDS dependency did not define the required private dependency target (source=${AIMRT_FASTDDS_SELECTION_SOURCE}).")
endif()
if(NOT fastdds_VERSION)
  message(FATAL_ERROR "The selected Fast DDS dependency did not report fastdds_VERSION; AimRT requires a verifiable Fast DDS 3.6.x package.")
endif()
if(NOT fastdds_VERSION MATCHES "^3\\.6\\.")
  message(FATAL_ERROR "AIMRT_BUILD_WITH_DDS requires Fast DDS 3.6.x, found '${fastdds_VERSION}' from '${AIMRT_FASTDDS_SELECTION_SOURCE}'.")
endif()

get_target_property(AIMRT_FASTDDS_TARGET_TYPE ${AIMRT_FASTDDS_SELECTED_TARGET} TYPE)
if(NOT AIMRT_FASTDDS_ISOLATED_SOURCE_BUILD)
  _aimrt_fastdds_target_location(${AIMRT_FASTDDS_SELECTED_TARGET} AIMRT_FASTDDS_RUNTIME_LIBRARY_PATH)
endif()
set(AIMRT_FASTDDS_LINK_MODE shared)
set(AIMRT_FASTDDS_ROS2_ISOLATION_MODE not_applicable)
if(AIMRT_FASTDDS_TARGET_TYPE STREQUAL "STATIC_LIBRARY" OR AIMRT_FASTDDS_ISOLATED_SOURCE_BUILD)
  set(AIMRT_FASTDDS_LINK_MODE static_hidden)
endif()

get_target_property(fastdds_interface_links ${AIMRT_FASTDDS_SELECTED_TARGET} INTERFACE_LINK_LIBRARIES)
if(fastdds_interface_links MATCHES "(^|;)fastrtps(::[^;]+)?(;|$)")
  message(FATAL_ERROR "The selected Fast DDS target transitively links legacy fastrtps; DDS plugin isolation requires Fast DDS 3.6.x without libfastrtps.so.2.14 in its closure.")
endif()

if(AIMRT_BUILD_WITH_ROS2 AND AIMRT_BUILD_ROS2_PLUGIN)
  _aimrt_fastdds_path_is_under("${AIMRT_FASTDDS_RUNTIME_LIBRARY_PATH}" "/opt/ros" fastdds_from_ros)
  if(fastdds_from_ros)
    message(
      FATAL_ERROR
        "Unsafe DDS/ROS2 dependency closure: resolved library=fastdds, resolved path='${AIMRT_FASTDDS_RUNTIME_LIBRARY_PATH}', expected prefix='${AIMRT_FASTDDS_PREFIX}', reason=Fast DDS 3.6.x resolved from a ROS prefix. Remediation: set FastDDS_ROOT/FastDDS_LOCAL_SOURCE to an isolated 3.6.x dependency."
    )
  endif()

  set(approved_eprosima_closure "${AIMRT_FASTDDS_PREFIX}")
  if(AIMRT_FASTDDS_EPROSIMA_ALLOWLIST)
    foreach(approved_prefix IN LISTS AIMRT_FASTDDS_EPROSIMA_ALLOWLIST)
      if(NOT IS_ABSOLUTE "${approved_prefix}" OR NOT EXISTS "${approved_prefix}")
        message(FATAL_ERROR "AIMRT_FASTDDS_EPROSIMA_ALLOWLIST entries must be existing absolute prefixes: '${approved_prefix}'")
      endif()
      list(APPEND approved_eprosima_closure "${approved_prefix}")
    endforeach()
  endif()

  if(NOT AIMRT_FASTDDS_FASTCDR_TARGET)
    set(AIMRT_FASTDDS_FASTCDR_TARGET fastcdr)
  endif()
  if(NOT AIMRT_FASTDDS_SELECTED_FASTCDR_VERSION)
    set(AIMRT_FASTDDS_SELECTED_FASTCDR_VERSION "${fastcdr_VERSION}")
  endif()

  if(TARGET ${AIMRT_FASTDDS_FASTCDR_TARGET})
    _aimrt_fastdds_target_location(${AIMRT_FASTDDS_FASTCDR_TARGET} fastcdr_runtime_path)
    get_filename_component(fastcdr_runtime_name "${fastcdr_runtime_path}" NAME)
    _aimrt_fastdds_read_elf_soname("${fastcdr_runtime_path}" fastcdr_runtime_soname)
    set(fastcdr_compatibility_proven FALSE)
    if(AIMRT_FASTDDS_SELECTED_FASTCDR_VERSION MATCHES "^2\\." AND (fastcdr_runtime_soname STREQUAL "libfastcdr.so.2" OR fastcdr_runtime_name STREQUAL "libfastcdr.a"))
      set(fastcdr_compatibility_proven TRUE)
    endif()
    _aimrt_fastdds_path_is_approved("${fastcdr_runtime_path}" "${AIMRT_FASTDDS_PREFIX}" "${AIMRT_FASTDDS_EPROSIMA_ALLOWLIST}" fastcdr_approved)
    if(NOT fastcdr_approved)
      message(
        FATAL_ERROR
          "Unsafe DDS/ROS2 dependency closure: resolved library=fastcdr, resolved path='${fastcdr_runtime_path}', expected prefix='${AIMRT_FASTDDS_PREFIX}' or AIMRT_FASTDDS_EPROSIMA_ALLOWLIST, reason=dependency is outside the approved closure, version='${AIMRT_FASTDDS_SELECTED_FASTCDR_VERSION}', soname='${fastcdr_runtime_soname}', compatible_soname='${fastcdr_compatibility_proven}'. Remediation: provide a complete isolated Fast DDS 3.6.x prefix, add an explicitly reviewed prefix to AIMRT_FASTDDS_EPROSIMA_ALLOWLIST, or use a static-hidden dependency build."
      )
    endif()
    _aimrt_fastdds_path_is_under("${fastcdr_runtime_path}" "${AIMRT_FASTDDS_PREFIX}" fastcdr_is_selected)
    if(NOT fastcdr_is_selected AND NOT fastcdr_compatibility_proven)
      message(
        FATAL_ERROR
          "Unsafe DDS/ROS2 dependency closure: resolved library=fastcdr, resolved path='${fastcdr_runtime_path}', expected prefix='${AIMRT_FASTDDS_PREFIX}' or AIMRT_FASTDDS_EPROSIMA_ALLOWLIST, reason=external eProsima ABI compatibility is unproven, version='${AIMRT_FASTDDS_SELECTED_FASTCDR_VERSION}', soname='${fastcdr_runtime_soname}', compatible_soname='${fastcdr_compatibility_proven}'. Remediation: provide Fast CDR 2.x with ELF SONAME libfastcdr.so.2 from an explicitly reviewed prefix, use a complete isolated Fast DDS 3.6.x prefix, or use a static-hidden dependency build."
      )
    endif()
    if(AIMRT_FASTDDS_STRICT_EPROSIMA_PREFIX)
      if(NOT fastcdr_is_selected)
        message(
          FATAL_ERROR
            "Unsafe DDS/ROS2 dependency closure: resolved library=fastcdr, resolved path='${fastcdr_runtime_path}', expected prefix='${AIMRT_FASTDDS_PREFIX}', reason=strict selected-prefix mode rejects allowlisted external dependencies. Remediation: provide a complete isolated Fast DDS 3.6.x prefix or disable AIMRT_FASTDDS_STRICT_EPROSIMA_PREFIX after review."
        )
      endif()
    endif()
  else()
    message(
      FATAL_ERROR
        "Unsafe DDS/ROS2 dependency closure: resolved library=fastcdr, resolved path='<missing target>', expected prefix='${AIMRT_FASTDDS_PREFIX}' or AIMRT_FASTDDS_EPROSIMA_ALLOWLIST, reason=dependency closure cannot be proven. Remediation: provide a complete isolated Fast DDS 3.6.x prefix or an explicitly reviewed dependency prefix."
    )
  endif()

  if(TARGET foonathan_memory)
    _aimrt_fastdds_target_location(foonathan_memory foonathan_memory_path)
    _aimrt_fastdds_path_is_approved("${foonathan_memory_path}" "${AIMRT_FASTDDS_PREFIX}" "${AIMRT_FASTDDS_EPROSIMA_ALLOWLIST}" foonathan_memory_approved)
    if(AIMRT_FASTDDS_ISOLATED_SOURCE_BUILD)
      # The nested build is configured with this exact package directory, so
      # the selected static archive and the main target share one proven
      # foonathan_memory dependency rather than resolving it opportunistically.
      set(foonathan_memory_approved TRUE)
    endif()
    if(foonathan_memory_path
       AND NOT foonathan_memory_path MATCHES "^<build-tree:"
       AND NOT foonathan_memory_approved)
      message(
        FATAL_ERROR
          "Unsafe DDS/ROS2 dependency closure: resolved library=foonathan_memory, resolved path='${foonathan_memory_path}', expected prefix='${AIMRT_FASTDDS_PREFIX}' or AIMRT_FASTDDS_EPROSIMA_ALLOWLIST, reason=dependency is outside the approved closure. Remediation: provide a complete isolated Fast DDS 3.6.x prefix, add an explicitly reviewed prefix to AIMRT_FASTDDS_EPROSIMA_ALLOWLIST, or use a static-hidden dependency build."
      )
    endif()
    if(AIMRT_FASTDDS_STRICT_EPROSIMA_PREFIX
       AND foonathan_memory_path
       AND NOT foonathan_memory_path MATCHES "^<build-tree:")
      _aimrt_fastdds_path_is_under("${foonathan_memory_path}" "${AIMRT_FASTDDS_PREFIX}" foonathan_memory_is_selected)
      if(NOT foonathan_memory_is_selected)
        message(
          FATAL_ERROR
            "Unsafe DDS/ROS2 dependency closure: resolved library=foonathan_memory, resolved path='${foonathan_memory_path}', expected prefix='${AIMRT_FASTDDS_PREFIX}', reason=strict selected-prefix mode rejects allowlisted external dependencies. Remediation: provide a complete isolated Fast DDS 3.6.x prefix or disable AIMRT_FASTDDS_STRICT_EPROSIMA_PREFIX after review."
        )
      endif()
    endif()
  endif()

  set(AIMRT_FASTDDS_ROS2_ISOLATION_MODE shared_approved_closure)

  if(AIMRT_FASTDDS_LINK_MODE STREQUAL "static_hidden")
    set(AIMRT_FASTDDS_ROS2_ISOLATION_MODE static_hidden)
  endif()
endif()

set(AIMRT_FASTDDS_FASTCDR_RUNTIME_LIBRARY_PATH "${fastcdr_runtime_path}")
set(AIMRT_FASTDDS_FASTCDR_VERSION "${AIMRT_FASTDDS_SELECTED_FASTCDR_VERSION}")
set(AIMRT_FASTDDS_FASTCDR_SONAME "${fastcdr_runtime_soname}")
set(AIMRT_FASTDDS_FASTCDR_COMPATIBILITY_PROVEN "${fastcdr_compatibility_proven}")

add_library(aimrt_fastdds INTERFACE)
add_library(aimrt::deps::fastdds ALIAS aimrt_fastdds)
target_link_libraries(
  aimrt_fastdds
  INTERFACE "$<BUILD_INTERFACE:${AIMRT_FASTDDS_SELECTED_TARGET}>"
            "$<INSTALL_INTERFACE:fastdds>")
if(AIMRT_FASTDDS_LINK_MODE STREQUAL "static_hidden" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(aimrt_fastdds INTERFACE "LINKER:--exclude-libs,libfastdds.a")
endif()
if(AIMRT_INSTALL)
  set_property(TARGET aimrt_fastdds PROPERTY EXPORT_NAME deps::fastdds)
  install(TARGETS aimrt_fastdds EXPORT ${INSTALL_CONFIG_NAME})
endif()

set(AIMRT_FASTDDS_SELECTION_SOURCE
    "${AIMRT_FASTDDS_SELECTION_SOURCE}"
    CACHE INTERNAL "Selected Fast DDS source kind." FORCE)
set(AIMRT_FASTDDS_PREFIX
    "${AIMRT_FASTDDS_PREFIX}"
    CACHE INTERNAL "Selected Fast DDS source/install prefix." FORCE)
set(AIMRT_FASTDDS_VERSION
    "${fastdds_VERSION}"
    CACHE INTERNAL "Selected Fast DDS version." FORCE)
set(AIMRT_FASTDDS_DEFAULT_REPRODUCIBLE_COMMIT
    "${FastDDS_DEFAULT_GIT_TAG}"
    CACHE INTERNAL "Default reproducible Fast DDS commit." FORCE)
set(AIMRT_FASTDDS_PRIVATE_TARGET
    "aimrt::deps::fastdds"
    CACHE INTERNAL "AimRT-private Fast DDS dependency target." FORCE)
set(AIMRT_FASTDDS_LINK_MODE
    "${AIMRT_FASTDDS_LINK_MODE}"
    CACHE INTERNAL "Selected Fast DDS link mode." FORCE)
set(AIMRT_FASTDDS_RUNTIME_LIBRARY_PATH
    "${AIMRT_FASTDDS_RUNTIME_LIBRARY_PATH}"
    CACHE INTERNAL "Selected Fast DDS runtime library path." FORCE)
set(AIMRT_FASTDDS_ROS2_ISOLATION_MODE
    "${AIMRT_FASTDDS_ROS2_ISOLATION_MODE}"
    CACHE INTERNAL "DDS/ROS2 dependency isolation mode." FORCE)
set(AIMRT_FASTDDS_FASTCDR_RUNTIME_LIBRARY_PATH
    "${AIMRT_FASTDDS_FASTCDR_RUNTIME_LIBRARY_PATH}"
    CACHE INTERNAL "Resolved Fast CDR runtime library path used by the DDS dependency closure." FORCE)
set(AIMRT_FASTDDS_FASTCDR_VERSION
    "${AIMRT_FASTDDS_FASTCDR_VERSION}"
    CACHE INTERNAL "Resolved Fast CDR version used by the DDS dependency closure." FORCE)
set(AIMRT_FASTDDS_FASTCDR_SONAME
    "${AIMRT_FASTDDS_FASTCDR_SONAME}"
    CACHE INTERNAL "ELF SONAME of the resolved Fast CDR shared library." FORCE)
set(AIMRT_FASTDDS_FASTCDR_COMPATIBILITY_PROVEN
    "${AIMRT_FASTDDS_FASTCDR_COMPATIBILITY_PROVEN}"
    CACHE INTERNAL "Whether an external Fast CDR ABI has a compatible 2.x version and SONAME." FORCE)

message(STATUS "DDS Fast DDS selection source: ${AIMRT_FASTDDS_SELECTION_SOURCE}")
message(STATUS "DDS Fast DDS compatibility window: 3.6.x")
message(STATUS "DDS Fast DDS version: ${fastdds_VERSION}")
message(STATUS "DDS Fast DDS default reproducible commit: ${FastDDS_DEFAULT_GIT_TAG}")
message(STATUS "DDS Fast DDS private target: aimrt::deps::fastdds")
message(STATUS "DDS Fast DDS link mode: ${AIMRT_FASTDDS_LINK_MODE}")
message(STATUS "DDS Fast DDS runtime library path: ${AIMRT_FASTDDS_RUNTIME_LIBRARY_PATH}")
message(STATUS "DDS Fast DDS ROS2 isolation mode: ${AIMRT_FASTDDS_ROS2_ISOLATION_MODE}")
message(STATUS "DDS Fast CDR runtime library path: ${AIMRT_FASTDDS_FASTCDR_RUNTIME_LIBRARY_PATH}")
message(STATUS "DDS Fast CDR version: ${AIMRT_FASTDDS_FASTCDR_VERSION}")
message(STATUS "DDS Fast CDR SONAME: ${AIMRT_FASTDDS_FASTCDR_SONAME}")
message(STATUS "DDS Fast CDR compatibility proven: ${AIMRT_FASTDDS_FASTCDR_COMPATIBILITY_PROVEN}")
