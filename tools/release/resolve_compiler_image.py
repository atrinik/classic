#!/usr/bin/env python3
"""Resolve the trusted compiler image from an exact release's source lock."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import subprocess


IMAGE = re.compile(r"ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}")
LOCK = "tools/ci/appimage/packaging.lock.json"
HISTORICAL_SOURCE_LIMIT = "fd4943252ac872fce2995c286b10a0c6d249aba3"
HISTORICAL_IMAGE = "ghcr.io/atrinik/classic-build:1.16.0@sha256:e1c366dbf83ef987765ff913bbb193868314a139ae1e00cc134b22a3159464f2"
HISTORICAL_APPIMAGE_FILES = {
    "packaging.lock.json": "8a98124a11dea9aef6984ee7119c874d991fc18f14fbf1037d58a6345171e7e7",
    "Dockerfile": "f170e46418702c9bad138b48545900f33a53e92c367b71e75a53d83f10936e60",
    "build-dependencies.sh": "957bd97738c11bd4d374fdea3212a16e4aca2ae1614ea7a01e758faf712846fd",
    "runtime-probe.c": "e2ea3d6589160823ae513f34354abd72807df54c0edd1a1390de42d4d6c52f74",
    "graphical-probe.c": "299b4da4d3289c6aaad895cf82a0c83f3ccb1859c4838796ef5627804b6d754d",
}
HISTORICAL_DEBIAN_SHA256 = "9ff6f7e13e736b7ee68e42bf0fd19107143460c8784401525e984222ef845323"
HISTORICAL_SCHEMA1_SOURCE = "34bbad5c3f4d9b303c17aec5e2c75b3550ec8983"
HISTORICAL_SCHEMA1_FILES = {
    ".github/workflows/build-release-candidate.yml": "8497f85c816911f81ca541b4950843c593b961f58462bef24f533c63beb44f90",
    "tools/release/finalize_artifacts.py": "13990807db023fb69167d1ca720f80538f634fcea8b319f32f6ba73e5b082d1c",
    "tools/ci/check_installed_library.sh": "82474e87b1d287b0b3089546e80037ec32c5825db216807e81ae09584f43e0d2",
}


def git_blob(root: Path, revision: str, path: str) -> bytes:
    entry = subprocess.check_output(["git", "-C", str(root), "ls-tree", revision, "--", path], timeout=30)
    fields = entry.decode().split()
    if len(fields) < 2 or fields[0] not in ("100644", "100755") or fields[1] != "blob":
        raise ValueError(f"historical producer input must be a regular Git blob: {path}")
    return subprocess.check_output(["git", "-C", str(root), "show", revision + ":" + path], timeout=30)


def validate_historical_source(root: Path, revision: str) -> int:
    """Accept only audited pre-LLVM history and its original producer contracts."""
    source_lock_bytes(root, revision, HISTORICAL_IMAGE)
    ancestry = subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor",
                              revision, HISTORICAL_SOURCE_LIMIT], capture_output=True, timeout=30)
    if ancestry.returncode != 0:
        raise ValueError("source without a compiler descriptor is outside approved historical history")
    workflow = git_blob(root, revision, ".github/workflows/build-release-candidate.yml").decode()
    images = re.findall(r"^  CLASSIC_LINUX_IMAGE: (\S+)\s*$", workflow, re.MULTILINE)
    if images != [HISTORICAL_IMAGE]:
        raise ValueError("historical source does not select the approved original Linux producer")
    if revision == HISTORICAL_SCHEMA1_SOURCE:
        for path, expected in HISTORICAL_SCHEMA1_FILES.items():
            if hashlib.sha256(git_blob(root, revision, path)).hexdigest() != expected:
                raise ValueError(f"historical schema-1 producer input changed: {path}")
        entries = subprocess.check_output(["git", "-C", str(root), "ls-tree", revision, "--",
                                           LOCK, "tools/ci/debian-client/Dockerfile"], timeout=30)
        if entries:
            raise ValueError("historical schema-1 source unexpectedly contains a Linux client package recipe")
        return 1
    finalizer = ast.parse(git_blob(root, revision, "tools/release/finalize_artifacts.py"))
    schemas = [node.value.value for node in finalizer.body
               if isinstance(node, ast.Assign) and isinstance(node.value, ast.Constant)
               and any(isinstance(target, ast.Name) and target.id == "RELEASE_ARTIFACT_SCHEMA"
                       for target in node.targets)]
    if len(schemas) != 1 or type(schemas[0]) is not int or schemas[0] not in (2, 3):
        raise ValueError("source without a compiler descriptor has no approved historical release schema")
    schema = schemas[0]
    inputs = ({"tools/ci/appimage/" + name: sha for name, sha in HISTORICAL_APPIMAGE_FILES.items()}
              if schema == 3 else {"tools/ci/debian-client/Dockerfile": HISTORICAL_DEBIAN_SHA256})
    for path, expected in inputs.items():
        if hashlib.sha256(git_blob(root, revision, path)).hexdigest() != expected:
            raise ValueError(f"historical source differs from the approved original producer: {path}")
    if schema == 2 and source_lock_bytes(root, revision, HISTORICAL_IMAGE) is not None:
        raise ValueError("historical schema-2 source unexpectedly contains an AppImage lock")
    return schema


def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate source lock key: {key}")
        result[key] = value
    return result


def source_lock_bytes(root: Path, revision: str, current_image: str) -> bytes | None:
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("release revision must be an exact lowercase commit SHA")
    if IMAGE.fullmatch(current_image) is None:
        raise ValueError("current compiler image must be an immutable published reference")
    def git(*arguments: str) -> bytes:
        return subprocess.run(["git", "-C", str(root), *arguments], check=True,
                              capture_output=True, timeout=30).stdout
    if git("rev-parse", "--verify", revision + "^{commit}").decode().strip() != revision:
        raise ValueError("release revision is not the selected commit")
    entry = git("ls-tree", revision, "--", LOCK)
    if not entry:
        # Descriptor absence requires a separate historical producer check.
        return None
    fields = entry.decode().split()
    if fields[:2] != ["100644", "blob"]:
        raise ValueError("release packaging lock must be a regular Git blob")
    return git("show", revision + ":" + LOCK)


def resolve_descriptor(root: Path, revision: str, current_image: str) -> dict[str, object] | None:
    lock_bytes = source_lock_bytes(root, revision, current_image)
    if lock_bytes is None:
        return None
    locked = json.loads(lock_bytes, object_pairs_hook=unique_object)
    if not isinstance(locked, dict):
        raise ValueError("release packaging lock must be an object")
    if "compiler" not in locked:
        return None
    compiler = locked["compiler"]
    if not isinstance(compiler, dict):
        raise ValueError("release compiler descriptor must be an object")
    image = compiler.get("image")
    if not isinstance(image, str) or IMAGE.fullmatch(image) is None:
        raise ValueError("LLVM release lock requires its immutable compiler image")
    return compiler


def resolve_image(root: Path, revision: str, current_image: str, *, source_finalizer: bool = False) -> str | None:
    compiler = resolve_descriptor(root, revision, current_image)
    if compiler is None:
        validate_historical_source(root, revision)
        return None if source_finalizer else HISTORICAL_IMAGE
    return str(compiler["image"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--current-image", required=True)
    parser.add_argument("--source-finalizer-arguments", action="store_true",
                        help="emit only arguments supported by the selected source finalizer")
    parser.add_argument("--source-packaging-lock-output", type=Path,
                        help="write exact tagged source lock bytes for trusted SDK validation")
    options = parser.parse_args()
    try:
        image = resolve_image(options.source_root, options.revision, options.current_image,
                              source_finalizer=options.source_finalizer_arguments)
        if options.source_packaging_lock_output is not None:
            lock_bytes = source_lock_bytes(options.source_root, options.revision, options.current_image)
            options.source_packaging_lock_output.write_bytes(lock_bytes or b"")
        if image is not None:
            if options.source_finalizer_arguments:
                print("--compiler-image")
            print(image)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"cannot resolve trusted compiler image: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
