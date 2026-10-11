#!/usr/bin/env python3
"""Select an authenticated Windows recipe without changing candidate source.

The registry deliberately admits only two reviewed pre-LLVM source commits.
A source recipe change requires reviewing and updating its digest registry; an
absent compiler descriptor never grants an automatic historical fallback.
"""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

LLVM_IMAGE = "ghcr.io/atrinik/windows-build:1.18.0@sha256:01065f081cdf843a6591abf25f2b27734103ab385c5aa003b2eeb9a3a1ca0e5a"
MXE_IMAGE = "ghcr.io/atrinik/windows-build:1.16.0@sha256:4266256d4b4196241df6463e687dd286022e24e3bc46dfafb4f877744a731d69"
HISTORICAL = {
    "fd4943252ac872fce2995c286b10a0c6d249aba3": {
        "client/CMakeLists.txt": "da73739b768228f16f3372423ee6d05041f0fb7977544cca85f8bc56b1ea2ca4",
        ".github/workflows/build-release-candidate.yml": "a2d0ee01585c89a74cb1f995f85ebedb1836e5c71b3dafe8083aa17e96a05f69",
    },
    "2349e0e14276fee251250cd451da6da0e68a3148": {
        "client/CMakeLists.txt": "7b96197394a739c1244285c9524775b838814249fe24544b65f8a3ec92da502f",
        ".github/workflows/build-release-candidate.yml": "aa39a531b2c8b1248bc1f3aed94a6b44ae013e2bdc0259db2289b9abb242d646",
    },
}
MXE_RECIPE = {
    "client/tools/build-windows-package.sh": "b4ca907f6fcb847b5171902d723a22572034e7c763cf51a27f3bb70acc06ba8e",
    "server/tools/build-windows-package.sh": "0431158578a9ce0b9d4d0111b78db6ead9871807af97ace83201b402aff9811c",
    "server/CMakeLists.txt": "a0aad039d80d5a155288d8e22d64cb27bd1daa0ce5dbd674a0ad876f64fdfa0f",
    "tools/ci/stage_windows_runtime.py": "12d315f854726e46ec768c5c4db6a34b0aabb93e5cab959233f5c5ec1650aeca",
}
LLVM_RECIPE = {
    "client/tools/build-windows-package.sh": "10e25f9e5dd939fc7b34b14ae0d197956540659fec531b4915b6e0e9f43fddba",
    "server/tools/build-windows-package.sh": "fd8c5dc43aaa88a9ea772bcbf151f82ea7cd5f2a77ac33734905f585f4df7a03",
    "client/CMakeLists.txt": "f4a8595ffe7f4c4888b77eea2c8ddc248a6e0c74c733bc87b7b7bafb88d19f12",
    "server/CMakeLists.txt": "e9bac9c28d7f4f180dcf0f6496e8b429722ac5e8a82a60454381ac9125c21e1b",
    "tools/ci/stage_windows_runtime.py": "2640baf4187bce4afca95da560cff08e4bdf8802840245c3cbf26eff372691e9",
    "cmake/toolchains/llvm.cmake": "b1eb85208a5f44a5b28956c33b92d86c4f056da1cac9355b483a84b46d55a513",
}
SYSTEM_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"


def git(root: Path, *arguments: str) -> bytes:
    return subprocess.run(["git", "-C", str(root), *arguments], check=True,
                          capture_output=True, timeout=30).stdout


def verify_recipe(root: Path, revision: str, recipe: dict[str, str]) -> None:
    for path, expected in recipe.items():
        entry = git(root, "ls-tree", revision, "--", path).decode().split()
        if len(entry) != 4 or (entry[0] not in {"100644", "100755"} or entry[1] != "blob") or entry[3] != path:
            raise ValueError(f"Windows recipe is not a regular source blob: {path}")
        source = git(root, "show", revision + ":" + path)
        if hashlib.sha256(source).hexdigest() != expected:
            raise ValueError(f"unapproved Windows source recipe: {path}")
        # The package scripts execute the checkout, so tagged blobs alone do not
        # prove the actual inputs. Never rewrite or repair those inputs here.
        local = root / path
        if local.is_symlink() or not local.is_file() or local.read_bytes() != source:
            raise ValueError(f"Windows checkout differs from selected source: {path}")


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
    historical = HISTORICAL.get(revision)
    if historical is not None:
        verify_recipe(root, revision, MXE_RECIPE | historical)
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
        verify_recipe(root, revision, LLVM_RECIPE)
        verify_llvm_descriptor(root, revision)
        environment = {
            "PATH": "/opt/llvm-mingw/bin:/opt/atrinik-windows/bin:" + SYSTEM_PATH,
            "ATRINIK_WINDOWS_TOOLCHAIN_FILE": "/opt/atrinik-windows/share/cmake/llvm-mingw.cmake",
            "ATRINIK_WINDOWS_TARGET": "x86_64-w64-mingw32",
            "ATRINIK_WINDOWS_RUNTIME_DIR": "/opt/atrinik-windows/bin",
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
