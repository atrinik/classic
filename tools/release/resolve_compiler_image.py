#!/usr/bin/env python3
"""Resolve the trusted compiler image from an exact release's source lock."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess


IMAGE = re.compile(r"ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}")
LOCK = "tools/ci/appimage/packaging.lock.json"


def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate source lock key: {key}")
        result[key] = value
    return result


def resolve_descriptor(root: Path, revision: str, current_image: str) -> dict[str, object] | None:
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
        # Pre-AppImage source has no compiler descriptor to constrain.
        return None
    fields = entry.decode().split()
    if fields[:2] != ["100644", "blob"]:
        raise ValueError("release packaging lock must be a regular Git blob")
    locked = json.loads(git("show", revision + ":" + LOCK), object_pairs_hook=unique_object)
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
        return None if source_finalizer else current_image
    return str(compiler["image"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--current-image", required=True)
    parser.add_argument("--source-finalizer-arguments", action="store_true",
                        help="emit only arguments supported by the selected source finalizer")
    parser.add_argument("--source-packaging-lock-output", type=Path,
                        help="write exact tagged LLVM source lock bytes for trusted SDK validation")
    options = parser.parse_args()
    try:
        image = resolve_image(options.source_root, options.revision, options.current_image,
                              source_finalizer=options.source_finalizer_arguments)
        if options.source_packaging_lock_output is not None:
            descriptor = resolve_descriptor(options.source_root, options.revision, options.current_image)
            lock_bytes = b"" if descriptor is None else subprocess.run(
                ["git", "-C", str(options.source_root), "show", options.revision + ":" + LOCK],
                check=True, capture_output=True, timeout=30).stdout
            options.source_packaging_lock_output.write_bytes(lock_bytes)
        if image is not None:
            if options.source_finalizer_arguments:
                print("--compiler-image")
            print(image)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"cannot resolve trusted compiler image: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
