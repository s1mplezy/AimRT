from __future__ import annotations

from pathlib import Path

from build_variant_support import arguments, cache_value, configure, run


def main() -> None:
    args = arguments()
    build_dir = configure(
        args,
        ["-DAIMRT_BUILD_DDS_PLUGIN=OFF", "-DAIMRT_BUILD_EXAMPLES=OFF"],
    )
    expected = {
        "FETCHCONTENT_SOURCE_DIR_LIBUNIFEX": args.libunifex_source,
        "FETCHCONTENT_SOURCE_DIR_FMT": args.fmt_source,
        "FETCHCONTENT_SOURCE_DIR_GOOGLETEST": args.googletest_source,
        "asio_LOCAL_SOURCE": args.asio_source,
        "gflags_LOCAL_SOURCE": args.gflags_source,
        "yaml-cpp_LOCAL_SOURCE": args.yaml_cpp_source,
        "tbb_LOCAL_SOURCE": args.tbb_source,
        "backward_LOCAL_SOURCE": args.backward_source,
    }
    for name, source in expected.items():
        actual = Path(cache_value(build_dir, name)).resolve()
        if actual != Path(source).resolve():
            raise AssertionError(
                f"preseeded dependency source changed for {name}: {actual} != {source}"
            )
    run(
        [
            args.cmake,
            "--build",
            str(build_dir),
            "--target",
            "aimrt_common_util_test",
            "--parallel",
            "4",
        ]
    )


if __name__ == "__main__":
    main()
