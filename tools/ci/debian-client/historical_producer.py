#!/usr/bin/env python3
"""Select the approved frozen GCC producer for historical Debian recovery."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import subprocess


SOURCE_PRODUCER = "tools/ci/debian-client/Dockerfile"
TRUSTED_PRODUCER = "tools/ci/debian-client/Dockerfile.historical"
# Original schema-2 producer at 994e6fdc20; frozen Debian digest and APT snapshot.
PRODUCER_SHA256 = "9ff6f7e13e736b7ee68e42bf0fd19107143460c8784401525e984222ef845323"


def resolve_producer(source_root: Path, revision: str, trusted_root: Path) -> str:
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise ValueError("release revision must be an exact lowercase commit SHA")

    def git(*arguments: str) -> bytes:
        return subprocess.run(["git", "-C", str(source_root), *arguments], check=True,
                              capture_output=True, timeout=30).stdout

    if git("rev-parse", "--verify", revision + "^{commit}").decode().strip() != revision:
        raise ValueError("release revision is not the selected commit")
    if git("rev-parse", "HEAD").decode().strip() != revision or git("status", "--porcelain", "--untracked-files=all"):
        raise ValueError("candidate source checkout must be clean at the exact release revision")
    entry = git("ls-tree", revision, "--", SOURCE_PRODUCER).decode().split()
    if entry[:2] != ["100644", "blob"]:
        raise ValueError("historical Debian producer must be a regular Git blob")
    source = git("show", revision + ":" + SOURCE_PRODUCER)
    if hashlib.sha256(source).hexdigest() != PRODUCER_SHA256:
        raise ValueError("source has no approved historical Debian producer")
    trusted = trusted_root / TRUSTED_PRODUCER
    if trusted.is_symlink() or hashlib.sha256(trusted.read_bytes()).hexdigest() != PRODUCER_SHA256:
        raise ValueError("trusted historical Debian producer differs from its approved pin")
    return TRUSTED_PRODUCER


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()
    try:
        print(resolve_producer(args.source_root, args.revision, Path(__file__).resolve().parents[3]))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"cannot select historical Debian producer: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
