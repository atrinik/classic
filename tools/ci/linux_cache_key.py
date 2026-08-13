#!/usr/bin/env python3
"""Build fail-closed, trust-scoped Linux ccache keys for Classic Check."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path, PurePosixPath
import re


ROOT = Path(__file__).resolve().parents[2]
SHA_RE = re.compile(r"[0-9a-f]{40}")
DIGEST_RE = re.compile(r"sha256:([0-9a-f]{64})")
TOKEN_RE = re.compile(r"[A-Za-z0-9._-]+")
COMPONENTS = {"client", "core", "integrated", "server"}


class CacheKeyError(RuntimeError):
    """Raised when an input cannot produce a safe cache key."""


def cache_scope(event: str, ref: str, pr_number: str, commit: str) -> str:
    if SHA_RE.fullmatch(commit) is None:
        raise CacheKeyError("commit must be a full lowercase commit ID")
    if event == "push":
        if ref != "refs/heads/main":
            raise CacheKeyError("trusted push caches are restricted to main")
        return "trusted-main"
    if event == "pull_request":
        if not pr_number.isdecimal() or int(pr_number) < 1:
            raise CacheKeyError("pull requests require a positive number")
        return f"pr-{int(pr_number)}"
    if event == "merge_group":
        return f"merge-{commit}"
    if event == "workflow_dispatch":
        return f"dispatch-{commit}"
    raise CacheKeyError(f"unsupported cache event: {event}")


def safe_material(value: str, root: Path = ROOT) -> Path:
    relative = PurePosixPath(value)
    if not value or relative.is_absolute() or ".." in relative.parts:
        raise CacheKeyError(f"unsafe material path: {value!r}")
    path = root.resolve(strict=True)
    for part in relative.parts:
        path = path / part
        if path.is_symlink():
            raise CacheKeyError(f"material path contains a symlink: {value}")
    if not path.is_file():
        raise CacheKeyError(f"material is not a regular file: {value}")
    return path


def material_digest(materials: list[str], root: Path = ROOT) -> str:
    if not materials:
        raise CacheKeyError("at least one material file is required")
    digest = hashlib.sha256()
    for value in sorted(set(materials)):
        path = safe_material(value, root)
        digest.update(value.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def build_prefix(
    *,
    component: str,
    event: str,
    ref: str,
    pr_number: str,
    commit: str,
    image_digest: str,
    compiler: str,
    epoch: str,
    materials: list[str],
    root: Path = ROOT,
) -> str:
    if component not in COMPONENTS:
        raise CacheKeyError(f"unsupported component: {component}")
    match = DIGEST_RE.fullmatch(image_digest)
    if match is None:
        raise CacheKeyError("image digest must be a sha256 digest")
    for label, value in (("compiler", compiler), ("epoch", epoch)):
        if TOKEN_RE.fullmatch(value) is None:
            raise CacheKeyError(f"{label} contains unsafe characters")
    scope = cache_scope(event, ref, pr_number, commit)
    config = material_digest(materials, root)
    return (
        f"classic-Linux-X64-linux-{scope}-{component}-ccache-v{epoch}-"
        f"{compiler}-image-{match.group(1)}-config-{config}"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--component", required=True, choices=sorted(COMPONENTS))
    parser.add_argument("--event", required=True)
    parser.add_argument("--ref", required=True)
    parser.add_argument("--pr-number", default="")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--image-digest", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--epoch", required=True)
    parser.add_argument("--material", action="append", default=[])
    parser.add_argument("--github-output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    prefix = build_prefix(
        component=args.component,
        event=args.event,
        ref=args.ref,
        pr_number=args.pr_number,
        commit=args.commit,
        image_digest=args.image_digest,
        compiler=args.compiler,
        epoch=args.epoch,
        materials=args.material,
    )
    with args.github_output.open("a", encoding="utf-8") as output:
        output.write(f"key={prefix}-{args.commit}\n")
        output.write(f"restore_prefix={prefix}-\n")
        output.write(f"scope={cache_scope(args.event, args.ref, args.pr_number, args.commit)}\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CacheKeyError as error:
        raise SystemExit(str(error)) from error
