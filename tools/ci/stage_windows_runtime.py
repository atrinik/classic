#!/usr/bin/env python3
"""Stage the bounded MinGW DLL closure for native Windows test executables."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
from typing import Callable, Iterable


DLL_NAME = re.compile(r"^\s*DLL Name:\s*(\S+)\s*$", re.IGNORECASE | re.MULTILINE)
SYSTEM_DLLS = {
    "advapi32.dll",
    "bcrypt.dll",
    "crypt32.dll",
    "dnsapi.dll",
    "gdi32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "msvcrt.dll",
    "ncrypt.dll",
    "normaliz.dll",
    "ole32.dll",
    "rpcrt4.dll",
    "secur32.dll",
    "shell32.dll",
    "user32.dll",
    "version.dll",
    "winmm.dll",
    "wldap32.dll",
    "ws2_32.dll",
    "wsock32.dll",
}
MAX_RUNTIME_DLLS = 256


class StageError(RuntimeError):
    """Raised when a Windows runtime closure cannot be proven complete."""


def imported_dlls(objdump: str, binary: Path) -> set[str]:
    result = subprocess.run(
        [objdump, "-p", str(binary)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=30,
    )
    if result.returncode:
        raise StageError(
            f"cannot inspect {binary}: {result.stderr.strip() or result.returncode}"
        )
    return {match.group(1) for match in DLL_NAME.finditer(result.stdout)}


def is_system_dll(name: str) -> bool:
    normalized = name.lower()
    return (
        normalized in SYSTEM_DLLS
        or normalized.startswith("api-ms-win-")
        or normalized.startswith("ext-ms-win-")
    )


def runtime_index(runtime_dir: Path) -> dict[str, Path]:
    if runtime_dir.is_symlink() or not runtime_dir.is_dir():
        raise StageError(f"runtime directory is not a normal directory: {runtime_dir}")
    index: dict[str, Path] = {}
    for candidate in runtime_dir.iterdir():
        if candidate.is_symlink() or not candidate.is_file() or candidate.suffix.lower() != ".dll":
            continue
        normalized = candidate.name.lower()
        if normalized in index:
            raise StageError(f"duplicate case-insensitive runtime DLL: {candidate.name}")
        index[normalized] = candidate
    return index


def stage_runtime(
    binaries: Iterable[Path],
    runtime_dir: Path,
    output_dir: Path,
    inspector: Callable[[Path], set[str]],
) -> list[str]:
    sources = runtime_index(runtime_dir)
    if output_dir.exists():
        if output_dir.is_symlink() or not output_dir.is_dir():
            raise StageError(f"output directory is not a normal directory: {output_dir}")
        if next(output_dir.iterdir(), None) is not None:
            raise StageError(f"output directory is not empty: {output_dir}")
    else:
        output_dir.mkdir(parents=True)
    queue: list[Path] = []
    staged: dict[str, Path] = {}

    def stage(source: Path) -> None:
        if source.is_symlink() or not source.is_file():
            raise StageError(f"runtime input is not a normal file: {source}")
        normalized = source.name.lower()
        previous = staged.get(normalized)
        if previous is not None:
            if previous != source:
                raise StageError(f"duplicate staged filename: {source.name}")
            return
        destination = output_dir / source.name
        shutil.copyfile(source, destination)
        staged[normalized] = source
        queue.append(source)

    for binary in binaries:
        stage(binary)

    position = 0
    runtime_count = 0
    while position < len(queue):
        binary = queue[position]
        position += 1
        for dependency in sorted(inspector(binary), key=str.lower):
            normalized = dependency.lower()
            if is_system_dll(dependency) or normalized in staged:
                continue
            source = sources.get(normalized)
            if source is None:
                raise StageError(f"unresolved runtime DLL for {binary.name}: {dependency}")
            runtime_count += 1
            if runtime_count > MAX_RUNTIME_DLLS:
                raise StageError("Windows runtime dependency closure is unexpectedly large")
            stage(source)

    return sorted((path.name for path in output_dir.iterdir()), key=str.lower)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("binaries", nargs="+", type=Path)
    arguments = parser.parse_args()
    try:
        staged = stage_runtime(
            arguments.binaries,
            arguments.runtime_dir,
            arguments.output_dir,
            lambda binary: imported_dlls(arguments.objdump, binary),
        )
    except (OSError, StageError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Windows runtime staging failed: {error}\n")
    print(json.dumps({"staged": staged}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
