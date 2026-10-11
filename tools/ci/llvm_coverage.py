#!/usr/bin/env python3
"""Export native LLVM coverage for the complete Classic binary graph."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

LLVM_VERSION = "23.1.3"
PROFILE_DIRECTORY = "llvm-profiles"


def run(command):
    result = subprocess.run(command, check=True, text=True, capture_output=True)
    # llvm-cov can exit successfully while warning about mismatched functions
    # or stale profiles. Such output is incomplete acceptance evidence.
    if result.stderr.strip():
        raise ValueError(f"{command[0]} reported diagnostics: {result.stderr.strip()}")
    return result.stdout


def verify_tool(tool):
    version = run([tool, "--version"])
    if not re.search(r"(?:[Vv]ersion |LLD )" + re.escape(LLVM_VERSION) + r"(?![\d.])", version):
        raise ValueError(f"{tool} must be LLVM {LLVM_VERSION}")


def profile_directory(build_dir):
    # Never follow a symlink when cleaning task-owned profile output.
    if build_dir.is_symlink() or (build_dir / PROFILE_DIRECTORY).is_symlink():
        raise ValueError("coverage build/profile directory must not be a symlink")
    if not build_dir.is_dir():
        raise ValueError(f"coverage build directory is unavailable: {build_dir}")
    return build_dir / PROFILE_DIRECTORY


def prepare(build_dir):
    directory = profile_directory(build_dir)
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir()
    return str(directory.resolve() / "%m-%p.profraw")


def coverage_objects(build_dir):
    objects = []
    for path in sorted(build_dir.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        relative = path.relative_to(build_dir)
        if "CMakeFiles" in relative.parts or PROFILE_DIRECTORY in relative.parts:
            continue
        # Executables and plugins both carry coverage maps; static libraries
        # are represented by their linked consumers. ELF magic prevents source,
        # archives, scripts or generated files from being mistaken for objects.
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
            if b"__llvm_covmap" in stream.read():
                objects.append(path)
    if not objects:
        raise ValueError("no instrumented executables or shared plugins found")
    return objects


def filter_lcov(report, source_root, component):
    root = source_root.resolve()
    prefixes = [root / component] if component != "all" else [
        root / "client", root / "server", root / "libatrinik"]
    records = []
    for record in report.split("end_of_record"):
        match = re.search(r"^SF:(.+)$", record, re.MULTILINE)
        if not match:
            continue
        path = Path(match.group(1))
        if not path.is_absolute():
            path = root / path
        path = path.resolve()
        for prefix in prefixes:
            if not path.is_relative_to(prefix):
                continue
            relative = path.relative_to(prefix)
            # Production C only. Tests, generated build/dependency trees and
            # bundled upstream sources never contribute to Classic coverage.
            excluded = {"tests", "build", "dependencies", "_deps", "third_party", "third-party"}
            if path.suffix == ".c" and not excluded.intersection(relative.parts):
                if prefix.name == "libatrinik" or relative.parts[0] == "src":
                    records.append(record.strip() + "\nend_of_record\n")
            break
    if not records:
        raise ValueError("coverage export contains no intended production C sources")
    return "".join(records)


def export(build_dir, source_root, component, output, compiler=None):
    if output != "-":
        # A failed collection must never leave an older report to be uploaded.
        Path(output).unlink(missing_ok=True)
    directory = profile_directory(build_dir)
    profiles = sorted(directory.glob("*.profraw"))
    if not profiles or any(path.is_symlink() or path.stat().st_size == 0 for path in profiles):
        raise ValueError("missing, empty or symlinked LLVM raw profiles")
    if compiler is None:
        cache = build_dir / "CMakeCache.txt"
        match = re.search(r"^CMAKE_C_COMPILER:(?:FILEPATH|STRING)=(.+)$", cache.read_text(), re.MULTILINE)
        if not match:
            raise ValueError("coverage build has no recorded C compiler")
        compiler = match.group(1)
    profdata = os.environ.get("LLVM_PROFDATA", "llvm-profdata")
    cov = os.environ.get("LLVM_COV", "llvm-cov")
    for tool in (compiler, profdata, cov):
        verify_tool(tool)
    objects = coverage_objects(build_dir)
    merged = directory / "coverage.profdata"
    run([profdata, "merge", "-sparse", "-o", str(merged), *map(str, profiles)])
    arguments = [cov, "export", "-format=lcov", f"-instr-profile={merged}", str(objects[0])]
    for object_path in objects[1:]:
        arguments += ["-object", str(object_path)]
    report = filter_lcov(run(arguments), source_root, component)
    if output == "-":
        sys.stdout.write(report)
    else:
        Path(output).write_text(report)
    print(f"LLVM coverage: {len(profiles)} profiles, {len(objects)} objects, {component} -> {output}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    reset = commands.add_parser("prepare", help="clear only this build's raw profiles before its tests")
    reset.add_argument("--build-dir", required=True, type=Path)
    emit = commands.add_parser("export", help="merge profiles and export filtered LCOV")
    emit.add_argument("--build-dir", required=True, type=Path)
    emit.add_argument("--source-root", required=True, type=Path)
    emit.add_argument("--component", required=True, choices=("client", "server", "libatrinik", "all"))
    emit.add_argument("--output", required=True)
    emit.add_argument("--compiler", help="exact compiler when the build does not use CMake")
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            print(prepare(args.build_dir))
        else:
            export(args.build_dir, args.source_root, args.component, args.output, args.compiler)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"LLVM coverage failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
