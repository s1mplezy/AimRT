from __future__ import annotations

import argparse
from pathlib import Path
import subprocess


def run(command: list[str]) -> None:
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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build-dir", required=True)
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    run(
        [
            args.cmake,
            "--build",
            str(build_dir),
            "--target",
            "aimrt_dds_rpc_codegen_positive_test",
            "--parallel",
            "2",
        ]
    )
    generated = build_dir / "src/tools/aimrt_dds_rpc_gen/positive-generated"
    required = (
        generated / "Positive.h",
        generated / "Positive.cc",
        generated / "Shared.h",
        generated / "Shared.cc",
        generated / "detail/Positive/Positive.hpp",
        generated / "detail/Shared/Shared.hpp",
    )
    for path in required:
        if not path.is_file():
            raise AssertionError(f"positive imported-IDL output is missing: {path}")
    if (generated / "Positive.hpp").exists() or (generated / "Shared.hpp").exists():
        raise AssertionError("native generated header leaked into the public generated root")
    run([str(build_dir / "aimrt_dds_rpc_codegen_positive_test")])


if __name__ == "__main__":
    main()
