from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess


IDL = """module public_header {
  struct Request { long value; };
  struct Response { long value; };
  interface Service { Response Call(in Request request); };
};
"""


def run(command: list[str]) -> str:
    completed = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n{completed.stdout}"
        )
    return completed.stdout


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--fastddsgen", required=True)
    parser.add_argument("--rpc-generator", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--fastdds-prefix", required=True)
    parser.add_argument("--dependency-prefix", action="append", default=[])
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    work = Path(args.work_dir).resolve()
    if work.exists():
        shutil.rmtree(work)
    prefix = work / "install"
    consumer_source = work / "consumer-source"
    generated = consumer_source / "generated"
    native_generated = generated / "detail/PublicHeader"
    consumer_build = work / "consumer-build"
    native_generated.mkdir(parents=True)
    idl = consumer_source / "PublicHeader.idl"
    idl.write_text(IDL, encoding="utf-8")

    # Install the exact primary tree under test. In the interface-only matrix
    # that tree is configured with AIMRT_BUILD_RUNTIME=OFF and
    # AIMRT_BUILD_DDS_PLUGIN=OFF, so this consumer cannot accidentally prove
    # the public API against a separately configured full-runtime build.
    run(
        [
            args.cmake,
            "--install",
            str(Path(args.build_dir).resolve()),
            "--prefix",
            str(prefix),
        ]
    )

    # B05 deliberately uses the build-tree generators and only consumes the
    # installed public C++ targets/headers. B12 independently validates the
    # installed generator and its Python package.
    run(
        [
            args.fastddsgen,
            "-replace",
            "-flat-output-dir",
            "-d",
            str(native_generated),
            str(idl),
        ]
    )
    run(
        [
            args.rpc_generator,
            "--idl",
            str(idl),
            "--output-dir",
            str(generated),
        ]
    )
    for path in native_generated.iterdir():
        if path.suffix not in {".hpp", ".ipp", ".cxx"}:
            continue
        content = path.read_text(encoding="utf-8")
        for basename in (
            "PublicHeader.hpp",
            "PublicHeaderCdrAux.hpp",
            "PublicHeaderCdrAux.ipp",
            "PublicHeaderPubSubTypes.hpp",
            "PublicHeaderTypeObjectSupport.hpp",
        ):
            content = content.replace(
                f'#include "{basename}"',
                f'#include "detail/PublicHeader/{basename}"',
            )
        path.write_text(content, encoding="utf-8")

    (consumer_source / "main.cc").write_text(
        """#include "PublicHeader.h"

#include <type_traits>

int main() {
  static_assert(aimrt::DdsMessageType<public_header::Request>);
  static_assert(std::is_base_of_v<aimrt::rpc::ServiceBase,
                                  public_header::ServiceSyncService>);
  const auto* support = aimrt::GetDdsMessageTypeSupport<public_header::Request>();
  void* message = support->create(support->impl);
  support->destroy(support->impl, message);
  return support->custom_type_support_ptr(support->impl) != nullptr ? 0 : 1;
}
""",
        encoding="utf-8",
    )
    (consumer_source / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.24)
project(aimrt_dds_public_header_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(fastdds 3.6 REQUIRED CONFIG)
find_package(fmt REQUIRED CONFIG)
add_library(std::coroutines INTERFACE IMPORTED)
find_package(unifex REQUIRED CONFIG)
add_library(asio::asio INTERFACE IMPORTED)
add_library(yaml-cpp::yaml-cpp INTERFACE IMPORTED)
add_library(TBB::tbb INTERFACE IMPORTED)
add_library(protobuf::libprotobuf INTERFACE IMPORTED)
# This consumer deliberately uses only the installed DDS interface.  The
# complete AimRT export also contains ROS2/runtime targets, so provide inert
# placeholders for their transitive export references before including it.
add_library(jsoncpp_static INTERFACE IMPORTED)
add_library(ros2_plugin_proto::ros2_plugin_proto__rosidl_generator_cpp INTERFACE IMPORTED)
add_library(ros2_plugin_proto::ros2_plugin_proto__rosidl_typesupport_cpp INTERFACE IMPORTED)
add_library(ros2_plugin_proto::ros2_plugin_proto__rosidl_typesupport_fastrtps_cpp INTERFACE IMPORTED)
add_library(ros2_plugin_proto::ros2_plugin_proto__rosidl_typesupport_introspection_cpp INTERFACE IMPORTED)
 include([[{prefix / 'lib/cmake/aimrt/aimrt-config.cmake'}]])
add_executable(public_header_consumer
  main.cc
  generated/detail/PublicHeader/PublicHeaderPubSubTypes.cxx
  generated/detail/PublicHeader/PublicHeaderTypeObjectSupport.cxx
  generated/PublicHeader.cc)
target_include_directories(public_header_consumer PRIVATE generated)
target_link_libraries(
  public_header_consumer
  PRIVATE aimrt::interface::aimrt_module_dds_interface fastdds)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_options(public_header_consumer PRIVATE --coverage)
endif()
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
    run(
        [
            args.cmake,
            "--build",
            str(consumer_build),
            "--target",
            "public_header_consumer",
            "--parallel",
            "4",
        ]
    )
    run([str(consumer_build / "public_header_consumer")])


if __name__ == "__main__":
    main()
