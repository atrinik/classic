#!/usr/bin/env python3
"""Verify raw-byte hashes for GPU fixture inputs and staged client packages."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath, PureWindowsPath


INPUT_ATTRIBUTES = (
    "settings",
    "archdef",
    "interface",
    "layout",
    "snapshot",
    "next-snapshot",
    "transition-snapshot",
)
PACKAGE_INPUTS = ("data/archdef.dat", "data/interface.cfg")
HEX_64 = re.compile(r"[0-9a-f]{64}\Z")


class FixtureError(ValueError):
    """A GPU fixture input or staged package violates its byte contract."""


def _directory(path: Path, label: str) -> Path:
    if path.is_symlink() or not path.is_dir():
        raise FixtureError(f"{label} is not a regular directory: {path}")
    return path.resolve()



def _package_path(root: Path, relative: PurePosixPath, label: str) -> Path:
    path = root
    for part in relative.parts:
        path /= part
        if path.is_symlink():
            raise FixtureError(f"{label} contains a symbolic-link component: {path}")
    return path

def _read_lf(path: Path, label: str) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise FixtureError(f"{label} is not a regular file: {path}")
    contents = path.read_bytes()
    if b"\r" in contents:
        raise FixtureError(f"{label} contains a carriage-return byte: {path}")
    return contents


def _inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def _relative_file(value: str, label: str) -> PurePosixPath:
    if (
        not value
        or value != value.strip()
        or "\\" in value
        or ":" in value
        or PurePosixPath(value).is_absolute()
        or PureWindowsPath(value).is_absolute()
        or PureWindowsPath(value).drive
        or any(part in ("", ".", "..") for part in value.split("/"))
    ):
        raise FixtureError(f"{label} must be a safe relative POSIX path: {value!r}")
    return PurePosixPath(*value.split("/"))


def _input_root(value: str, label: str) -> Path:
    if (
        not value
        or "\\" in value
        or ":" in value
        or PurePosixPath(value).is_absolute()
        or PureWindowsPath(value).is_absolute()
        or PureWindowsPath(value).drive
        or any(part == "" for part in value.split("/"))
    ):
        raise FixtureError(f"{label} must be a safe relative POSIX path: {value!r}")
    return Path(*value.split("/"))


def _resolve_input(
    source_root: Path,
    manifest: Path,
    input_root: str,
    value: str,
    label: str,
) -> tuple[Path, str]:
    relative = _relative_file(value, label)
    base = (manifest.parent / _input_root(input_root, f"{manifest}: input-root")).resolve()
    if not _inside(base, source_root):
        raise FixtureError(f"{manifest}: input-root escapes the client source root")
    candidate = base.joinpath(*relative.parts)
    resolved = candidate.resolve()
    if not _inside(resolved, source_root) or resolved != candidate:
        raise FixtureError(f"{manifest}: {label} escapes the client source root")
    return resolved, resolved.relative_to(source_root).as_posix()


def validate_source(source_root: Path) -> dict[str, str]:
    """Return the declared raw-byte digests after validating every GPU manifest."""
    source_root = _directory(source_root, "client source root")
    fixture_root = _directory(
        source_root / "src" / "tests" / "fixtures" / "player_view",
        "GPU fixture root",
    )
    manifests = sorted(fixture_root.glob("*.xml"))
    if not manifests:
        raise FixtureError(f"GPU fixture root has no XML manifests: {fixture_root}")

    pinned: dict[str, str] = {}
    gpu_manifests = 0
    for manifest in manifests:
        manifest_bytes = _read_lf(manifest, "GPU fixture manifest")
        try:
            root = ET.fromstring(manifest_bytes)
        except ET.ParseError as error:
            raise FixtureError(f"invalid GPU fixture manifest {manifest}: {error}") from error
        if root.attrib.get("renderer") != "gpu":
            continue
        gpu_manifests += 1
        input_root = root.attrib.get("input-root", ".")
        for attribute in INPUT_ATTRIBUTES:
            value = root.attrib.get(attribute)
            digest_name = f"{attribute}-sha256"
            expected = root.attrib.get(digest_name)
            if value is None:
                if expected is not None:
                    raise FixtureError(f"{manifest}: {digest_name} has no matching input")
                continue
            if expected is None or not HEX_64.fullmatch(expected):
                raise FixtureError(f"{manifest}: {digest_name} is not a lowercase SHA-256")
            path, relative = _resolve_input(
                source_root, manifest, input_root, value, attribute
            )
            prior = pinned.get(relative)
            if prior is not None and prior != expected:
                raise FixtureError(f"conflicting SHA-256 pins for {relative}")
            pinned[relative] = expected
            contents = _read_lf(path, f"pinned {attribute} input")
            actual = hashlib.sha256(contents).hexdigest()
            if actual != expected:
                raise FixtureError(
                    f"{relative}: expected {expected}, found {actual}"
                )

    if gpu_manifests == 0 or not pinned:
        raise FixtureError("no hash-pinned GPU fixture inputs were found")
    return pinned


def validate_package(package_root: Path, source_inputs: dict[str, str]) -> None:
    """Check the hash-pinned client data files copied into a staged package."""
    package_root = _directory(package_root, "staged package root")
    for relative_text in PACKAGE_INPUTS:
        relative = PurePosixPath(relative_text)
        expected = source_inputs.get(relative_text)
        if expected is None:
            raise FixtureError(f"source manifests do not pin package input: {relative_text}")
        path = _package_path(package_root, relative, "staged package input")
        contents = _read_lf(path, "staged package input")
        actual = hashlib.sha256(contents).hexdigest()
        if actual != expected:
            raise FixtureError(
                f"staged package {relative_text}: expected {expected}, found {actual}"
            )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="client source directory (default: this script's client directory)",
    )
    parser.add_argument(
        "--package-root",
        type=Path,
        help="optional extracted client package directory to validate",
    )
    arguments = parser.parse_args(argv)
    try:
        source_inputs = validate_source(arguments.source_root)
        if arguments.package_root is not None:
            validate_package(arguments.package_root, source_inputs)
    except (FixtureError, OSError) as error:
        print(f"GPU fixture byte verification failed: {error}", file=sys.stderr)
        return 1
    package_message = " and staged package" if arguments.package_root is not None else ""
    print(f"GPU fixture byte verification passed: {len(source_inputs)} inputs{package_message}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
