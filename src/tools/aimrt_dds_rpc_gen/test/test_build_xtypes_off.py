from build_variant_support import arguments, assert_codegen_outputs, assert_plugin_example_and_core_smoke, cache_value, configure


def main() -> None:
    args = arguments()
    build_dir = configure(
        args,
        [
            "-DAIMRT_BUILD_WITH_DDS=ON",
            "-DAIMRT_BUILD_DDS_PLUGIN=ON",
            "-DAIMRT_DDS_ENABLE_XTYPES=OFF",
        ],
    )
    if cache_value(build_dir, "AIMRT_DDS_ENABLE_XTYPES") != "OFF":
        raise AssertionError("AIMRT_DDS_ENABLE_XTYPES=OFF was not preserved")
    assert_codegen_outputs(args.cmake, build_dir, expect_xtypes=False)
    assert_plugin_example_and_core_smoke(args.cmake, build_dir)


if __name__ == "__main__":
    main()
