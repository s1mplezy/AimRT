from build_variant_support import arguments, cache_value, configure, run


def main() -> None:
    args = arguments()
    build_dir = configure(
        args,
        ["-DAIMRT_BUILD_WITH_DDS=OFF", "-DAIMRT_BUILD_DDS_PLUGIN=OFF"],
    )
    if cache_value(build_dir, "AIMRT_BUILD_WITH_DDS") != "OFF":
        raise AssertionError("AIMRT_BUILD_WITH_DDS=OFF was not preserved")
    if cache_value(build_dir, "AIMRT_BUILD_DDS_PLUGIN") != "OFF":
        raise AssertionError("AIMRT_BUILD_DDS_PLUGIN=OFF was not preserved")
    targets = run([args.cmake, "--build", str(build_dir), "--target", "help"])
    if "aimrt_dds_" in targets:
        raise AssertionError(f"DDS targets exist while the plugin is disabled: \n{targets}")
    run([args.cmake, "--build", str(build_dir), "--target",
        "aimrt_interface_aimrt_module_cpp_interface_test", "--parallel", "1"])


if __name__ == "__main__":
    main()
