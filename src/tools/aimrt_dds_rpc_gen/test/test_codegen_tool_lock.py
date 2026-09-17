from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess


def configure(cmake: str, source: Path, build: Path, generator: str,
              java: str, path: str | None = None) -> tuple[int, str]:
    command = [cmake, "-S", str(source), "-B", str(build), f"-DJAVA={java}"]
    if generator:
        command.append(f"-DGENERATOR={generator}")
    environment = os.environ.copy()
    if path:
        environment["PATH"] = path
    completed = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=environment,
    )
    return completed.returncode, completed.stdout


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--module-dir", required=True)
    parser.add_argument("--java", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    work = Path(args.work_dir).resolve()
    if work.exists():
        shutil.rmtree(work)
    source = work / "source"
    source.mkdir(parents=True)
    (source / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.24)
project(aimrt_dds_tool_lock LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH [[{Path(args.module_dir).resolve()}]])
include(FastDdsCodegenTool)
aimrt_find_dds_codegen_tool(FASTDDSGEN_EXECUTABLE "${{GENERATOR}}" OUTPUT_VARIABLE RESOLVED_GENERATOR)
aimrt_validate_dds_codegen_tools(FASTDDSGEN_EXECUTABLE "${{RESOLVED_GENERATOR}}" JAVA_EXECUTABLE "${{JAVA}}")
""",
        encoding="utf-8",
    )

    generator_dir = work / "bin"
    generator_dir.mkdir()
    valid = generator_dir / "fastddsgen"
    wrong = work / "fastddsgen-4.3.1"
    valid.write_text("#!/bin/sh\nprintf '%s\\n' 'fastddsgen version 4.3.0'\n", encoding="utf-8")
    wrong.write_text("#!/bin/sh\nprintf '%s\\n' 'fastddsgen version 4.3.1'\n", encoding="utf-8")
    valid.chmod(0o755)
    wrong.chmod(0o755)

    cases = [
        ("relative", "relative-fastddsgen", False, "must be an absolute path"),
        ("missing", str(work / "missing-fastddsgen"), False, "does not name an executable file"),
        ("wrong", str(wrong), False, "must report exactly version 4.3.0"),
        ("valid", str(valid), True, "DDS Java raw version output"),
        ("discovered", "", True, "discovered from PATH"),
    ]
    for name, generator, should_pass, expected in cases:
        path = f"{generator_dir}{os.pathsep}{os.environ['PATH']}" if name == "discovered" else None
        result, output = configure(args.cmake, source, work /
                                   f"build-{name}", generator, str(Path(args.java).resolve()), path)
        if (result == 0) != should_pass:
            raise AssertionError(f"unexpected configure result for {name}: {result}\n{output}")
        if expected not in output:
            raise AssertionError(f"missing diagnostic/report text for {name}: {expected!r}\n{output}")
        if should_pass:
            for evidence in (str(valid), "fastddsgen version 4.3.0", str(Path(args.java).resolve())):
                if evidence not in output:
                    raise AssertionError(f"missing successful tool-lock evidence {evidence!r}\n{output}")


if __name__ == "__main__":
    main()
