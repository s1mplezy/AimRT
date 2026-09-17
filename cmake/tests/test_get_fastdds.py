#!/usr/bin/env python3

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys


def write_fake_shared_library(path: pathlib.Path, soname: str) -> None:
    compiler = shutil.which("cc")
    if compiler is None:
        raise RuntimeError("a C compiler is required to create the Fast CDR SONAME fixture")
    path.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [compiler, "-shared", "-x", "c", f"-Wl,-soname,{soname}", "-o", str(path), "-"],
        input="int aimrt_fastcdr_fixture(void) { return 0; }\n",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"failed to create Fast CDR SONAME fixture:\n{result.stdout}")


def write_fake_package(
    prefix: pathlib.Path,
    version: str | None,
    fastdds_location: pathlib.Path,
    fastcdr_location: pathlib.Path,
    fastcdr_version: str = "2.2.7",
) -> None:
    config_dir = prefix / "lib" / "cmake" / "fastdds"
    config_dir.mkdir(parents=True, exist_ok=True)
    (prefix / "include" / "fastdds").mkdir(parents=True, exist_ok=True)
    fastdds_location.parent.mkdir(parents=True, exist_ok=True)
    fastdds_location.touch()
    if not fastcdr_location.exists():
        fastcdr_location.parent.mkdir(parents=True, exist_ok=True)
        fastcdr_location.touch()
    foonathan_location = prefix / "lib" / "libfoonathan_memory.a"
    foonathan_location.touch()

    fastcdr_config_dir = prefix / "lib" / "cmake" / "fastcdr"
    fastcdr_config_dir.mkdir(parents=True, exist_ok=True)
    (fastcdr_config_dir / "fastcdr-config.cmake").write_text(
        f'''set(fastcdr_FOUND TRUE)\nset(fastcdr_VERSION "{fastcdr_version}")\nadd_library(fastcdr SHARED IMPORTED)\nset_target_properties(fastcdr PROPERTIES IMPORTED_LOCATION "{fastcdr_location}")\n''',
        encoding="utf-8",
    )
    foonathan_config_dir = prefix / "lib" / "cmake" / "foonathan_memory"
    foonathan_config_dir.mkdir(parents=True, exist_ok=True)
    (foonathan_config_dir / "foonathan_memory-config.cmake").write_text(
        f'''set(foonathan_memory_FOUND TRUE)\nadd_library(foonathan_memory STATIC IMPORTED)\nset_target_properties(foonathan_memory PROPERTIES IMPORTED_LOCATION "{foonathan_location}")\n''',
        encoding="utf-8",
    )
    version_line = f'set(fastdds_VERSION "{version}")\n' if version is not None else ""
    config = f"""{version_line}
set(fastdds_FOUND TRUE)
set(fastdds_DIR \"{config_dir}\")
set(fastcdr_VERSION \"{fastcdr_version}\")
add_library(fastcdr SHARED IMPORTED)
set_target_properties(fastcdr PROPERTIES IMPORTED_LOCATION \"{fastcdr_location}\")
add_library(fastdds SHARED IMPORTED)
set_target_properties(fastdds PROPERTIES
  IMPORTED_LOCATION \"{fastdds_location}\"
  INTERFACE_LINK_LIBRARIES \"fastcdr\")
"""
    (config_dir / "fastdds-config.cmake").write_text(config, encoding="utf-8")


def write_fake_source(source: pathlib.Path, version: str) -> None:
    source.mkdir(parents=True, exist_ok=True)
    bundled_fastcdr = source / "thirdparty/fastcdr"
    bundled_fastcdr.mkdir(parents=True)
    (bundled_fastcdr / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.24)\nproject(fastcdr VERSION "2.3.5" LANGUAGES CXX)\n',
        encoding="utf-8",
    )
    (source / "CMakeLists.txt").write_text(
        f'cmake_minimum_required(VERSION 3.24)\nproject(fastdds VERSION "{version}" LANGUAGES CXX)\nadd_library(fastdds INTERFACE)\n',
        encoding="utf-8",
    )


