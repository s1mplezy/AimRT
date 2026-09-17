from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import time


ALPHA_V1 = """module sample {
  struct PingRequest { long value; };
  struct PingResponse { long value; };
  interface Alpha { PingResponse Ping(in PingRequest request); };
};
"""

ALPHA_V2 = """module sample {
  struct PingRequest { long value; };
  struct PingResponse { long value; };
  struct PongRequest { long value; };
  struct PongResponse { long value; };
  interface Alpha {
    PingResponse Ping(in PingRequest request);
    PongResponse Pong(in PongRequest request);
  };
};
"""

BETA = """module sample {
  struct EchoRequest { string value; };
  struct EchoResponse { string value; };
  interface Beta { EchoResponse Echo(in EchoRequest request); };
};
"""

SHARED = """module shared { struct Request { long value; }; struct Response { long value; }; };
"""

INCLUDED = """#include "Shared.idl"
module sample { interface Included { shared::Response Call(in shared::Request request); }; };
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


def snapshot(paths: list[Path]) -> dict[Path, tuple[str, int]]:
    return {path: (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns) for path in paths}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--module-dir", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--fastddsgen", required=True)
    parser.add_argument("--rpc-generator", required=True)
    parser.add_argument("--xtypes", required=True)
    parser.add_argument("--aimrt-source", required=True)
    parser.add_argument("--fastdds-prefix", required=True)
    parser.add_argument("--libunifex-source", required=True)
    parser.add_argument("--dependency-prefix", action="append", default=[])
    args = parser.parse_args()

    work_dir = Path(args.work_dir).resolve()
    if work_dir.exists():
        shutil.rmtree(work_dir)
    source_dir = work_dir / "source"
    build_dir = work_dir / "build"
    source_dir.mkdir(parents=True)
    alpha = source_dir / "Alpha.idl"
    beta = source_dir / "Beta.idl"
    shared = source_dir / "Shared.idl"
    included = source_dir / "Included.idl"
    alpha.write_text(ALPHA_V1, encoding="utf-8")
    beta.write_text(BETA, encoding="utf-8")
    shared.write_text(SHARED, encoding="utf-8")
    included.write_text(INCLUDED, encoding="utf-8")
    (source_dir / "included_graph_consumer.cc").write_text(
        """#include "Included.h"

#include <type_traits>

