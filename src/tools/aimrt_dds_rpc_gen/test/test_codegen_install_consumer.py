from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess


SHARED_IDL = """module shared {
  struct Request { long value; };
  struct Response { long value; };
};
"""

IDL = """#include "Shared.idl"
module installed {
  interface Service { shared::Response Call(in shared::Request request); };
};
"""


def run(command: list[str]) -> str:
    completed = subprocess.run(command, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode != 0:
        raise AssertionError(f"command failed ({completed.returncode}): {' '.join(command)}\n{completed.stdout}")
    return completed.stdout


def run_failed(command: list[str], expected: str) -> str:
    completed = subprocess.run(command, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode == 0 or expected not in completed.stdout:
        raise AssertionError(f"command did not fail as expected: {' '.join(command)}\n{completed.stdout}")
    return completed.stdout


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--fastddsgen", required=True)
    parser.add_argument("--fastdds-prefix", required=True)
    parser.add_argument("--dependency-prefix", action="append", default=[])
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    work = Path(args.work_dir).resolve()
    if work.exists():
        shutil.rmtree(work)
    prefix = work / "install"
    output = work / "generated"
    work.mkdir(parents=True)
    idl = work / "Example.idl"
    idl.write_text(IDL, encoding="utf-8")
    shared_idl = work / "Shared.idl"
    shared_idl.write_text(SHARED_IDL, encoding="utf-8")

    run([args.cmake, "--build", str(Path(args.build_dir).resolve()), "--target", "all", "--parallel", "4"])
    run(
        [
            args.cmake,
            "--install",
            str(Path(args.build_dir).resolve()),
            "--prefix",
            str(prefix),
        ]
    )
    executable = prefix / "bin/aimrt_dds_rpc_gen"
    if not executable.is_file():
        raise AssertionError("installed aimrt_dds_rpc_gen executable is missing")
    run(
        [
            str(executable),
            "--idl",
            str(idl.resolve()),
            "--output-dir",
            str(output),
            "--include-dir",
            str(work),
        ]
    )
    for path in (output / "Example.h", output / "Example.cc", output / "Shared.h", output / "Shared.cc"):
        if not path.is_file():
            raise AssertionError(f"installed generator did not produce {path.name}")
    package_root = prefix / "lib/aimrt/python/aimrt_dds_rpc_gen"
    for module in ("lexer.py", "parser.py", "loader.py", "generator.py", "cli.py"):
        if not (package_root / module).is_file():
            raise AssertionError(f"installed parser package is incomplete: {module}")

    consumer_source = work / "consumer-source"
    consumer_build = work / "consumer-build"
    consumer_source.mkdir()
    shutil.copy2(idl, consumer_source / idl.name)
    shutil.copy2(shared_idl, consumer_source / shared_idl.name)
    (consumer_source / "bare_native_header_probe.cc").write_text(
        '#include "Example.hpp"\nint main() { return 0; }\n',
        encoding="utf-8",
    )
    (consumer_source / "main.cc").write_text(
        """#include "Example.h"

#include <type_traits>

int main() {
  static_assert(aimrt::DdsMessageType<shared::Request>);
  static_assert(std::is_base_of_v<aimrt::rpc::ServiceBase, installed::ServiceSyncService>);
  const auto* type_support = aimrt::GetDdsMessageTypeSupport<shared::Request>();
  void* message = type_support->create(type_support->impl);
  type_support->destroy(type_support->impl, message);
  return 0;
}
""",
        encoding="utf-8",
    )
    (consumer_source / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.24)
project(aimrt_dds_installed_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(fastdds 3.6 REQUIRED CONFIG)
find_package(fmt REQUIRED CONFIG)
add_library(std::coroutines INTERFACE IMPORTED)
find_package(unifex REQUIRED CONFIG)
# The installed export contains runtime targets that this interface-only
# consumer does not use. Provide inert targets so the export integrity check
# does not turn this codegen test into a runtime packaging test.
add_library(asio::asio INTERFACE IMPORTED)
add_library(yaml-cpp::yaml-cpp INTERFACE IMPORTED)
add_library(TBB::tbb INTERFACE IMPORTED)
add_library(protobuf::libprotobuf INTERFACE IMPORTED)
add_library(jsoncpp_static INTERFACE IMPORTED)
add_library(ros2_plugin_proto::ros2_plugin_proto__rosidl_generator_cpp INTERFACE IMPORTED)
add_library(ros2_plugin_proto::ros2_plugin_proto__rosidl_typesupport_cpp INTERFACE IMPORTED)
add_library(ros2_plugin_proto::ros2_plugin_proto__rosidl_typesupport_fastrtps_cpp INTERFACE IMPORTED)
add_library(ros2_plugin_proto::ros2_plugin_proto__rosidl_typesupport_introspection_cpp INTERFACE IMPORTED)
include([[{prefix / 'lib/cmake/aimrt/aimrt-config.cmake'}]])
list(APPEND CMAKE_MODULE_PATH [[{prefix / 'cmake'}]])
set(CMAKE_PROGRAM_PATH [[{prefix / 'bin'}]])
if(NOT FASTDDSGEN_EXECUTABLE)
  set(FASTDDSGEN_EXECUTABLE [[{Path(args.fastddsgen).resolve()}]])
endif()
set(AIMRT_DDS_ENABLE_XTYPES ON)
include(FastDdsGenCode)
add_executable(installed_consumer main.cc)
target_link_libraries(installed_consumer PRIVATE aimrt::interface::aimrt_module_dds_interface fastdds)
target_link_options(installed_consumer PRIVATE --coverage)
aimrt_add_dds_idl_codegen(
  TARGET_NAME installed_consumer_codegen
  IDL_FILES Example.idl
  INCLUDE_DIRS ${{CMAKE_CURRENT_SOURCE_DIR}}
  OUTPUT_DIR ${{CMAKE_CURRENT_BINARY_DIR}}/generated
  ATTACH_TO_TARGET installed_consumer)
add_executable(bare_native_header_probe EXCLUDE_FROM_ALL bare_native_header_probe.cc)
add_dependencies(bare_native_header_probe installed_consumer_codegen)
target_include_directories(
  bare_native_header_probe PRIVATE ${{CMAKE_CURRENT_BINARY_DIR}}/generated)
""",
        encoding="utf-8",
    )
    prefix_paths = [str(prefix), str(Path(args.fastdds_prefix).resolve())]
    for value in args.dependency_prefix:
        prefix_paths.extend(part for part in value.split(";") if part)
    run(
        [
            args.cmake,
            "-S",
            str(consumer_source),
            "-B",
            str(consumer_build),
            f"-DCMAKE_PREFIX_PATH={';'.join(dict.fromkeys(prefix_paths))}",
        ]
    )
    run([args.cmake, "--build", str(consumer_build), "--target", "installed_consumer", "--parallel", "4"])
    run([str(consumer_build / "installed_consumer")])
    generated = consumer_build / "generated"
    required = (
        generated / "Example.h",
        generated / "Example.cc",
        generated / "Shared.h",
        generated / "Shared.cc",
        generated / "detail/Example/Example.hpp",
        generated / "detail/Shared/Shared.hpp",
    )
    for path in required:
        if not path.is_file():
            raise AssertionError(f"installed helper graph output is missing: {path}")
    if (generated / "Example.hpp").exists():
        raise AssertionError("native Example.hpp leaked into the public generated root")
    run_failed(
        [
            args.cmake,
            "--build",
            str(consumer_build),
            "--target",
            "bare_native_header_probe",
            "--parallel",
            "1",
        ],
        "Example.hpp",
    )

    collision_source = work / "collision-source"
    (collision_source / "left").mkdir(parents=True)
    (collision_source / "right").mkdir(parents=True)
    duplicate_idl = "module collision { struct Value { long value; }; };\n"
    left_duplicate = collision_source / "left/Duplicate.idl"
    right_duplicate = collision_source / "right/Duplicate.idl"
    left_duplicate.write_text(duplicate_idl, encoding="utf-8")
    right_duplicate.write_text(duplicate_idl, encoding="utf-8")
    (collision_source / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.24)
project(aimrt_dds_installed_collision LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH [[{prefix / 'cmake'}]])
set(CMAKE_PROGRAM_PATH [[{prefix / 'bin'}]])
set(FASTDDSGEN_EXECUTABLE [[{Path(args.fastddsgen).resolve()}]])
set(AIMRT_DDS_ENABLE_XTYPES ON)
include(FastDdsGenCode)
aimrt_add_dds_idl_codegen(
  TARGET_NAME installed_colliding_codegen
  IDL_FILES left/Duplicate.idl right/Duplicate.idl)
""",
        encoding="utf-8",
    )
    collision_output = run_failed(
        [
            args.cmake,
            "-S",
            str(collision_source),
            "-B",
            str(work / "collision-build"),
        ],
        "duplicate stems or colliding output paths",
    )
    left_text = str(left_duplicate.resolve())
    right_text = str(right_duplicate.resolve())
    if left_text not in collision_output or right_text not in collision_output:
        raise AssertionError(
            f"installed helper collision diagnostic omitted canonical paths:\n{collision_output}"
        )
    if collision_output.index(left_text) > collision_output.index(right_text):
        raise AssertionError(
            f"installed helper collision diagnostic paths are not sorted:\n{collision_output}"
        )

    wrong_generator = work / "fastddsgen-4.3.1"
    wrong_generator.write_text("#!/bin/sh\nprintf '%s\\n' 'fastddsgen version 4.3.1'\n", encoding="utf-8")
    wrong_generator.chmod(0o755)
    run_failed(
        [
            args.cmake,
            "-S",
            str(consumer_source),
            "-B",
            str(work / "consumer-build-wrong-generator"),
            f"-DCMAKE_PREFIX_PATH={';'.join(dict.fromkeys(prefix_paths))}",
            f"-DFASTDDSGEN_EXECUTABLE={wrong_generator}",
        ],
        "must report exactly version 4.3.0",
    )


if __name__ == "__main__":
    main()
