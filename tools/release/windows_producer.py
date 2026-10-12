#!/usr/bin/env python3
"""Select an authenticated Windows recipe without changing candidate source.

The registry deliberately admits only six reviewed pre-LLVM source commits.
Schema 1 is admitted only for its exact authenticated registry entry; unknown
source fails closed.
A source recipe change requires reviewing and updating its digest registry; an
absent compiler descriptor never grants an automatic historical fallback.

The current recipe guards drift in selected compiler/version policy, runtime
and sound staging, dependency-acquisition CMake policy and their module routing.
It rejects preferred local modules that would shadow those canonical inputs.
It does not authenticate the entire executable source graph or independently
attest compiler origin. Ordinary C, source-list CMake, protocol/pathfinding source
graphs, generators and the compiled native worldmaker remain trusted reviewed
inputs at the selected Git commit. Source review and fresh hosted build evidence
establish compiler use; artifact inspection establishes package layout and runtime
identities, not compiler origin.
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
    "34bbad5c3f4d9b303c17aec5e2c75b3550ec8983": {
        "client/CMakeLists.txt": "a75d70efe1d736d10496d993bde83bf49012789dcf370d2b962f7d2bff64de03",
        ".github/workflows/build-release-candidate.yml": "8497f85c816911f81ca541b4950843c593b961f58462bef24f533c63beb44f90",
    },
    "994e6fdc20da962be9c11055bda28ef5561a1dea": {
        "client/CMakeLists.txt": "443e654babcb909ef3a78eb4798a23420d732ede073f652fcf98b042ca8433d3",
        ".github/workflows/build-release-candidate.yml": "aa39a531b2c8b1248bc1f3aed94a6b44ae013e2bdc0259db2289b9abb242d646",
    },
    "24bee6c8a830aa7372a30be6b842210b07639f73": {
        "client/CMakeLists.txt": "443e654babcb909ef3a78eb4798a23420d732ede073f652fcf98b042ca8433d3",
        ".github/workflows/build-release-candidate.yml": "aa39a531b2c8b1248bc1f3aed94a6b44ae013e2bdc0259db2289b9abb242d646",
    },
    "c5a8f9fa2bdcfbae9311093fb69907c8d64b35d4": {
        "client/CMakeLists.txt": "443e654babcb909ef3a78eb4798a23420d732ede073f652fcf98b042ca8433d3",
        ".github/workflows/build-release-candidate.yml": "aa39a531b2c8b1248bc1f3aed94a6b44ae013e2bdc0259db2289b9abb242d646",
    },
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
    "client/tools/build-windows-package.sh": "635c2eba2923b0a90c6d2896ae7f00c85f15685b1acd74b0e78cffe8ce365d99",
    "server/tools/build-windows-package.sh": "14b93dc0f22ad945225712833d044d48704134f1b3ce768b10451998dd6916c2",
    "client/CMakeLists.txt": "f13c97b203a198cdf5848a48bcbc218641d2787422eb6c194a2eeb37b3474e83",
    "server/CMakeLists.txt": "ecdfbfd98d6864d7f43541409a21e63e5db024ac0c65ba8b161c5dc22e8a941a",
    "libatrinik/CMakeLists.txt": "a6bc61acf194bd33b43e86f77dd6068e1ee6be17005390a7f17ddde585efb6a9",
    "libatrinik/cmake/RequireClassicProtocol.cmake": "774325cab773ea8277eb3936238e4708141838169b4384d914a83fd7ee888734",
    "tools/ci/stage_windows_runtime.py": "2640baf4187bce4afca95da560cff08e4bdf8802840245c3cbf26eff372691e9",
    "cmake/toolchains/llvm.cmake": "b1eb85208a5f44a5b28956c33b92d86c4f056da1cac9355b483a84b46d55a513",
    "server/cmake/pcpnatpmp.cmake": "f020a35f4ff28a37465d244fd49b6eb3091ee45836c2ba78346fe0844cf5ac70",
    "server/cmake/immutable_source_cache.cmake": "0d83c3c2bd4e3d2790a4cf749ee52d5b0de8bcd7ded827386bf6dc2a9b1ba2f3",
    "server/cmake/apply_patch_idempotent.cmake": "df536def2f9dd910cc1e6b7e96925b0047099c258f7b3b388541371600815fef",
    "cmake/AtrinikLLVM.cmake": "5a87ecf9c1f5cadc0dcf3391c3d461eac90a6dce06b4238e625f86beed0727bc",
    "cmake/AtrinikVersion.cmake": "49ba744105d0bdbe8e90d46d19470f4357612ac3019b26bca1c4f25e3a412266",
    "cmake/InstallWindowsRuntime.cmake": "18efb62084ec7e2324a0a533295b0517f89f642e495659c4001cb5c4168f3c38",
    "client/cmake/verify_windows_client_sound.cmake": "5b933e4c70315925980f38ec32d59fbc43caead94baf380281052d632f4482d9",
}
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
