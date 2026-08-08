#!/usr/bin/env python3
"""Build the classic protocol wheel from a scoped release source archive."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile


VERSION_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")


def safe_extract(archive: tarfile.TarFile, destination: Path) -> None:
    root = destination.resolve()
    for member in archive.getmembers():
        target = (destination / member.name).resolve()
        if target != root and root not in target.parents:
            raise RuntimeError(f"unsafe archive member: {member.name}")
    archive.extractall(destination, filter="data")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-epoch", type=int, required=True)
    arguments = parser.parse_args()
    if not VERSION_RE.fullmatch(arguments.version):
        parser.error("--version must be MAJOR.MINOR.PATCH")
    if arguments.source_epoch < 0:
        parser.error("--source-epoch must not be negative")

    source = arguments.source.resolve(strict=True)
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    expected = f"atrinik_classic_protocol-{arguments.version}-"
    existing = [path for path in output.glob(f"{expected}*.whl")]
    if existing:
        raise RuntimeError(f"refusing to overwrite protocol wheel: {existing[0]}")
    before = {path.name for path in output.glob("*.whl")}

    with tempfile.TemporaryDirectory(prefix="atrinik-classic-protocol-wheel-") as value:
        temporary = Path(value)
        with tarfile.open(source, "r:gz") as archive:
            roots = {Path(member.name).parts[0] for member in archive if member.name}
            if len(roots) != 1:
                raise RuntimeError("protocol source archive must have one root directory")
        with tarfile.open(source, "r:gz") as archive:
            safe_extract(archive, temporary)
        project = temporary / roots.pop()
        environment = os.environ.copy()
        environment.update(
            {
                "SOURCE_DATE_EPOCH": str(arguments.source_epoch),
                "SETUPTOOLS_SCM_PRETEND_VERSION": arguments.version,
            }
        )
        subprocess.run(
            ["python3", "-m", "build", "--wheel", "--outdir", str(output)],
            cwd=project,
            env=environment,
            check=True,
        )

    created = [path for path in output.glob("*.whl") if path.name not in before]
    if len(created) != 1 or not created[0].name.startswith(expected):
        names = ", ".join(path.name for path in created) or "none"
        raise RuntimeError(f"expected one {expected} wheel, found: {names}")
    print(created[0])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
