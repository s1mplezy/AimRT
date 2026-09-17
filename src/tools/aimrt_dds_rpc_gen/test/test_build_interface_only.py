from build_variant_support import arguments, cache_value, configure, run


def main() -> None:
    args = arguments()
    build_dir = configure(
        args,
        [
            "-DAIMRT_BUILD_RUNTIME=OFF",
            "-DAIMRT_BUILD_WITH_DDS=ON",
            "-DAIMRT_BUILD_DDS_PLUGIN=OFF",
        ],
    )
    if cache_value(build_dir, "AIMRT_BUILD_RUNTIME") != "OFF":
        raise AssertionError("AIMRT_BUILD_RUNTIME=OFF was not preserved")
    if cache_value(build_dir, "AIMRT_BUILD_WITH_DDS") != "ON":
        raise AssertionError("AIMRT_BUILD_WITH_DDS=ON was not preserved")
    if cache_value(build_dir, "AIMRT_BUILD_DDS_PLUGIN") != "OFF":
        raise AssertionError("AIMRT_BUILD_DDS_PLUGIN=OFF was not preserved")

    run([args.cmake, "--build", str(build_dir), "--target", "all", "--parallel", "4"])
    targets = run([args.cmake, "--build", str(build_dir), "--target", "help"])
    if "aimrt_plugins_dds_plugin" in targets:
        raise AssertionError("DDS runtime plugin target exists in interface-only mode")
    run(
        [
            "ctest",
            "--test-dir",
            str(build_dir),
            "--output-on-failure",
            "--timeout",
            "120",
            "-R",
            "^(aimrt_dds_header_only_api|aimrt_dds_native_type_interface|aimrt_dds_rpc_codegen_install_consumer)$",
        ]
    )


if __name__ == "__main__":
    main()
