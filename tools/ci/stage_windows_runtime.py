#!/usr/bin/env python3
"""Stage a bounded LLVM-MinGW DLL closure for Windows tests and packages."""

from __future__ import annotations

import argparse
import json
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import stat
import tempfile
from typing import Callable, Iterable


DLL_NAME = re.compile(r"[A-Za-z0-9_+.-]+\.dll", re.IGNORECASE)
IMPORT_NAME = re.compile(r"[A-Za-z0-9_+.-]+\.(?:dll|drv)", re.IGNORECASE)
GCC_RUNTIME = re.compile(
    r"(?:libgcc_s|libstdc\+\+|libssp|libgomp|libatomic|libquadmath)[^/]*\.dll",
    re.IGNORECASE,
)
SYSTEM_DLLS = {
    "advapi32.dll",
    "avrt.dll",
    "bcrypt.dll",
    "cabinet.dll",
    "cfgmgr32.dll",
    "comctl32.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "d2d1.dll",
    "d3d11.dll",
    "d3d12.dll",
    "d3d9.dll",
    "d3dcompiler_47.dll",
    "dbghelp.dll",
    "dinput8.dll",
    "dnsapi.dll",
    "dwmapi.dll",
    "dwrite.dll",
    "dxgi.dll",
    "gdi32.dll",
    "gdiplus.dll",
    "hid.dll",
    "imagehlp.dll",
    "imm32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "ksuser.dll",
    "mf.dll",
    "mfplat.dll",
    "mfreadwrite.dll",
    "mfuuid.dll",
    "msimg32.dll",
    "mswsock.dll",
    "ncrypt.dll",
    "netapi32.dll",
    "netio.dll",
    "normaliz.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleaut32.dll",
    "opengl32.dll",
    "powrprof.dll",
    "propsys.dll",
    "psapi.dll",
    "rpcrt4.dll",
    "runtimeobject.dll",
    "secur32.dll",
    "setupapi.dll",
    "shell32.dll",
    "shlwapi.dll",
    "ucrtbase.dll",
    "uiautomationcore.dll",
    "user32.dll",
    "userenv.dll",
    "usp10.dll",
    "uxtheme.dll",
    "version.dll",
    "winhttp.dll",
    "wininet.dll",
    "winmm.dll",
    "winspool.drv",
    "wintrust.dll",
    "wldap32.dll",
    "ws2_32.dll",
    "wsock32.dll",
    "wtsapi32.dll",
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
    outer_kind = None
    depth = 0
    for line in output.splitlines():
        stripped = line.strip()
        if stripped in {"Import {", "DelayImport {"}:
            if block is None:
                block = []
                outer_kind = stripped
                depth = 1
            elif outer_kind == "DelayImport {" and stripped == "Import {" and depth == 1:
                # LLVM nests delayed symbols inside their DLL's DelayImport.
                depth = 2
            else:
                raise StageError("nested LLVM COFF import block")
        elif block is not None:
            if stripped == "}":
                depth -= 1
                if depth == 0:
                    if len(block) != 1 or IMPORT_NAME.fullmatch(block[0]) is None:
                        raise StageError("LLVM COFF import block has an invalid DLL name")
                    imports.add(block[0])
                    block = None
                    outer_kind = None
            elif stripped.startswith("Name:"):
                if depth != 1:
                    raise StageError("LLVM delayed symbol has an unexpected DLL name")
                block.append(stripped.removeprefix("Name:").strip())
            elif stripped.endswith("{"):
                raise StageError("unknown nested LLVM COFF import block")
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


def is_system_dll(name: str, *, external_python: bool = False) -> bool:
    normalized = name.lower()
    return (
        normalized in SYSTEM_DLLS
        or external_python and normalized == "msvcrt.dll"
        or normalized.endswith(".dll") and normalized.startswith(("api-ms-win-", "ext-ms-win-"))
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



def read_inventory(path: Path) -> dict:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 4 * 1024 * 1024:
        raise StageError(f"invalid Windows inventory file: {path}")

    def unique_keys(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise StageError(f"duplicate Windows inventory key: {key}")
            result[key] = value
        return result

    try:
        result = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_keys)
    except (ValueError, UnicodeError) as error:
        raise StageError(f"invalid Windows inventory JSON: {path}: {error}") from error
    if not isinstance(result, dict):
        raise StageError(f"Windows inventory must be an object: {path}")
    return result


def safe_relative_path(name: str) -> Path:
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_+./-]+", name):
        raise StageError(f"unsafe Windows inventory path: {name}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in {".", ".."} for part in name.split("/")):
        raise StageError(f"unsafe Windows inventory path: {name}")
    return Path(*path.parts)


def copy_snapshot(source: Path, destination: Path, expected: str | None = None,
                  *, max_bytes: int | None = None) -> str:
    """Copy one opened regular file, then bind inspection to those copied bytes."""
    descriptor = os.open(source, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    digest = hashlib.sha256()
    copied_bytes = 0
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise StageError(f"runtime input is not a normal file: {source}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        with os.fdopen(descriptor, "rb", closefd=False) as source_file, destination.open("xb") as output:
            for chunk in iter(lambda: source_file.read(1024 * 1024), b""):
                copied_bytes += len(chunk)
                if max_bytes is not None and copied_bytes > max_bytes:
                    raise StageError(f"Windows inventory is unexpectedly large: {source.name}")
                output.write(chunk)
                digest.update(chunk)
    finally:
        os.close(descriptor)
    value = digest.hexdigest()
    if expected is not None and value != expected:
        raise StageError(f"runtime bytes differ from producer inventory: {source.name}")
    return value


def hash_record(value, name: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise StageError(f"invalid producer SHA-256 for {name}")
    return value


def producer_inventory(path: Path, runtime_dir: Path,
                       inspector: Callable[[Path], set[str]] | None = None,
                       ) -> tuple[dict[str, dict], dict[str, tuple[Path, str]]]:
    manifest = read_inventory(path)
    if manifest.get("schema_version") != 2 or not isinstance(manifest.get("dlls"), dict):
        raise StageError("unsupported producer PE inventory schema")
    dependencies = read_inventory(path.parent / "dependencies.json")
    if dependencies.get("schema_version") != 1 or not isinstance(dependencies.get("dependencies"), list):
        raise StageError("unsupported producer dependency inventory schema")
    notices = manifest.get("notices")
    if not isinstance(notices, dict) or not notices:
        raise StageError("producer PE inventory has no notice ownership registry")
    notice_files = {}
    for owner, owner_notices in notices.items():
        if not isinstance(owner, str) or re.fullmatch(r"[a-z0-9][a-z0-9_-]*", owner) is None:
            raise StageError(f"invalid producer notice owner: {owner}")
        if not isinstance(owner_notices, dict) or not owner_notices:
            raise StageError(f"producer owner has no notices: {owner}")
        for name, digest in owner_notices.items():
            relative = safe_relative_path(name)
            if relative.parts[0] != owner or len(relative.parts) < 2 or name in notice_files:
                raise StageError(f"producer notice has incorrect ownership: {name}")
            source = runtime_dir.parent / "share/licenses" / relative
            if source.is_symlink() or not source.is_file():
                raise StageError(f"producer owner notice is missing: {name}")
            notice_files[name] = (source, hash_record(digest, name))
    llvm_notices = {"llvm-mingw/" + name for name in ("LICENSE.TXT", "COPYING", "COPYING.MinGW-w64-runtime.txt",
                                                       "COPYING.MinGW-w64.txt", "COPYING.winpthreads.txt", "COPYING.winstorecompat.txt")}
    if not llvm_notices.issubset(notice_files):
        raise StageError("producer inventory omits required LLVM-MinGW notices")
    owners = set(notices)
    for dependency in dependencies["dependencies"]:
        if not isinstance(dependency, dict) or not isinstance(dependency.get("name"), str):
            raise StageError("invalid producer dependency owner")
        notices = dependency.get("licenses")
        if not isinstance(notices, list) or not notices:
            raise StageError(f"producer owner has no notices: {dependency['name']}")
        safe_relative_path(dependency["name"])
        for notice in notices:
            safe_relative_path(notice)
            notice_file = runtime_dir.parent / "share/licenses" / dependency["name"] / notice.replace("/", "_")
            if notice_file.is_symlink() or not notice_file.is_file():
                raise StageError(f"producer owner notice is missing: {dependency['name']}/{notice}")
    records = {}
    if len(manifest["dlls"]) > MAX_RUNTIME_DLLS:
        raise StageError("producer PE inventory is unexpectedly large")
    for name, record in manifest["dlls"].items():
        if DLL_NAME.fullmatch(name) is None or name.lower() in records or not isinstance(record, dict):
            raise StageError(f"invalid producer DLL record: {name}")
        if record.get("owner") not in owners:
            raise StageError(f"producer DLL has no notice owner: {name}")
        hash_record(record.get("sha256"), name)
        hash_record(record.get("source_sha256"), f"{name} source")
        imports = record.get("imports")
        if not isinstance(imports, list) or any(not isinstance(item, str) or IMPORT_NAME.fullmatch(item) is None for item in imports):
            raise StageError(f"invalid producer imports: {name}")
        records[name.lower()] = {**record, "imports": [item.lower() for item in imports]}
    modules = manifest.get("modules", {})
    if not isinstance(modules, dict) or len(modules) > MAX_RUNTIME_DLLS:
        raise StageError("invalid producer module inventory")
    module_names = set()
    with tempfile.TemporaryDirectory(prefix="atrinik-module-inspection-") as temporary:
        for name, record in modules.items():
            relative = safe_relative_path(name)
            if relative.suffix.lower() != ".dll" or name.lower() in module_names or not isinstance(record, dict):
                raise StageError(f"invalid producer module record: {name}")
            module_names.add(name.lower())
            if record.get("owner") not in owners:
                raise StageError(f"producer module has no notice owner: {name}")
            hash_record(record.get("source_sha256"), f"{name} source")
            imports = record.get("imports")
            if not isinstance(imports, list) or any(not isinstance(item, str) or IMPORT_NAME.fullmatch(item) is None for item in imports):
                raise StageError(f"invalid producer module imports: {name}")
            snapshot = Path(temporary) / relative
            copy_snapshot(runtime_dir.parent / relative, snapshot, hash_record(record.get("sha256"), name))
            if inspector is not None and {item.lower() for item in inspector(snapshot)} != {item.lower() for item in imports}:
                raise StageError(f"module imports differ from producer inventory: {name}")
    return records, notice_files


def snapshot_python(runtime_dir: Path, inventory_path: Path, output_dir: Path) -> list[Path]:
    if runtime_dir.is_symlink() or not runtime_dir.is_dir():
        raise StageError(f"Python runtime is not a normal directory: {runtime_dir}")
    manifest = read_inventory(inventory_path)
    files = manifest.get("files")
    if manifest.get("schema_version") != 1 or not isinstance(files, dict) or not 1 <= len(files) <= 10000:
        raise StageError("invalid Python runtime inventory")
    actual = {}
    windows_paths = set()
    for source in runtime_dir.rglob("*"):
        relative = source.relative_to(runtime_dir).as_posix()
        if source.is_symlink() or not (source.is_file() or source.is_dir()):
            raise StageError(f"invalid Python runtime entry: {relative}")
        if source.is_file():
            if relative.lower() in windows_paths:
                raise StageError(f"duplicate case-insensitive Python runtime path: {relative}")
            windows_paths.add(relative.lower())
            actual[relative] = source
    if set(actual) != set(files):
        raise StageError("Python runtime files differ from producer inventory")
    binaries = []
    names = set()
    for name, source in sorted(actual.items()):
        relative = safe_relative_path(name)
        record = files[name]
        expected = hash_record(record.get("sha256") if isinstance(record, dict) else record, name)
        destination = output_dir / relative
        if GCC_RUNTIME.fullmatch(source.name):
            raise StageError(f"GCC runtime DLL in Python runtime: {name}")
        copy_snapshot(source, destination, expected)
        if source.suffix.lower() in {".dll", ".pyd", ".exe"}:
            normalized = source.name.lower()
            if normalized in names:
                raise StageError(f"duplicate Python PE filename: {source.name}")
            names.add(normalized)
            binaries.append(destination)
    return binaries


def stage_runtime(
    binaries: Iterable[Path],
    runtime_dir: Path | Iterable[Path],
    output_dir: Path,
    inspector: Callable[[Path], set[str]],
    *,
    dlls_only: bool = False,
    producer_records: dict[str, dict] | None = None,
    python_runtime: Path | None = None,
    python_inventory_path: Path | None = None,
    producer_notices: dict[str, tuple[Path, str]] | None = None,
) -> list[str]:
    sources: dict[str, Path] = {}
    runtime_dirs = [runtime_dir] if isinstance(runtime_dir, Path) else list(runtime_dir)
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
    if producer_notices is not None:
        for name, (source, digest) in producer_notices.items():
            copy_snapshot(source, output_dir / "licenses" / safe_relative_path(name), digest)
    python_binaries = []
    python_sources: dict[str, Path] = {}
    if python_runtime is not None:
        if python_inventory_path is None:
            raise StageError("Python runtime requires a producer file inventory")
        python_binaries = snapshot_python(python_runtime, python_inventory_path, output_dir)
        for binary in python_binaries:
            if binary.suffix.lower() == ".dll":
                name = binary.name.lower()
                python_sources[name] = binary
    queue: list[tuple[Path, Path, dict | None, bool]] = []
    staged: dict[str, Path] = {}

    with tempfile.TemporaryDirectory(prefix="atrinik-pe-inspection-") as temporary:
        inspection_dir = Path(temporary)

        def stage(source: Path, copy: bool = True, external: bool = False) -> None:
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
            record = None
            if producer_records is not None and source in sources.values() and not external:
                record = producer_records.get(normalized)
                if record is None:
                    raise StageError(f"runtime DLL has no producer ownership record: {source.name}")
            if external:
                inspection = source
            else:
                inspection = output_dir / source.name if copy else inspection_dir / source.name
                copy_snapshot(source, inspection, record["sha256"] if record else None)
            staged[normalized] = source
            queue.append((source, inspection, record, external))

        for binary in binaries:
            stage(binary, copy=not dlls_only)

        position = 0
        runtime_count = 0

        def inspect_queue() -> None:
            nonlocal position, runtime_count
            while position < len(queue):
                original, binary, record, external = queue[position]
                position += 1
                imports = inspector(binary)
                if record is not None and {name.lower() for name in imports} != set(record["imports"]):
                    raise StageError(f"runtime imports differ from producer inventory: {original.name}")
                for dependency in sorted(imports, key=str.lower):
                    if IMPORT_NAME.fullmatch(dependency) is None:
                        raise StageError(f"invalid imported DLL name for {binary.name}: {dependency}")
                    normalized = dependency.lower()
                    if GCC_RUNTIME.fullmatch(normalized):
                        raise StageError(f"GCC runtime DLL in LLVM closure: {dependency}")
                    if is_system_dll(dependency, external_python=external):
                        continue
                    # Resolve the actual target graph before admitting all of
                    # Python's sealed files. A full SDK's unused duplicate is
                    # harmless; a target import needing it cannot share the
                    # package filename with Python's independently built DLL.
                    expected_target = normalized in sources or (
                        producer_records is not None and normalized in producer_records
                    )
                    if not external and expected_target and normalized in python_sources:
                        raise StageError(f"reachable DLL provider conflict: {dependency}")
                    python_source = python_sources.get(normalized)
                    source = python_source if external and python_source is not None else sources.get(normalized)
                    selected_external = source is not None and source == python_source
                    if source is None:
                        source = python_source
                        selected_external = source is not None
                    previous = staged.get(normalized)
                    if previous is not None:
                        if source is not None and previous != source:
                            raise StageError(f"reachable DLL provider conflict: {dependency}")
                        continue
                    if source is None:
                        raise StageError(f"unresolved runtime DLL for {binary.name}: {dependency}")
                    runtime_count += 1
                    if runtime_count > MAX_RUNTIME_DLLS:
                        raise StageError("Windows runtime dependency closure is unexpectedly large")
                    stage(source, copy=not selected_external, external=selected_external)

        inspect_queue()
        for binary in python_binaries:
            stage(binary, copy=False, external=True)
        inspect_queue()

    return sorted((path.relative_to(output_dir).as_posix() for path in output_dir.rglob("*")
                   if path.is_file()), key=str.lower)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--readobj", required=True)
    parser.add_argument("--runtime-dir", type=Path, action="append", default=[])
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--producer-inventory", type=Path)
    parser.add_argument("--python-runtime", type=Path)
    parser.add_argument("--python-inventory", type=Path)
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
        inventory_path = arguments.producer_inventory or (
            arguments.runtime_dir[0].parent / "share/atrinik/windows-dependencies/pe-closure.json")
        with tempfile.TemporaryDirectory(prefix="atrinik-pe-inventory-") as temporary:
            snapshot_dir = Path(temporary)
            frozen_inventory = snapshot_dir / "pe-closure.json"
            frozen_dependencies = snapshot_dir / "dependencies.json"
            copy_snapshot(inventory_path, frozen_inventory, max_bytes=4 * 1024 * 1024)
            copy_snapshot(inventory_path.parent / "dependencies.json", frozen_dependencies, max_bytes=4 * 1024 * 1024)
            frozen_python = None
            if arguments.python_runtime is not None:
                frozen_python = snapshot_dir / "python-runtime.json"
                copy_snapshot(arguments.python_inventory or inventory_path.parent / "python-runtime.json", frozen_python,
                              max_bytes=4 * 1024 * 1024)
            records, notices = producer_inventory(frozen_inventory, arguments.runtime_dir[0],
                lambda binary: imported_dlls(arguments.readobj, binary))
            staged = stage_runtime(
                arguments.binaries,
                arguments.runtime_dir,
                arguments.output_dir,
                lambda binary: imported_dlls(arguments.readobj, binary),
                dlls_only=arguments.dlls_only,
                producer_records=records,
                python_runtime=arguments.python_runtime,
                python_inventory_path=frozen_python,
                producer_notices=notices,
            )
            metadata = {"windows-pe-closure.json": frozen_inventory,
                        "windows-dependencies.json": frozen_dependencies}
            if frozen_python is not None:
                metadata["windows-python-runtime.json"] = frozen_python
            for name, source in metadata.items():
                copy_snapshot(source, arguments.output_dir / "licenses" / name)
                staged.append("licenses/" + name)
    except (OSError, StageError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Windows runtime staging failed: {error}\n")
    print(json.dumps({"staged": staged}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
