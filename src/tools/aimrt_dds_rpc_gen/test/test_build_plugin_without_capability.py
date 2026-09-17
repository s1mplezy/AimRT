from __future__ import annotations

from pathlib import Path
import shutil
import subprocess

from build_variant_support import arguments


def main() -> None:
    args = arguments()
    work_dir = Path(args.work_dir).resolve()
    if work_dir.exists():
        shutil.rmtree(work_dir)
    completed = subprocess.run(
        [
            args.cmake,
            "-S",
            str(Path(args.source_dir).resolve()),
            "-B",
            str(work_dir / "build"),
            "-DAIMRT_BUILD_WITH_DDS=OFF",
            "-DAIMRT_BUILD_DDS_PLUGIN=ON",
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    expected = "AIMRT_BUILD_DDS_PLUGIN=ON requires AIMRT_BUILD_WITH_DDS=ON"
    if completed.returncode == 0 or expected not in completed.stdout:
        raise AssertionError(
            "invalid DDS option combination did not fail with the stable diagnostic:\n"
            + completed.stdout
        )


if __name__ == "__main__":
    main()
