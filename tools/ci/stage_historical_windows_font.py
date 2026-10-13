#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Adapt one immutable MXE font baseline; never stage LLVM candidate bundles."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import types

HELPER_REVISION = "fd4943252ac872fce2995c286b10a0c6d249aba3"
HELPER_SHA256 = "12d315f854726e46ec768c5c4db6a34b0aabb93e5cab959233f5c5ec1650aeca"
IMAGE = "ghcr.io/atrinik/windows-build:1.16.0@sha256:4266256d4b4196241df6463e687dd286022e24e3bc46dfafb4f877744a731d69"
PREFIX = Path("/opt/mxe/usr/x86_64-w64-mingw32.shared")
OBJDUMP = "x86_64-w64-mingw32.shared-objdump"
GCC = "x86_64-w64-mingw32.shared-gcc"
EXTRA_SYSTEM_DLLS = frozenset({"imm32.dll", "oleaut32.dll", "setupapi.dll"})
IMPORT_NAME = re.compile(r"[A-Za-z0-9_+.-]+\.dll", re.IGNORECASE)
DELAY_DIRECTORY = re.compile(
    r"^Entry d ([0-9a-f]+) ([0-9a-f]+) Delay Import Directory\s*$",
    re.IGNORECASE | re.MULTILINE,
)


class BaselineError(RuntimeError):
    """The historical font fixture cannot be bound to its verified inputs."""


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def regular_bytes(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise BaselineError(f"not a regular fixture file: {path}")
    return path.read_bytes()


def load_helper(path: Path):
    # Execute the exact buffer we verified, without rereading it or using pyc.
    data = regular_bytes(path)
    if digest(data) != HELPER_SHA256:
        raise BaselineError("historical staging helper checksum mismatch")
    module = types.ModuleType("verified_historical_windows_font_staging")
    module.__file__ = str(path)
    exec(compile(data, str(path), "exec"), module.__dict__)
    return module


def current_policy(path: Path) -> tuple[set[str], str]:
    data = regular_bytes(path)
    for node in ast.parse(data).body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "SYSTEM_DLLS"
            for target in node.targets
        ):
            policy = ast.literal_eval(node.value)
            if isinstance(policy, set) and all(isinstance(name, str) for name in policy):
                if not EXTRA_SYSTEM_DLLS <= policy:
                    raise BaselineError("historical additions are absent from current system policy")
                return policy, digest(data)
    raise BaselineError("cannot validate current Windows system policy")


def parse_inspection(helper, output: str) -> set[str]:
    if len(re.findall(r"file format pei-x86-64\s*$", output, re.MULTILINE)) != 1:
        raise BaselineError("historical fixture is not an AMD64 PE image")
    delay = DELAY_DIRECTORY.findall(output)
    if len(delay) != 1 or any(int(value, 16) for value in delay[0]):
        raise BaselineError("historical fixture has unsupported or unknown delayed imports")
    names = [match.group(1) for match in helper.DLL_NAME.finditer(output)]
    if len(names) != len(re.findall(r"DLL Name:", output, re.IGNORECASE)):
        raise BaselineError("malformed historical import row")
    if any(IMPORT_NAME.fullmatch(name) is None for name in names):
        raise BaselineError("invalid historical DLL import name")
    return {name.lower() for name in names}


def inspect(helper, objdump: str, path: Path) -> dict:
    before = digest(regular_bytes(path))
    result = subprocess.run(
        [objdump, "-p", str(path)], capture_output=True, text=True, timeout=30,
    )
    if result.returncode:
        raise BaselineError(f"cannot inspect {path}: {result.stderr.strip()}")
    imports = parse_inspection(helper, result.stdout)
    if digest(regular_bytes(path)) != before:
        raise BaselineError(f"fixture changed during inspection: {path}")
    return {"sha256": before, "imports": sorted(imports), "architecture": "AMD64"}


