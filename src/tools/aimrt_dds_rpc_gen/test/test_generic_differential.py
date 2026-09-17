from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fastddsgen", required=True)
    parser.add_argument("--rpc-generator", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    work_dir = Path(args.work_dir).resolve()
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)

    invalid = work_dir / "invalid-empty-sequence.idl"
    invalid.write_text(
        "module invalid {\n"
        "  struct Broken { sequence<> values; };\n"
        "};\n",
        encoding="utf-8",
    )
    aimrt_invalid_output = work_dir / "aimrt-invalid"
    aimrt_result = run(
        [args.rpc_generator, "--idl", str(invalid), "--output-dir", str(aimrt_invalid_output)]
    )
    if aimrt_result.returncode == 0:
        raise AssertionError(f"AimRT accepted sequence<>: \n{aimrt_result.stdout}")
    if "AIMRT_DDS_IDL_E001_SYNTAX" not in aimrt_result.stdout or ":2:28:" not in aimrt_result.stdout:
        raise AssertionError(f"AimRT rejection lacked stable code/location: \n{aimrt_result.stdout}")
    if aimrt_invalid_output.exists():
        raise AssertionError("AimRT rejection created an output directory")

    fastdds_invalid_output = work_dir / "fastdds-invalid"
    fastdds_invalid_output.mkdir()
    fastdds_result = run(
        [
            args.fastddsgen,
            "-replace",
            "-flat-output-dir",
            "-d",
            str(fastdds_invalid_output),
            str(invalid),
        ]
    )
    if fastdds_result.returncode == 0:
        raise AssertionError("Fast DDS-Gen unexpectedly accepted sequence<>")

    valid = work_dir / "valid-bounded-sequence.idl"
    valid.write_text(
        "module valid {\n"
        "  struct Request { sequence<long, 8> values; };\n"
        "  struct Response { long value; };\n"
        "  interface Service { Response Call(in Request request); };\n"
        "};\n",
        encoding="utf-8",
    )
    aimrt_valid_output = work_dir / "aimrt-valid"
    aimrt_valid = run(
        [args.rpc_generator, "--idl", str(valid), "--output-dir", str(aimrt_valid_output)]
    )
    if aimrt_valid.returncode != 0:
        raise AssertionError(f"AimRT rejected valid bounded sequence: \n{aimrt_valid.stdout}")
    fastdds_valid_output = work_dir / "fastdds-valid"
    fastdds_valid_output.mkdir()
    fastdds_valid = run(
        [
            args.fastddsgen,
            "-replace",
            "-flat-output-dir",
            "-d",
            str(fastdds_valid_output),
            str(valid),
        ]
    )
    if fastdds_valid.returncode != 0:
        raise AssertionError(f"Fast DDS-Gen rejected valid bounded sequence: \n{fastdds_valid.stdout}")


if __name__ == "__main__":
    main()