int main() {
  static_assert(aimrt::DdsMessageType<shared::Request>);
  static_assert(std::is_base_of_v<aimrt::rpc::ServiceBase,
                                  sample::IncludedSyncService>);
  return aimrt::GetDdsMessageTypeSupport<shared::Request>() != nullptr ? 0 : 1;
}
""",
        encoding="utf-8",
    )
    prefix_paths = [str(Path(args.fastdds_prefix).resolve())]
    for value in args.dependency_prefix:
        prefix_paths.extend(part for part in value.split(";") if part)
    (source_dir / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.24)
project(aimrt_dds_incremental LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(PREPEND CMAKE_PREFIX_PATH [[{';'.join(dict.fromkeys(prefix_paths))}]])
find_package(fastdds 3.6 REQUIRED CONFIG)
find_package(fmt REQUIRED CONFIG)
set(UNIFEX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(UNIFEX_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory([[{Path(args.libunifex_source).resolve()}]]
  ${{CMAKE_CURRENT_BINARY_DIR}}/_deps/libunifex-build EXCLUDE_FROM_ALL)
add_library(unifex::unifex ALIAS unifex)
list(APPEND CMAKE_MODULE_PATH [[{Path(args.module_dir).resolve()}]])
set(FASTDDSGEN_EXECUTABLE [[{Path(args.fastddsgen).resolve()}]])
set(AIMRT_DDS_ENABLE_XTYPES {args.xtypes})
set_property(GLOBAL PROPERTY AIMRT_DDS_RPC_GEN_COMMAND_PROPERTY [[{Path(args.rpc_generator).resolve()}]])
include(FastDdsGenCode)
aimrt_add_dds_idl_codegen(
  TARGET_NAME incremental_codegen
  IDL_FILES Alpha.idl Beta.idl Included.idl
  INCLUDE_DIRS ${{CMAKE_CURRENT_SOURCE_DIR}}
  OUTPUT_DIR ${{CMAKE_CURRENT_BINARY_DIR}}/generated)
set(INCLUDED_GENERATED_DIR ${{CMAKE_CURRENT_BINARY_DIR}}/generated)
set(INCLUDED_GENERATED_SOURCES
  ${{INCLUDED_GENERATED_DIR}}/detail/Included/IncludedPubSubTypes.cxx
  ${{INCLUDED_GENERATED_DIR}}/detail/Shared/SharedPubSubTypes.cxx
  ${{INCLUDED_GENERATED_DIR}}/Included.cc
  ${{INCLUDED_GENERATED_DIR}}/Shared.cc)
if({args.xtypes})
  list(APPEND INCLUDED_GENERATED_SOURCES
    ${{INCLUDED_GENERATED_DIR}}/detail/Included/IncludedTypeObjectSupport.cxx
    ${{INCLUDED_GENERATED_DIR}}/detail/Shared/SharedTypeObjectSupport.cxx)
endif()
add_executable(included_graph_consumer
  included_graph_consumer.cc ${{INCLUDED_GENERATED_SOURCES}})
add_dependencies(included_graph_consumer incremental_codegen)
target_include_directories(included_graph_consumer PRIVATE
  ${{INCLUDED_GENERATED_DIR}}
  [[{Path(args.aimrt_source).resolve() / 'src/interface'}]]
  [[{Path(args.aimrt_source).resolve() / 'src/common'}]])
target_compile_definitions(included_graph_consumer PRIVATE AIMRT_USE_FMT_LIB)
target_link_libraries(included_graph_consumer PRIVATE
  fastdds fmt::fmt-header-only unifex::unifex)
""",
        encoding="utf-8",
    )

    run([args.cmake, "-S", str(source_dir), "-B", str(build_dir)])
    run([args.cmake, "--build", str(build_dir), "--target", "incremental_codegen", "--parallel", "1"])

    generated = build_dir / "generated"
    alpha_outputs = sorted((generated / "detail/Alpha").glob("Alpha*")) + \
        [generated / "Alpha.h", generated / "Alpha.cc"]
    beta_outputs = sorted((generated / "detail/Beta").glob("Beta*")) + [generated / "Beta.h", generated / "Beta.cc"]
    if not alpha_outputs or not beta_outputs:
        raise AssertionError("both IDLs must produce generated files")
    if not (generated / "Alpha.cc").is_file():
        raise AssertionError("AimRT binding output is missing")
    if not any(path.name == "AlphaPubSubTypes.cxx" for path in alpha_outputs):
        raise AssertionError("Fast DDS output is missing")
    included_output = generated
    for name in (
        "detail/Included/Included.hpp",
        "detail/Shared/Shared.hpp",
        "detail/Shared/SharedPubSubTypes.cxx",
        "Shared.h",
        "Shared.cc",
    ):
        if not (included_output / name).is_file():
            raise AssertionError(f"include graph output is missing: {name}")
    run([args.cmake, "--build", str(build_dir), "--target", "included_graph_consumer", "--parallel", "4"])
    run([str(build_dir / "included_graph_consumer")])

    first = snapshot(alpha_outputs + beta_outputs)
    time.sleep(1.1)
    run([args.cmake, "--build", str(build_dir), "--target", "incremental_codegen", "--parallel", "1"])
    if snapshot(alpha_outputs + beta_outputs) != first:
        raise AssertionError("no-change rebuild modified generated outputs")

    time.sleep(1.1)
    alpha.write_text(ALPHA_V2, encoding="utf-8")
    run([args.cmake, "--build", str(build_dir), "--target", "incremental_codegen", "--parallel", "1"])
    second = snapshot(alpha_outputs + beta_outputs)
    if first[generated / "detail/Alpha/Alpha.hpp"][0] == second[generated / "detail/Alpha/Alpha.hpp"][0]:
        raise AssertionError("Fast DDS output for the changed IDL did not change")
    if first[generated / "Alpha.cc"][0] == second[generated / "Alpha.cc"][0]:
        raise AssertionError("AimRT binding output for the changed IDL did not change")
    for path in beta_outputs:
        if first[path] != second[path]:
            raise AssertionError(f"unrelated IDL output was modified: {path}")

    unexpected = [
        path
        for path in source_dir.rglob("*")
        if path.is_file()
        and path.name != "included_graph_consumer.cc"
        and path.suffix in {".cxx", ".cc", ".hpp", ".h", ".ipp"}
    ]
    if unexpected:
        raise AssertionError(f"generated files escaped into the source tree: {unexpected}")

    source_output_project = work_dir / "source-output-project"
    source_output_project.mkdir()
    (source_output_project / "Invalid.idl").write_text(ALPHA_V1, encoding="utf-8")
    (source_output_project / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.24)