def copy_notices(source: Path, output: Path) -> dict[str, str]:
    if source.is_symlink() or not source.is_dir():
        raise BaselineError("historical notice directory is not a normal directory")
    hashes = {}
    seen = set()
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        normalized = relative.as_posix().lower()
        if normalized in seen or path.is_symlink():
            raise BaselineError(f"ambiguous historical notice path: {relative}")
        seen.add(normalized)
        if path.is_dir():
            continue
        data = regular_bytes(path)
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        hashes[relative.as_posix()] = digest(data)
    if not hashes:
        raise BaselineError("historical notice tree is empty")
    return hashes


def stage_fixture(helper_path: Path, binary: Path, runtime: Path, notices: Path,
                  output: Path, objdump: str, compiler: str, *, inspector=inspect) -> dict:
    if binary.name != "client-font-render-probe.exe":
        raise BaselineError("adapter accepts only the standalone historical font probe")
    helper = load_helper(helper_path)
    _, policy_sha256 = current_policy(Path(__file__).with_name("stage_windows_runtime.py"))
    helper.SYSTEM_DLLS |= EXTRA_SYSTEM_DLLS
    # Every name has passed IMPORT_NAME before this legacy API-set prefix test.
    records = {}

    def record(path):
        data = inspector(helper, objdump, path)
        records[path.name.lower()] = data
        return set(data["imports"])

    helper.stage_runtime([binary], runtime, output, record)
    actual = {}
    for path in output.iterdir():
        if path.is_symlink() or not path.is_file() or path.name.lower() in actual:
            raise BaselineError("invalid staged historical fixture inventory")
        actual[path.name.lower()] = path
    if actual.keys() != records.keys():
        raise BaselineError("staged historical inventory differs from inspected source inventory")
    for name, path in actual.items():
        if inspector(helper, objdump, path) != records[name]:
            raise BaselineError(f"copied historical fixture bytes changed: {name}")
        for dependency in records[name]["imports"]:
            if not helper.is_system_dll(dependency) and dependency not in actual:
                raise BaselineError(f"incomplete copied historical closure: {dependency}")
    notice_hashes = copy_notices(notices, output / "licenses")
    manifest = {
        "schema_version": 1,
        "purpose": "historical-windows-font-comparison-fixture",
        "producer_image": IMAGE,
        "producer_identity_evidence": "workflow exact digest-pinned docker run",
        "historical_helper_revision": HELPER_REVISION,
        "historical_helper_sha256": HELPER_SHA256,
        "adapter_sha256": digest(regular_bytes(Path(__file__))),
        "current_system_policy_sha256": policy_sha256,
        "additional_system_dlls": sorted(EXTRA_SYSTEM_DLLS),
        "compiler": compiler,
        "objdump": objdump,
        "runtime_directory": str(runtime),
        "pe_files": records,
        "notices": notice_hashes,
        "notice_coverage": {"complete": False, "status": "historical producer completeness unverified"},
        "native_windows_rendering_verified": False,
    }
    (output / "historical-font-inventory.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--historical-helper", type=Path, required=True)
    parser.add_argument("--producer-image", required=True)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--notice-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    try:
        if args.producer_image != IMAGE or args.objdump != OBJDUMP:
            raise BaselineError("adapter requires the exact historical producer and inspector")
        if args.runtime_dir.resolve() != PREFIX / "bin" or args.notice_dir.resolve() != PREFIX / "share/licenses":
            raise BaselineError("adapter requires the historical SDK runtime and notice directories")
        for tool in (OBJDUMP, GCC):
            found = shutil.which(tool)
            if not found or not Path(found).resolve().is_relative_to(Path("/opt/mxe/usr")):
                raise BaselineError(f"historical SDK tool cannot be resolved: {tool}")
        compiler = subprocess.check_output([GCC, "--version"], text=True, timeout=30)
        manifest = stage_fixture(args.historical_helper, args.binary, args.runtime_dir,
                                 args.notice_dir, args.output_dir, args.objdump, compiler)
    except (BaselineError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Historical font staging failed: {error}\n")
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
