#!/usr/bin/env python3
"""Measure clean, warm, incremental, and common-header Classic builds."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time
from typing import Any


ARTIFACT_SUFFIXES = (".o", ".obj", ".gch", ".pch")


def parse_cmake_cache(path: Path) -> dict[str, str]:
    """Return ordinary CMake cache entries without internal metadata."""
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        if ":" not in key_and_type:
            continue
        key, entry_type = key_and_type.rsplit(":", 1)
        if entry_type not in {"INTERNAL", "STATIC"}:
            values[key] = value
    return values


def artifact_snapshot(build_dir: Path) -> dict[Path, int]:
    """Record compiler artifact mtimes beneath a configured build tree."""
    return {
        path: path.stat().st_mtime_ns
        for path in build_dir.rglob("*")
        if path.is_file() and path.suffix in ARTIFACT_SUFFIXES
    }


def changed_artifacts(before: dict[Path, int], after: dict[Path, int]) -> int:
    """Count new or rewritten compiler artifacts."""
    return sum(before.get(path) != mtime for path, mtime in after.items())


def process_group_rss_kib(process_group: int) -> int:
    """Sum resident memory for a Linux process group using procfs."""
    total = 0
    proc = Path("/proc")
    if not proc.is_dir():
        return 0
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text(encoding="utf-8")
            closing_paren = stat.rfind(")")
            if closing_paren < 0:
                continue
            fields = stat[closing_paren + 2 :].split()
            if len(fields) < 3 or int(fields[2]) != process_group:
                continue
            for line in (entry / "status").read_text(encoding="utf-8").splitlines():
                if line.startswith("VmRSS:"):
                    total += int(line.split()[1])
                    break
        except (FileNotFoundError, PermissionError, ProcessLookupError, ValueError):
            continue
    return total


def run_measured(command: list[str], build_dir: Path) -> dict[str, Any]:
    """Run a build while sampling elapsed time and aggregate process RSS."""
    before = artifact_snapshot(build_dir)
    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as output:
        started = time.perf_counter()
        process = subprocess.Popen(
            command,
            stdout=output,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            text=True,
        )
        peak_rss_kib = 0
        while process.poll() is None:
            peak_rss_kib = max(peak_rss_kib, process_group_rss_kib(process.pid))
            time.sleep(0.005)
        peak_rss_kib = max(peak_rss_kib, process_group_rss_kib(process.pid))
        elapsed = time.perf_counter() - started
        output.seek(0)
        build_output = output.read()
    after = artifact_snapshot(build_dir)
    result = {
        "elapsed_seconds": round(elapsed, 6),
        "peak_rss_kib": peak_rss_kib,
        "rebuilt_artifacts": changed_artifacts(before, after),
        "return_code": process.returncode,
        "output_tail": build_output.splitlines()[-20:],
    }
    if process.returncode:
        raise RuntimeError(json.dumps(result, indent=2))
    return result


def touch_for_build(path: Path) -> tuple[int, int]:
    """Advance a source mtime and return the original atime/mtime pair."""
    original = path.stat()
    touched_mtime = max(time.time_ns(), original.st_mtime_ns + 1_000_000_000)
    os.utime(path, ns=(original.st_atime_ns, touched_mtime))
    return original.st_atime_ns, original.st_mtime_ns


def build_command(build_dir: Path, target: str, jobs: int) -> list[str]:
    return [
        "cmake",
        "--build",
        str(build_dir),
        "--target",
        target,
        "--parallel",
        str(jobs),
    ]


def compiler_version(compiler: str) -> str:
    result = subprocess.run(
        [compiler, "--version"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.splitlines()[0]


def measure(args: argparse.Namespace) -> dict[str, Any]:
    build_dir = args.build_dir.resolve()
    source = args.source.resolve()
    header = args.header.resolve()
    cache_path = build_dir / "CMakeCache.txt"
    for path, description in (
        (cache_path, "configured CMake cache"),
        (source, "representative source"),
        (header, "common header"),
    ):
        if not path.is_file():
            raise ValueError(f"{description} does not exist: {path}")

    cache = parse_cmake_cache(cache_path)
    compiler = cache.get("CMAKE_C_COMPILER")
    if not compiler:
        raise ValueError(f"CMAKE_C_COMPILER is absent from {cache_path}")
    command = build_command(build_dir, args.target, args.jobs)
    clean_command = ["cmake", "--build", str(build_dir), "--target", "clean"]
    runs = []
    for run_number in range(1, args.runs + 1):
        subprocess.run(clean_command, check=True, capture_output=True, text=True)
        clean = run_measured(command, build_dir)
        warm = run_measured(command, build_dir)

        source_times = touch_for_build(source)
        try:
            incremental = run_measured(command, build_dir)
        finally:
            os.utime(source, ns=source_times)

        header_times = touch_for_build(header)
        try:
            header_touch = run_measured(command, build_dir)
        finally:
            os.utime(header, ns=header_times)

        runs.append(
            {
                "run": run_number,
                "clean": clean,
                "warm": warm,
                "incremental": incremental,
                "header_touch": header_touch,
            }
        )

    return {
        "schema_version": 1,
        "environment": {
            "platform": platform.platform(),
            "processor": platform.processor(),
            "logical_cpus": os.cpu_count(),
            "jobs": args.jobs,
            "compiler": compiler,
            "compiler_version": compiler_version(compiler),
            "cmake_version": subprocess.run(
                ["cmake", "--version"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.splitlines()[0],
        },
        "configuration": {
            "build_dir": str(build_dir),
            "target": args.target,
            "source": str(source),
            "header": str(header),
            "build_type": cache.get("CMAKE_BUILD_TYPE", ""),
            "precompiled_headers": cache.get("ENABLE_PRECOMPILED_HEADERS", "OFF"),
            "unity_build": cache.get("CMAKE_UNITY_BUILD", "OFF"),
            "coverage": cache.get("ENABLE_COVERAGE", "OFF"),
            "sanitizers": cache.get("ENABLE_SANITIZERS", "OFF"),
            "command": command,
        },
        "runs": runs,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--build-dir", type=Path, required=True)
    result.add_argument("--target", required=True)
    result.add_argument("--source", type=Path, required=True)
    result.add_argument("--header", type=Path, required=True)
    result.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    result.add_argument("--runs", type=int, default=3)
    result.add_argument("--output", type=Path)
    return result


def main() -> int:
    args = parser().parse_args()
    if args.jobs < 1 or args.runs < 1:
        raise SystemExit("--jobs and --runs must be positive")
    try:
        report = measure(args)
    except (OSError, subprocess.SubprocessError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