project(aimrt_dds_source_output LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH [[{Path(args.module_dir).resolve()}]])
set(FASTDDSGEN_EXECUTABLE [[{Path(args.fastddsgen).resolve()}]])
set(AIMRT_DDS_ENABLE_XTYPES {args.xtypes})
set_property(GLOBAL PROPERTY AIMRT_DDS_RPC_GEN_COMMAND_PROPERTY [[{Path(args.rpc_generator).resolve()}]])
include(FastDdsGenCode)
aimrt_add_dds_idl_codegen(TARGET_NAME invalid_output IDL_FILES Invalid.idl
  OUTPUT_DIR ${{CMAKE_CURRENT_SOURCE_DIR}}/generated)
""",
        encoding="utf-8",
    )
    run_failed(
        [args.cmake, "-S", str(source_output_project), "-B", str(work_dir / "source-output-build")],
        "must be in the build tree",
    )
    if (source_output_project / "generated").exists():
        raise AssertionError("rejected source-tree output created generated files")

    nested_build_project = work_dir / "nested-build-project"
    nested_build_project.mkdir()
    (nested_build_project / "Valid.idl").write_text(ALPHA_V1, encoding="utf-8")
    (nested_build_project / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.24)
project(aimrt_dds_nested_build_output LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH [[{Path(args.module_dir).resolve()}]])
set(FASTDDSGEN_EXECUTABLE [[{Path(args.fastddsgen).resolve()}]])
set(AIMRT_DDS_ENABLE_XTYPES {args.xtypes})
set_property(GLOBAL PROPERTY AIMRT_DDS_RPC_GEN_COMMAND_PROPERTY [[{Path(args.rpc_generator).resolve()}]])
include(FastDdsGenCode)
aimrt_add_dds_idl_codegen(TARGET_NAME valid_output IDL_FILES Valid.idl
  OUTPUT_DIR ${{CMAKE_CURRENT_BINARY_DIR}}/generated)
""",
        encoding="utf-8",
    )
    run(
        [
            args.cmake,
            "-S",
            str(nested_build_project),
            "-B",
            str(nested_build_project / "build"),
        ]
    )

    outside_output_project = work_dir / "outside-output-project"
    outside_output_project.mkdir()
    (outside_output_project / "Invalid.idl").write_text(ALPHA_V1, encoding="utf-8")
    outside_output_dir = work_dir / "outside-build-output"
    (outside_output_project / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.24)
project(aimrt_dds_outside_build_output LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH [[{Path(args.module_dir).resolve()}]])
set(FASTDDSGEN_EXECUTABLE [[{Path(args.fastddsgen).resolve()}]])
set(AIMRT_DDS_ENABLE_XTYPES {args.xtypes})
set_property(GLOBAL PROPERTY AIMRT_DDS_RPC_GEN_COMMAND_PROPERTY [[{Path(args.rpc_generator).resolve()}]])
include(FastDdsGenCode)
aimrt_add_dds_idl_codegen(TARGET_NAME invalid_output IDL_FILES Invalid.idl
  OUTPUT_DIR [[{outside_output_dir.resolve()}]])