def configure(
    cmake: str,
    fixture: pathlib.Path,
    build: pathlib.Path,
    module_dir: pathlib.Path,
    extra_args: list[str],
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            cmake,
            "-S",
            str(fixture),
            "-B",
            str(build),
            f"-DAIMRT_GET_FASTDDS_MODULE_DIR={module_dir}",
            *extra_args,
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def require(result: subprocess.CompletedProcess[str], success: bool, expected_text: str) -> None:
    if (result.returncode == 0) != success or expected_text not in result.stdout:
        raise RuntimeError(
            f"unexpected configure result (expected success={success}, text={expected_text!r}):\n{result.stdout}"
        )


def is_below(path: pathlib.Path, prefix: pathlib.Path) -> bool:
    try:
        path.resolve().relative_to(prefix.resolve())
        return True
    except ValueError:
        return False


def verify_real_dual_plugin_artifacts(args: argparse.Namespace) -> None:
    dds_plugin = args.dds_plugin.resolve()
    ros2_plugin = args.ros2_plugin.resolve()
    for plugin in (dds_plugin, ros2_plugin):
        if not plugin.is_file():
            raise RuntimeError(f"dual-plugin artifact is missing: {plugin}")

    readelf = subprocess.run(
        [str(args.readelf), "-d", str(dds_plugin)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if readelf.returncode != 0:
        raise RuntimeError(f"readelf failed for DDS plugin:\n{readelf.stdout}")
    if "libfastrtps.so.2.14" in readelf.stdout:
        raise RuntimeError(f"DDS plugin directly needs ROS 2 Fast-RTPS:\n{readelf.stdout}")
    if args.link_mode == "shared":
        require(readelf, True, "Shared library: [libfastdds.so.3.6]")
    elif "libfastdds.so" in readelf.stdout:
        raise RuntimeError(f"static-hidden DDS plugin still has a dynamic Fast DDS dependency:\n{readelf.stdout}")

    approved_prefixes = [args.selected_prefix.resolve(), *(prefix.resolve() for prefix in args.allow_prefix)]
    runpaths: list[pathlib.Path] = []
    for match in re.finditer(r"\((?:RUNPATH|RPATH)\).*?\[([^]]+)\]", readelf.stdout):
        for raw_path in match.group(1).split(":"):
            expanded = raw_path.replace("${ORIGIN}", str(dds_plugin.parent)).replace("$ORIGIN", str(dds_plugin.parent))
            runpath = pathlib.Path(expanded)
            if not runpath.is_absolute():
                raise RuntimeError(f"DDS plugin has an unprovable relative RUNPATH entry {raw_path!r}:\n{readelf.stdout}")
            runpaths.append(runpath.resolve())
    for runpath in runpaths:
        if not any(is_below(runpath, prefix) for prefix in approved_prefixes):
            raise RuntimeError(f"DDS plugin RUNPATH {runpath} is outside approved closure {approved_prefixes}:\n{readelf.stdout}")

    ldd = subprocess.run(
        [str(args.ldd), str(dds_plugin)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if ldd.returncode != 0:
        raise RuntimeError(f"ldd failed for DDS plugin:\n{ldd.stdout}")
    if "libfastrtps.so.2.14" in ldd.stdout:
        raise RuntimeError(f"DDS plugin dependency closure contains ROS 2 Fast-RTPS:\n{ldd.stdout}")

    resolved: dict[str, pathlib.Path] = {}
    for line in ldd.stdout.splitlines():
        fields = line.strip().split()
        if len(fields) >= 3 and fields[1] == "=>" and fields[2].startswith("/"):
            resolved[fields[0]] = pathlib.Path(fields[2])
    fastdds_path = resolved.get("libfastdds.so.3.6")
    if args.link_mode == "shared":
        if fastdds_path is None or not is_below(fastdds_path, args.selected_prefix):
            raise RuntimeError(
                f"libfastdds.so.3.6 did not resolve below selected prefix {args.selected_prefix}:\n{ldd.stdout}"
            )
    elif any(name.startswith("libfastdds.so") for name in resolved):
        raise RuntimeError(f"static-hidden DDS plugin resolves a dynamic Fast DDS library:\n{ldd.stdout}")

    fastcdr_entries = {name: path for name, path in resolved.items() if name.startswith("libfastcdr.so")}
    unexpected_fastcdr = sorted(name for name in fastcdr_entries if name != "libfastcdr.so.2")
    if unexpected_fastcdr:
        raise RuntimeError(f"DDS plugin resolves incompatible Fast CDR SONAMEs {unexpected_fastcdr}:\n{ldd.stdout}")
    fastcdr_path = fastcdr_entries.get("libfastcdr.so.2")
    if args.fastcdr_runtime.name != "libfastcdr.a" and fastcdr_path is None:
        raise RuntimeError(f"DDS plugin does not resolve required libfastcdr.so.2:\n{ldd.stdout}")
    if fastcdr_path is not None:
        if not re.fullmatch(r"2(?:\..*)?", args.fastcdr_version):
            raise RuntimeError(f"external Fast CDR version is not ABI-compatible 2.x: {args.fastcdr_version}")
        if fastcdr_path.resolve() != args.fastcdr_runtime.resolve():
            raise RuntimeError(
                f"libfastcdr.so.2 resolved to {fastcdr_path}, not configured runtime {args.fastcdr_runtime}:\n{ldd.stdout}"
            )
        if not any(is_below(fastcdr_path, prefix) for prefix in approved_prefixes):
            raise RuntimeError(f"libfastcdr.so.2 is outside approved closure {approved_prefixes}:\n{ldd.stdout}")

    for name, dependency_path in resolved.items():
        if name.startswith(("libfastdds", "libfastcdr", "libfoonathan_memory")) and not any(
            is_below(dependency_path, prefix) for prefix in approved_prefixes
        ):
            raise RuntimeError(f"eProsima dependency {name} is outside approved closure {approved_prefixes}:\n{ldd.stdout}")

    if args.link_mode == "static_hidden":
        dyn_symbols = subprocess.run(
            [str(args.readelf), "--dyn-syms", "--wide", "--demangle", str(dds_plugin)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if dyn_symbols.returncode != 0:
            raise RuntimeError(f"dynamic-symbol inspection failed:\n{dyn_symbols.stdout}")
        for line in dyn_symbols.stdout.splitlines():
            fields = line.split()
            if len(fields) >= 8 and fields[6] != "UND" and any(
                marker in line
                for marker in (
                    "eprosima::fastdds",
                    "eprosima::fastcdr",
                    "_ZN8eprosima7fastdds",
                    "_ZN8eprosima7fastcdr",
                )
            ):
                raise RuntimeError(f"static-hidden DDS plugin exports a Fast DDS public ABI symbol:\n{line}")

    loader_code = "import ctypes, os, sys; mode=os.RTLD_NOW|os.RTLD_LOCAL; ctypes.CDLL(sys.argv[1], mode=mode); ctypes.CDLL(sys.argv[2], mode=mode)"
    for first, second in ((ros2_plugin, dds_plugin), (dds_plugin, ros2_plugin)):
        loaded = subprocess.run(
            [sys.executable, "-c", loader_code, str(first), str(second)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if loaded.returncode != 0:
            raise RuntimeError(f"dual-plugin dlopen failed for order {first.name},{second.name}:\n{loaded.stdout}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--module-dir", type=pathlib.Path, required=True)
    parser.add_argument("--fixture", type=pathlib.Path, required=True)
    parser.add_argument("--work-dir", type=pathlib.Path, required=True)
    parser.add_argument("--mode", choices=("selection", "isolation"), required=True)
    parser.add_argument("--dds-plugin", type=pathlib.Path)
    parser.add_argument("--ros2-plugin", type=pathlib.Path)
    parser.add_argument("--readelf", type=pathlib.Path)
    parser.add_argument("--ldd", type=pathlib.Path)
    parser.add_argument("--link-mode", choices=("shared", "static_hidden"))
    parser.add_argument("--selected-prefix", type=pathlib.Path)
    parser.add_argument("--fastcdr-runtime", type=pathlib.Path)
    parser.add_argument("--fastcdr-version")
    parser.add_argument("--allow-prefix", type=pathlib.Path, action="append", default=[])
    args = parser.parse_args()

    shutil.rmtree(args.work_dir, ignore_errors=True)
    args.work_dir.mkdir(parents=True)

    if args.mode == "selection":
        good_prefix = args.work_dir / "prefix-3.6"
        write_fake_package(
            good_prefix,
            "3.6.9.1",
            good_prefix / "lib" / "libfastdds.so.3.6",
            good_prefix / "lib" / "libfastcdr.so.2",
        )
        result = configure(
            args.cmake,
            args.fixture,
            args.work_dir / "build-prefix",
            args.module_dir,
            [f"-DFastDDS_ROOT={good_prefix}"],
        )
        require(result, True, "DDS Fast DDS version: 3.6.9.1")
        report = (args.work_dir / "build-prefix" / "get-fastdds-report.txt").read_text(encoding="utf-8")
        if "source=explicit_prefix" not in report or "private_target=aimrt::deps::fastdds" not in report:
            raise RuntimeError(f"incorrect prefix report:\n{report}")

        static_prefix = args.work_dir / "prefix-3.6-static"
        write_fake_package(
            static_prefix,
            "3.6.9.1",
            static_prefix / "lib" / "libfastdds.a",
            static_prefix / "lib" / "libfastcdr.so.2",
        )
        result = configure(
            args.cmake,
            args.fixture,
            args.work_dir / "build-prefix-static",
            args.module_dir,
            [f"-DFastDDS_ROOT={static_prefix}"],
        )
        require(result, True, "DDS Fast DDS link mode: static_hidden")
        report = (args.work_dir / "build-prefix-static" / "get-fastdds-report.txt").read_text(encoding="utf-8")
        if "link_mode=static_hidden" not in report:
            raise RuntimeError(f"incorrect static-prefix report:\n{report}")

        bad_prefix = args.work_dir / "prefix-3.7"
        write_fake_package(
            bad_prefix,
            "3.7.0",
            bad_prefix / "lib" / "libfastdds.so.3.7",
            bad_prefix / "lib" / "libfastcdr.so.2",
        )
        require(
            configure(
                args.cmake,
                args.fixture,
                args.work_dir / "build-3.7",
                args.module_dir,
                [f"-DFastDDS_ROOT={bad_prefix}"],
            ),
            False,
            "requires Fast DDS 3.6.x",
        )

        missing_prefix = args.work_dir / "prefix-missing-version"
        write_fake_package(
            missing_prefix,
            None,
            missing_prefix / "lib" / "libfastdds.so.3.6",
            missing_prefix / "lib" / "libfastcdr.so.2",
        )
        require(
            configure(
                args.cmake,
                args.fixture,
                args.work_dir / "build-missing",
                args.module_dir,
                [f"-DFastDDS_ROOT={missing_prefix}"],
            ),
            False,
            "did not report fastdds_VERSION",
        )

        local_source = args.work_dir / "source-3.6"
        write_fake_source(local_source, "3.6.5.0")
        require(
            configure(
                args.cmake,
                args.fixture,
                args.work_dir / "build-source",
                args.module_dir,
                [f"-DFastDDS_LOCAL_SOURCE={local_source}"],
            ),
            True,
            "DDS Fast DDS selection source: local_source",
        )

        mirror_source = args.work_dir / "mirror-3.6"
        write_fake_source(mirror_source, "3.6.2.0")
        result = configure(
            args.cmake,
            args.fixture,
            args.work_dir / "build-mirror",
            args.module_dir,
            [f"-DFastDDS_DEFAULT_SOURCE_MIRROR={mirror_source}"],
        )
        require(result, True, "DDS Fast DDS selection source: default_reproducible_source")
        report = (args.work_dir / "build-mirror" / "get-fastdds-report.txt").read_text(encoding="utf-8")
        if "version=3.6.2.0" not in report:
            raise RuntimeError(f"incorrect reproducible mirror report:\n{report}")
    else:
        isolated_source = args.work_dir / "isolated-source-3.6"
        write_fake_source(isolated_source, "3.6.2.0")
        isolated_result = configure(
            args.cmake,
            args.fixture,
            args.work_dir / "build-isolated-source",
            args.module_dir,
            [
                f"-DFastDDS_DEFAULT_SOURCE_MIRROR={isolated_source}",
                "-DAIMRT_BUILD_WITH_ROS2=ON",
                "-DAIMRT_BUILD_ROS2_PLUGIN=ON",
                "-DAIMRT_GET_FASTDDS_PREDEFINE_EPROSIMA_ATOMIC=ON",
            ],
        )
        require(isolated_result, True, "DDS Fast DDS ROS2 isolation mode: static_hidden")
        isolated_report = (args.work_dir / "build-isolated-source" / "get-fastdds-report.txt").read_text(
            encoding="utf-8"
        )
        for expected in (
            "source=default_reproducible_source",
            "version=3.6.2.0",
            "link_mode=static_hidden",
            "isolation=static_hidden",
            "fastcdr_version=2.3.5",
        ):
            if expected not in isolated_report:
                raise RuntimeError(f"incorrect isolated-source report, missing {expected!r}:\n{isolated_report}")

        safe_prefix = args.work_dir / "fastdds-prefix"
        ros_fastcdr = pathlib.Path("/opt/ros/jazzy/lib/libfastcdr.so.2")
        if not ros_fastcdr.exists():
            ros_fastcdr = args.work_dir / "ros" / "lib" / "libfastcdr.so.2"
            write_fake_shared_library(ros_fastcdr, "libfastcdr.so.2")
        write_fake_package(
            safe_prefix,
            "3.6.2.0",
            safe_prefix / "lib" / "libfastdds.so.3.6",
            ros_fastcdr,
        )

        common = [
            f"-DFastDDS_ROOT={safe_prefix}",
            "-DAIMRT_BUILD_WITH_ROS2=ON",
            "-DAIMRT_BUILD_ROS2_PLUGIN=ON",
            "-DAIMRT_GET_FASTDDS_PREDEFINE_EPROSIMA_ATOMIC=ON",
        ]
        approved_prefix = ros_fastcdr.parents[1]
        require(
            configure(
                args.cmake,
                args.fixture,
                args.work_dir / "build-proven",
                args.module_dir,
                [*common, f"-DAIMRT_FASTDDS_EPROSIMA_ALLOWLIST={approved_prefix}"],
            ),
            True,
            "DDS Fast DDS ROS2 isolation mode: shared_approved_closure",
        )

        result = configure(
            args.cmake,
            args.fixture,
            args.work_dir / "build-unsafe",
            args.module_dir,
            common,
        )
        require(result, False, "Unsafe DDS/ROS2 dependency closure")
        for required in ("resolved library=fastcdr", "expected", "prefix=", "reason=", "Remediation:"):
            if required not in result.stdout:
                raise RuntimeError(f"unsafe diagnostic missing {required!r}:\n{result.stdout}")

        require(
            configure(
                args.cmake,
                args.fixture,
                args.work_dir / "build-strict",
                args.module_dir,
                [
                    *common,
                    f"-DAIMRT_FASTDDS_EPROSIMA_ALLOWLIST={approved_prefix}",
                    "-DAIMRT_FASTDDS_STRICT_EPROSIMA_PREFIX=ON",
                ],
            ),
            False,
            "reason=strict",
        )

        incompatible_prefix = args.work_dir / "incompatible-ros"
        incompatible_fastcdr = incompatible_prefix / "lib" / "libfastcdr.so"
        incompatible_package = args.work_dir / "incompatible-fastdds-prefix"
        write_fake_package(
            incompatible_package,
            "3.6.2.0",
            incompatible_package / "lib" / "libfastdds.so.3.6",
            incompatible_fastcdr,
            "2.2.7",
        )
        write_fake_shared_library(incompatible_fastcdr, "libfastcdr.so.1")
        require(
            configure(
                args.cmake,
                args.fixture,
                args.work_dir / "build-allowlisted-incompatible",
                args.module_dir,
                [
                    f"-DFastDDS_ROOT={incompatible_package}",
                    "-DAIMRT_BUILD_WITH_ROS2=ON",
                    "-DAIMRT_BUILD_ROS2_PLUGIN=ON",
                    "-DAIMRT_GET_FASTDDS_PREDEFINE_EPROSIMA_ATOMIC=ON",
                    f"-DAIMRT_FASTDDS_EPROSIMA_ALLOWLIST={incompatible_prefix}",
                ],
            ),
            False,
            "compatibility is unproven",
        )

        real_artifact_args = (
            args.dds_plugin,
            args.ros2_plugin,
            args.readelf,
            args.ldd,
            args.link_mode,
            args.selected_prefix,
            args.fastcdr_runtime,
            args.fastcdr_version,
        )
        if any(real_artifact_args):
            if not all(real_artifact_args):
                raise RuntimeError("real dual-plugin isolation verification requires all artifact/tool/prefix arguments")
            verify_real_dual_plugin_artifacts(args)


if __name__ == "__main__":
    main()
