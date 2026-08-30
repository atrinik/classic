#!/usr/bin/env python3
"""Write the deterministic manifest for one complete GPU shader cohort."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import tempfile


STAGES = (
    "final_fragment",
    "final_vertex",
    "light_fragment",
    "light_vertex",
    "world_fragment",
    "world_vertex",
)
FORMATS = ("dxil", "msl", "spv")
EXPECTED_NAMES = tuple(
    f"{stage}.{shader_format}" for shader_format in FORMATS for stage in STAGES
)


class ManifestError(RuntimeError):
    """The generated shader cohort cannot be represented safely."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def manifest_bytes(directory: Path) -> bytes:
    lines = []
    for name in EXPECTED_NAMES:
        path = directory / name
        if not path.is_file() or path.is_symlink():
            raise ManifestError(f"missing regular generated shader: {path}")
        lines.append(f"{sha256(path)}  ./{name}\n")
    return "".join(lines).encode("ascii")


def write_manifest(directory: Path, output: Path) -> None:
    payload = manifest_bytes(directory)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        dir=output.parent, prefix=f".{output.name}.", delete=False
    ) as stream:
        temporary = Path(stream.name)
        stream.write(payload)
    temporary.replace(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        write_manifest(arguments.input, arguments.output)
    except (ManifestError, OSError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