""",
        encoding="utf-8",
    )
    run_failed(
        [args.cmake, "-S", str(outside_output_project), "-B", str(work_dir / "outside-output-build")],
        "must be within the current build tree",
    )
    if outside_output_dir.exists():
        raise AssertionError("rejected outside-build output created generated files")

    collision_project = work_dir / "collision-project"
    (collision_project / "left").mkdir(parents=True)
    (collision_project / "right").mkdir(parents=True)
    duplicate_idl = "module collision { struct Value { long value; }; };\n"
    left_duplicate = collision_project / "left/Duplicate.idl"
    right_duplicate = collision_project / "right/Duplicate.idl"
    left_duplicate.write_text(duplicate_idl, encoding="utf-8")
    right_duplicate.write_text(duplicate_idl, encoding="utf-8")
    (collision_project / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.24)
project(aimrt_dds_collision LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH [[{Path(args.module_dir).resolve()}]])
set(FASTDDSGEN_EXECUTABLE [[{Path(args.fastddsgen).resolve()}]])
set(AIMRT_DDS_ENABLE_XTYPES {args.xtypes})
set_property(GLOBAL PROPERTY AIMRT_DDS_RPC_GEN_COMMAND_PROPERTY [[{Path(args.rpc_generator).resolve()}]])
include(FastDdsGenCode)
aimrt_add_dds_idl_codegen(
  TARGET_NAME colliding_codegen
  IDL_FILES left/Duplicate.idl right/Duplicate.idl)
""",
        encoding="utf-8",
    )
    collision_output = run_failed(
        [args.cmake, "-S", str(collision_project), "-B", str(work_dir / "collision-build")],
        "duplicate stems or colliding output paths",
    )
    left_text = str(left_duplicate.resolve())
    right_text = str(right_duplicate.resolve())
    if left_text not in collision_output or right_text not in collision_output:
        raise AssertionError(f"collision diagnostic omitted canonical paths:\n{collision_output}")
    if collision_output.index(left_text) > collision_output.index(right_text):
        raise AssertionError(f"collision diagnostic paths are not sorted:\n{collision_output}")

    atomic_project = work_dir / "atomic-project"
    atomic_project.mkdir()
    (atomic_project / "Rejected.idl").write_text(ALPHA_V1, encoding="utf-8")
    failing_generator = atomic_project / "failing_rpc_generator.py"
    failing_generator.write_text(
        """import pathlib
import sys

arguments = sys.argv[1:]
if "--print-dependencies" in arguments:
    idl_index = arguments.index("--idl") + 1
    print(pathlib.Path(arguments[idl_index]).resolve())
    raise SystemExit(0)
print("AIMRT_DDS_TEST_INJECTED_RPC_FAILURE", file=sys.stderr)
raise SystemExit(23)
""",
        encoding="utf-8",
    )
    (atomic_project / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.24)
project(aimrt_dds_atomic_rejection LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH [[{Path(args.module_dir).resolve()}]])
set(FASTDDSGEN_EXECUTABLE [[{Path(args.fastddsgen).resolve()}]])
set(AIMRT_DDS_ENABLE_XTYPES {args.xtypes})
set_property(GLOBAL PROPERTY AIMRT_DDS_RPC_GEN_COMMAND_PROPERTY [[{Path(sys.executable).resolve()};{failing_generator.resolve()}]])
include(FastDdsGenCode)
aimrt_add_dds_idl_codegen(TARGET_NAME rejected_codegen IDL_FILES Rejected.idl)
""",
        encoding="utf-8",
    )
    atomic_build = work_dir / "atomic-build"
    run([args.cmake, "-S", str(atomic_project), "-B", str(atomic_build)])
    run_failed(
        [args.cmake, "--build", str(atomic_build), "--target", "rejected_codegen", "--parallel", "1"],
        "AIMRT_DDS_TEST_INJECTED_RPC_FAILURE",
    )
    rejected_output = atomic_build / "generated/dds/rejected_codegen/detail/Rejected"
    if rejected_output.exists():
        raise AssertionError(f"generator rejection created final output directory: {rejected_output}")
    if list(atomic_build.rglob(".aimrt-codegen-staging")):
        raise AssertionError("generator rejection left a staging directory")
    partials = list(atomic_build.rglob("*.hpp")) + list(atomic_build.rglob("*.cxx")) + \
        list(atomic_build.rglob("Rejected.h")) + list(atomic_build.rglob("Rejected.cc"))
    if partials:
        raise AssertionError(f"generator rejection left half-generated outputs: {partials}")


if __name__ == "__main__":
    main()
