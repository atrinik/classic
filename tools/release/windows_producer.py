#!/usr/bin/env python3
"""Select the Windows build producer for an exact source commit.

Six pre-LLVM release commits build with the historical MXE image; every other
commit builds with the current LLVM image and must carry its immutable LLVM
compiler descriptor. An absent descriptor never grants a historical fallback.

The current recipe rejects preferred local modules that would shadow the
canonical shared CMake and staging inputs. Source content is trusted at the
selected Git commit; source review and fresh hosted build evidence establish
compiler use, and artifact inspection establishes package layout and runtime
identities, not compiler origin.
"""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess

LLVM_IMAGE = "ghcr.io/atrinik/windows-build:1.18.0@sha256:01065f081cdf843a6591abf25f2b27734103ab385c5aa003b2eeb9a3a1ca0e5a"
MXE_IMAGE = "ghcr.io/atrinik/windows-build:1.16.0@sha256:4266256d4b4196241df6463e687dd286022e24e3bc46dfafb4f877744a731d69"
HISTORICAL = frozenset({
    "34bbad5c3f4d9b303c17aec5e2c75b3550ec8983",
    "994e6fdc20da962be9c11055bda28ef5561a1dea",
    "24bee6c8a830aa7372a30be6b842210b07639f73",
    "c5a8f9fa2bdcfbae9311093fb69907c8d64b35d4",
    "fd4943252ac872fce2995c286b10a0c6d249aba3",
    "2349e0e14276fee251250cd451da6da0e68a3148",
})
# Current monorepo entrypoints must keep using the authenticated shared modules.
# Standalone source-archive overlays are not admitted by this producer recipe.
LLVM_RECIPE_ALTERNATIVES = (
    "client/cmake/AtrinikVersion.cmake",
    "client/cmake/AtrinikLLVM.cmake",
    "server/cmake/AtrinikVersion.cmake",
    "server/cmake/AtrinikLLVM.cmake",
    "protocol/cmake/AtrinikVersion.cmake",
    "client/cmake/InstallWindowsRuntime.cmake",
    "server/cmake/InstallWindowsRuntime.cmake",
    "server/cmake/toolchains/llvm.cmake",
    "client/tools/ci/stage_windows_runtime.py",
    "server/tools/ci/stage_windows_runtime.py",
)
SYSTEM_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"


def git(root: Path, *arguments: str) -> bytes:
    return subprocess.run(["git", "-C", str(root), *arguments], check=True,
                          capture_output=True, timeout=30).stdout


def verify_llvm_recipe_alternatives(root: Path, revision: str) -> None:
    for path in LLVM_RECIPE_ALTERNATIVES:
        local = root / path
        if git(root, "ls-tree", revision, "--", path) or local.exists() or local.is_symlink():
            raise ValueError(f"unapproved Windows source recipe alternative: {path}")


def verify_llvm_descriptor(root: Path, revision: str) -> None:
    path = "tools/ci/appimage/packaging.lock.json"
    entry = git(root, "ls-tree", revision, "--", path).decode().split()
    if len(entry) != 4 or entry[:2] != ["100644", "blob"] or entry[3] != path:
        raise ValueError("current Windows source requires its LLVM compiler descriptor")
    source = git(root, "show", revision + ":" + path)

    def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate LLVM source descriptor key: {key}")
            result[key] = value
        return result

    descriptor = json.loads(source, object_pairs_hook=unique_object)
    compiler = descriptor.get("compiler") if isinstance(descriptor, dict) else None
    if (not isinstance(compiler, dict) or compiler.get("family") != "Clang"
            or not isinstance(compiler.get("image"), str)
            or re.fullmatch(r"ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}",
                            compiler["image"]) is None):
        raise ValueError("current Windows source requires its immutable LLVM compiler descriptor")
    local = root / path
    if local.is_symlink() or not local.is_file() or local.read_bytes() != source:
        raise ValueError("LLVM compiler descriptor checkout differs from selected source")


def select_producer(root: Path, revision: str) -> dict[str, object]:
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise ValueError("Windows revision must be an exact lowercase commit SHA")
    if git(root, "rev-parse", "--verify", revision + "^{commit}").decode().strip() != revision:
        raise ValueError("Windows revision must select a commit")
    if git(root, "rev-parse", "HEAD").decode().strip() != revision:
        raise ValueError("Windows checkout HEAD differs from selected source")
    if revision in HISTORICAL:
        environment = {
            "PATH": "/opt/mxe/.ccache/bin:/opt/mxe/usr/bin:" + SYSTEM_PATH,
            "MXE_TARGET": "x86_64-w64-mingw32.shared",
            "MXE_TOOLCHAIN_FILE": "/opt/mxe/usr/x86_64-w64-mingw32.shared/share/cmake/mxe-conf.cmake",
            "MXE_RUNTIME_DIR": "/opt/mxe/usr/x86_64-w64-mingw32.shared/bin",
        }
        # The original GENERAL SDK supplies Python include/import/runtime and
        # native GCC/Python/worldmaker dependencies. Do not replace its ENV with
        # the focused Check SDK or demand receipts that predate neither source
        # nor SDK. Original CMake owns DLL glob/c-ares/Python notice staging.
        image, mode, epoch = MXE_IMAGE, "mxe", "mxe-gcc-general-1.16-4266256d"
    else:
        verify_llvm_recipe_alternatives(root, revision)
        verify_llvm_descriptor(root, revision)
        environment = {
            "PATH": "/opt/llvm-mingw/bin:/opt/atrinik-windows/bin:" + SYSTEM_PATH,
            "ATRINIK_WINDOWS_TOOLCHAIN_FILE": "/opt/atrinik-windows/share/cmake/llvm-mingw.cmake",
            "ATRINIK_WINDOWS_TARGET": "x86_64-w64-mingw32",
            "ATRINIK_WINDOWS_RUNTIME_DIR": "/opt/atrinik-windows/bin",
            "ATRINIK_WINDOWS_HOST_PYTHON": "/usr/bin/python3",
        }
        image, mode, epoch = LLVM_IMAGE, "llvm", "llvm-mingw-1.18-01065f08"
    return {"windows_image": image, "windows_mode": mode,
            "windows_cache_epoch": epoch, "environment": environment}


def docker_environment(producer: dict[str, object]) -> list[str]:
    return [argument for name, value in producer["environment"].items()
            for argument in ("--env", name + "=" + value)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--github-output", type=Path)
    parser.add_argument("--format", choices=("json", "docker-env"), default="json")
    options = parser.parse_args()
    try:
        producer = select_producer(options.source_root, options.revision)
        if options.github_output is not None:
            with options.github_output.open("a", encoding="utf-8") as output:
                for key in ("windows_image", "windows_mode", "windows_cache_epoch"):
                    output.write(f"{key}={producer[key]}\n")
        if options.format == "docker-env":
            print("\n".join(docker_environment(producer)))
        else:
            print(json.dumps(producer, sort_keys=True))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"cannot select trusted Windows producer: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
