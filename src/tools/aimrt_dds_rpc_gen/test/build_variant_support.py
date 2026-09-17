from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--fastddsgen", required=True)
    parser.add_argument("--fastdds-prefix", required=True)
    parser.add_argument("--dependency-prefix", action="append", default=[])
    parser.add_argument("--libunifex-source", required=True)
    parser.add_argument("--fmt-source", required=True)
    parser.add_argument("--googletest-source", required=True)
    parser.add_argument("--asio-source", required=True)
    parser.add_argument("--gflags-source", required=True)
    parser.add_argument("--yaml-cpp-source", required=True)
    parser.add_argument("--tbb-source", required=True)
    parser.add_argument("--backward-source", required=True)
    return parser.parse_args()


def run(command: list[str]) -> str:
    completed = subprocess.run(command, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode != 0:
        raise AssertionError(f"command failed ({completed.returncode}): {' '.join(command)}\n{completed.stdout}")
    return completed.stdout


def configure(args: argparse.Namespace, dds_options: list[str]) -> Path:
    work_dir = Path(args.work_dir).resolve()
    if work_dir.exists():
        shutil.rmtree(work_dir)
    build_dir = work_dir / "build"
    dependency_metadata_dir = build_dir / "_deps"
    prefix_paths = [str(Path(args.fastdds_prefix).resolve())]
    for value in args.dependency_prefix:
        prefix_paths.extend(part for part in value.split(";") if part)
    command = [
        args.cmake,
        "-S",
        str(Path(args.source_dir).resolve()),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DAIMRT_BUILD_TESTS=ON",
        "-DAIMRT_BUILD_RUNTIME=ON",
        "-DAIMRT_BUILD_PROTOCOLS=OFF",
        "-DAIMRT_BUILD_WITH_PROTOBUF=OFF",
        "-DAIMRT_BUILD_EXAMPLES=ON",
        f"-DFETCHCONTENT_BASE_DIR={dependency_metadata_dir}",
        f"-DCMAKE_PREFIX_PATH={';'.join(dict.fromkeys(prefix_paths))}",
        f"-DFASTDDSGEN_EXECUTABLE={Path(args.fastddsgen).resolve()}",
        f"-DFETCHCONTENT_SOURCE_DIR_LIBUNIFEX={Path(args.libunifex_source).resolve()}",
        f"-DFETCHCONTENT_SOURCE_DIR_FMT={Path(args.fmt_source).resolve()}",
        f"-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST={Path(args.googletest_source).resolve()}",
        f"-Dasio_LOCAL_SOURCE={Path(args.asio_source).resolve()}",
        f"-Dgflags_LOCAL_SOURCE={Path(args.gflags_source).resolve()}",
        f"-Dyaml-cpp_LOCAL_SOURCE={Path(args.yaml_cpp_source).resolve()}",
        f"-Dtbb_LOCAL_SOURCE={Path(args.tbb_source).resolve()}",
        f"-Dbackward_LOCAL_SOURCE={Path(args.backward_source).resolve()}",
        *dds_options,
    ]
    run(command)
    if cache_value(build_dir, "FETCHCONTENT_BASE_DIR") != str(dependency_metadata_dir):
        raise AssertionError("DDS variant dependency metadata escaped its build tree")
    return build_dir


def cache_value(build_dir: Path, name: str) -> str:
    prefix = f"{name}: "
    for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix):
            return line.split("=", 1)[1]
    raise AssertionError(f"cache entry is missing: {name}")


def assert_codegen_outputs(cmake: str, build_dir: Path, expect_xtypes: bool) -> None:
    run([cmake, "--build", str(build_dir), "--target", "aimrt_dds_rpc_codegen_fixture", "--parallel", "1"])
    generated = build_dir / "src/tools/aimrt_dds_rpc_gen/test-generated"
    native = generated / "detail/Calculator"
    required = [
        native / "Calculator.hpp",
        native / "CalculatorPubSubTypes.cxx",
        generated / "Calculator.h",
        generated / "Calculator.cc",
    ]
    for path in required:
        if not path.is_file():
            raise AssertionError(f"expected generated output is missing: {path}")
    type_object = native / "CalculatorTypeObjectSupport.cxx"
    if type_object.is_file() != expect_xtypes:
        raise AssertionError(f"unexpected XTypes output state: {type_object}")


def assert_plugin_example_and_core_smoke(cmake: str, build_dir: Path) -> None:
    run(
        [
            cmake,
            "--build",
            str(build_dir),
            "--target",
            "aimrt_dds_channel_test",
            "aimrt_dds_rpc_test",
            "aimrt_examples_plugins_dds_plugin_build_all",
            "--parallel",
            "8",
        ]
    )
    run(
        [
            "ctest",
            "--test-dir",
            str(build_dir),
            "--output-on-failure",
            "--timeout",
            "120",
            "-R",
            "^aimrt_dds_(channel_roundtrip|rpc_roundtrip)$",
        ]
    )
