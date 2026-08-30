#!/usr/bin/env python3
"""Validate a generated shader cohort and embed it in a C header."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import tempfile


STAGES = ("final_fragment", "final_vertex", "light_fragment", "light_vertex",
          "world_fragment", "world_vertex")
FORMATS = ("dxil", "msl", "spv")
EXPECTED_NAMES = tuple(f"{stage}.{shader_format}" for shader_format in FORMATS
                       for stage in STAGES)
MANIFEST_LINE = re.compile(r"([0-9a-f]{64})  \./([a-z_]+\.(?:dxil|msl|spv))")


class ShaderError(RuntimeError):
    """A generated shader cohort is incomplete or does not match its lock."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_manifest(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except OSError as error:
        raise ShaderError(f"cannot read shader manifest {path}: {error}") from error
    entries: dict[str, str] = {}
    for line in lines:
        match = MANIFEST_LINE.fullmatch(line)
        if match is None:
            raise ShaderError(f"invalid shader manifest line: {line!r}")
        digest, name = match.groups()
        if name in entries:
            raise ShaderError(f"duplicate shader manifest entry: {name}")
        entries[name] = digest
    expected = set(EXPECTED_NAMES)
    actual = set(entries)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ShaderError(f"shader manifest membership mismatch: missing={missing}, extra={extra}")
    return entries


def header_bytes(directory: Path, manifest: Path) -> bytes:
    entries = load_manifest(manifest)
    parts = [
        "/* Generated at build time from validated GPU shader artifacts. */\n",
        "#ifndef GPU_SHADER_DATA_H\n#define GPU_SHADER_DATA_H\n#include <stddef.h>\n",
    ]
    for name in EXPECTED_NAMES:
        path = directory / name
        if not path.is_file() or path.is_symlink():
            raise ShaderError(f"missing regular generated shader: {path}")
        actual = sha256(path)
        if actual != entries[name]:
            raise ShaderError(
                f"generated shader digest mismatch for {name}: "
                f"expected {entries[name]}, got {actual}"
            )
        identifier = name.replace(".", "_")
        payload = path.read_bytes()
        encoded = "".join(f"0x{byte:02x}," for byte in payload)
        parts.append(
            f"static const unsigned char gpu_shader_{identifier}[] = {{{encoded}}};\n"
            f"static const size_t gpu_shader_{identifier}_size = "
            f"sizeof(gpu_shader_{identifier});\n"
        )
    parts.append("#endif\n")
    return "".join(parts).encode("ascii")


def write_if_changed(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and not path.is_symlink() and path.read_bytes() == payload:
        return
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.",
                                     delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(payload)
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        payload = header_bytes(arguments.input, arguments.manifest)
        write_if_changed(arguments.output, payload)
    except (OSError, ShaderError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
