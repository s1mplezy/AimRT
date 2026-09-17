from build_variant_support import arguments, assert_codegen_outputs, assert_plugin_example_and_core_smoke, cache_value, configure


def main() -> None:
    args = arguments()
    build_dir = configure(args, [])
    if cache_value(build_dir, "AIMRT_BUILD_WITH_DDS") != "ON":
        raise AssertionError("AIMRT_BUILD_WITH_DDS must default to ON")
    if cache_value(build_dir, "AIMRT_BUILD_DDS_PLUGIN") != "ON":
        raise AssertionError("AIMRT_BUILD_DDS_PLUGIN must default to ON")
    if cache_value(build_dir, "AIMRT_DDS_ENABLE_XTYPES") != "ON":
        raise AssertionError("AIMRT_DDS_ENABLE_XTYPES must default to ON")
    assert_codegen_outputs(args.cmake, build_dir, expect_xtypes=True)
    assert_plugin_example_and_core_smoke(args.cmake, build_dir)


if __name__ == "__main__":
    main()
