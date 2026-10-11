#!/usr/bin/env python3
"""Stage a bounded LLVM-MinGW DLL closure for Windows tests and packages."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
from typing import Callable, Iterable


DLL_NAME = re.compile(r"[A-Za-z0-9_+.-]+\.dll", re.IGNORECASE)
GCC_RUNTIME = re.compile(
    r"(?:libgcc_s|libstdc\+\+|libssp|libgomp|libatomic|libquadmath)[^/]*\.dll",
    re.IGNORECASE,
)
SYSTEM_DLLS = {
    "advapi32.dll",
    "bcrypt.dll",
    "cfgmgr32.dll",
    "comctl32.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "d3d12.dll",
    "d3dcompiler_47.dll",
    "dnsapi.dll",
    "dwmapi.dll",
    "dxgi.dll",
    "gdi32.dll",
    "hid.dll",
    "imagehlp.dll",
    "imm32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "msvcrt.dll",
    "mswsock.dll",
    "ncrypt.dll",
    "normaliz.dll",
    "ole32.dll",
    "oleaut32.dll",
    "powrprof.dll",
    "psapi.dll",
    "rpcrt4.dll",
    "secur32.dll",
    "setupapi.dll",
    "shell32.dll",
    "shlwapi.dll",
    "user32.dll",
    "ucrtbase.dll",
    "uxtheme.dll",
    "version.dll",
    "winmm.dll",
    "wintrust.dll",
    "wldap32.dll",
    "ws2_32.dll",
    "wsock32.dll",
}
MAX_RUNTIME_DLLS = 256


class StageError(RuntimeError):
    """Raised when a Windows runtime closure cannot be proven complete."""


def parse_imports(output: str) -> set[str]:
    """Parse LLVM COFF import blocks, including delay-loaded DLLs."""
    if not re.search(r"^Format:\s+COFF-x86-64\s*$", output, re.MULTILINE):
        raise StageError("LLVM inspector did not identify an x86-64 COFF image")
    imports: set[str] = set()
    block = None
    for line in output.splitlines():
        stripped = line.strip()
        if stripped in {"Import {", "DelayImport {"}:
            if block is not None:
                raise StageError("nested LLVM COFF import block")
            block = []
        elif block is not None:
            if stripped == "}":
                if len(block) != 1 or DLL_NAME.fullmatch(block[0]) is None:
                    raise StageError("LLVM COFF import block has an invalid DLL name")
                imports.add(block[0])
                block = None
            elif stripped.startswith("Name:"):
                block.append(stripped.removeprefix("Name:").strip())
    if block is not None:
        raise StageError("unterminated LLVM COFF import block")
    return imports


def imported_dlls(readobj: str, binary: Path) -> set[str]:
    result = subprocess.run(
        [readobj, "--coff-imports", str(binary)],
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
    return parse_imports(result.stdout)


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
        if GCC_RUNTIME.fullmatch(normalized):
            raise StageError(f"GCC runtime DLL in LLVM runtime directory: {candidate.name}")
        if normalized in index:
            raise StageError(f"duplicate case-insensitive runtime DLL: {candidate.name}")
        index[normalized] = candidate
    return index


def stage_runtime(
    binaries: Iterable[Path],
    runtime_dir: Path | Iterable[Path],
    output_dir: Path,
    inspector: Callable[[Path], set[str]],
    *,
    dlls_only: bool = False,
) -> list[str]:
    sources: dict[str, Path] = {}
    runtime_dirs = [runtime_dir] if isinstance(runtime_dir, Path) else runtime_dir
    for directory in runtime_dirs:
        for name, source in runtime_index(directory).items():
            if name in sources and sources[name] != source:
                raise StageError(f"duplicate case-insensitive runtime DLL: {source.name}")
            sources[name] = source
    if output_dir.exists():
        if output_dir.is_symlink() or not output_dir.is_dir():
            raise StageError(f"output directory is not a normal directory: {output_dir}")
        if next(output_dir.iterdir(), None) is not None:
            raise StageError(f"output directory is not empty: {output_dir}")
    else:
        output_dir.mkdir(parents=True)
    queue: list[Path] = []
    staged: dict[str, Path] = {}

    def stage(source: Path, copy: bool = True) -> None:
        if source.is_symlink() or not source.is_file():
            raise StageError(f"runtime input is not a normal file: {source}")
        normalized = source.name.lower()
        if GCC_RUNTIME.fullmatch(normalized):
            raise StageError(f"GCC runtime DLL in LLVM closure: {source.name}")
        previous = staged.get(normalized)
        if previous is not None:
            if previous != source:
                raise StageError(f"duplicate staged filename: {source.name}")
            return
        if copy:
            destination = output_dir / source.name
            shutil.copyfile(source, destination)
        staged[normalized] = source
        queue.append(source)

    for binary in binaries:
        stage(binary, copy=not dlls_only)

    position = 0
    runtime_count = 0
    while position < len(queue):
        binary = queue[position]
        position += 1
        for dependency in sorted(inspector(binary), key=str.lower):
            if DLL_NAME.fullmatch(dependency) is None:
                raise StageError(f"invalid imported DLL name for {binary.name}: {dependency}")
            normalized = dependency.lower()
            if GCC_RUNTIME.fullmatch(normalized):
                raise StageError(f"GCC runtime DLL in LLVM closure: {dependency}")
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
    parser.add_argument("--readobj", required=True)
    parser.add_argument("--runtime-dir", type=Path, action="append", default=[])
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--dlls-only", action="store_true")
    parser.add_argument("--inspect-only", action="store_true")
    parser.add_argument("--require-import", action="append", default=[])
    parser.add_argument("binaries", nargs="+", type=Path)
    arguments = parser.parse_args()
    try:
        if arguments.require_import or arguments.inspect_only:
            for binary in arguments.binaries:
                imports = {name.lower() for name in imported_dlls(arguments.readobj, binary)}
                for required in arguments.require_import:
                    if required.lower() not in imports:
                        raise StageError(f"{binary.name} does not import required DLL: {required}")
        if arguments.inspect_only:
            return 0
        if not arguments.runtime_dir or arguments.output_dir is None:
            raise StageError("staging requires --runtime-dir and --output-dir")
        staged = stage_runtime(
            arguments.binaries,
            arguments.runtime_dir,
            arguments.output_dir,
            lambda binary: imported_dlls(arguments.readobj, binary),
            dlls_only=arguments.dlls_only,
        )
    except (OSError, StageError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Windows runtime staging failed: {error}\n")
    print(json.dumps({"staged": staged}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
