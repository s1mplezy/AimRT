"""Command line entry point for AimRT DDS RPC C++ generation."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys
import tempfile

from .diagnostics import IdlDiagnostic
from .generator import generate
from .loader import load_idl


def _write_if_changed(path: Path, content: str) -> None:
    encoded = content.encode("utf-8")
    if path.is_file() and path.read_bytes() == encoded:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def _escape_depfile(path: str) -> str:
    return path.replace(" ", "\\ ").replace("#", "\\#")


def _write_depfile(path: Path, outputs: list[Path], inputs: list[str]) -> None:
    line = " ".join(_escape_depfile(str(output)) for output in outputs)
    line += ": " + " ".join(_escape_depfile(source) for source in inputs) + "\n"
    _write_if_changed(path, line)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Generate AimRT DDS RPC C++ glue from OMG IDL")
    parser.add_argument("--idl", required=True, help="absolute path to the input IDL")
    parser.add_argument("--output-dir", required=True, help="build-tree output directory")
    parser.add_argument("--include-dir", action="append", default=[], help="IDL include search directory")
    parser.add_argument("--depfile", help="optional Make-style dependency file")
    parser.add_argument("--print-dependencies", action="store_true", help="print the resolved include graph and exit")
    parser.add_argument(
        "--root-only",
        action="store_true",
        help="generate only the requested IDL node while resolving its complete include graph")
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = build_argument_parser().parse_args(argv)
    idl_path = Path(arguments.idl)
    if not idl_path.is_absolute():
        print(f"{idl_path}:1:1: AIMRT_DDS_IDL_E011_INCLUDE: --idl must be an absolute path", file=sys.stderr)
        return 4
    try:
        unit = load_idl(str(idl_path), arguments.include_dir)
        if arguments.print_dependencies:
            for source_path in unit.source_paths:
                print(source_path)
            return 0

        output_dir = Path(arguments.output_dir)
        generated: list[tuple[Path, str]] = []
        outputs: list[Path] = []
        # Parse and render the complete graph before publishing any file. A
        # semantic rejection in any dependency therefore cannot leave a
        # partially updated root or dependency binding behind.
        source_inputs = [str(idl_path)] if arguments.root_only else unit.source_paths
        for source_input in source_inputs:
            source_unit = load_idl(source_input, arguments.include_dir)
            stem, header, source = generate(source_unit, source_input)
            header_path = output_dir / f"{stem}.h"
            source_path = output_dir / f"{stem}.cc"
            generated.extend(((header_path, header), (source_path, source)))
            outputs.extend((header_path, source_path))
        for output_path, content in generated:
            _write_if_changed(output_path, content)
        if arguments.depfile:
            _write_depfile(Path(arguments.depfile), outputs, unit.source_paths)
    except IdlDiagnostic as error:
        print(error, file=sys.stderr)
        return 2
    except (OSError, UnicodeError) as error:
        print(f"{idl_path}:1:1: AIMRT_DDS_IDL_E011_INCLUDE: {error}", file=sys.stderr)
        return 4
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
