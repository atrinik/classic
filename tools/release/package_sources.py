#!/usr/bin/env python3
"""Build deterministic source archives for a unified classic release."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[2]
VERSION_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
MODULES = ("client", "server", "editor", "libatrinik", "protocol")
SHARED_PATHS = ("LICENSE.md", "ATTRIBUTIONS.md")
PROVENANCE_PREFIX = "docs/history/"


class PackageError(RuntimeError):
    """Raised when a release archive cannot be built safely."""


def git(*arguments: str, text: bool = True) -> str | bytes:
    result = subprocess.run(
        ["git", "-C", str(ROOT), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=text,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise PackageError(f"git {' '.join(arguments)}: {detail}")
    return result.stdout.strip() if text else result.stdout


def resolve_revision(revision: str) -> tuple[str, int]:
    commit = str(git("rev-parse", "--verify", f"{revision}^{{commit}}"))
    timestamp = int(str(git("show", "-s", "--format=%ct", commit)))
    return commit, timestamp


def normalized_member(member: tarfile.TarInfo, name: str, timestamp: int) -> tarfile.TarInfo:
    value = tarfile.TarInfo(name)
    value.size = member.size
    value.mode = member.mode
    value.type = member.type
    value.linkname = member.linkname
    value.mtime = timestamp
    value.uid = 0
    value.gid = 0
    value.uname = ""
    value.gname = ""
    value.pax_headers = {}
    return value


def selected_name(scope: str, source_name: str, package: str) -> str | None:
    path = PurePosixPath(source_name)
    if path.is_absolute() or ".." in path.parts:
        raise PackageError(f"unsafe archive path: {source_name}")

    if scope == "root":
        return f"{package}/{source_name}"

    module_prefix = f"{scope}/"
    if source_name == scope:
        return package
    if source_name.startswith(module_prefix):
        return f"{package}/{source_name[len(module_prefix):]}"
    if source_name in SHARED_PATHS:
        return f"{package}/{source_name}"
    if source_name.startswith(PROVENANCE_PREFIX):
        relative = source_name[len(PROVENANCE_PREFIX) :]
        return f"{package}/PROVENANCE/history/{relative}"
    return None


def add_bytes(archive: tarfile.TarFile, name: str, data: bytes, timestamp: int) -> None:
    member = tarfile.TarInfo(name)
    member.size = len(data)
    member.mode = 0o644
    member.mtime = timestamp
    member.uid = 0
    member.gid = 0
    archive.addfile(member, io.BytesIO(data))


def build_archive(
    source_archive: Path,
    output: Path,
    scope: str,
    version: str,
    timestamp: int,
) -> None:
    package = "atrinik-classic" if scope == "root" else f"atrinik-classic-{scope}"
    package = f"{package}-{version}"
    if output.exists():
        raise PackageError(f"refusing to overwrite release artifact: {output}")

    seen: set[str] = set()
    with source_archive.open("rb") as source_file:
        with tarfile.open(fileobj=source_file, mode="r:") as source:
            with output.open("xb") as raw_output:
                with gzip.GzipFile(filename="", mode="wb", fileobj=raw_output, mtime=timestamp) as compressed:
                    with tarfile.open(fileobj=compressed, mode="w", format=tarfile.GNU_FORMAT) as target:
                        for original in source:
                            name = selected_name(scope, original.name, package)
                            if name is None:
                                continue
                            if name in seen:
                                raise PackageError(f"duplicate archive path: {name}")
                            seen.add(name)
                            member = normalized_member(original, name, timestamp)
                            payload = source.extractfile(original) if original.isfile() else None
                            target.addfile(member, payload)

                        version_path = f"{package}/VERSION"
                        if version_path in seen:
                            raise PackageError(f"tracked file conflicts with generated {version_path}")
                        add_bytes(target, version_path, f"{version}\n".encode(), timestamp)

    if not seen:
        raise PackageError(f"scope {scope} selected no tracked files")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--scope",
        action="append",
        choices=("root", *MODULES),
        help="build only this scope; repeat for multiple scopes",
    )
    arguments = parser.parse_args()

    if not VERSION_RE.fullmatch(arguments.version):
        parser.error("--version must be MAJOR.MINOR.PATCH")

    commit, timestamp = resolve_revision(arguments.revision)
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    scopes = tuple(arguments.scope) if arguments.scope else ("root", *MODULES)
    if len(scopes) != len(set(scopes)):
        parser.error("--scope values must be unique")
    paths: list[Path] = []

    with tempfile.TemporaryDirectory(prefix="atrinik-classic-source-") as temporary:
        source_archive = Path(temporary) / "repository.tar"
        with source_archive.open("xb") as stream:
            result = subprocess.run(
                ["git", "-C", str(ROOT), "archive", "--format=tar", commit],
                check=False,
                stdout=stream,
                stderr=subprocess.PIPE,
            )
        if result.returncode:
            raise PackageError(f"git archive: {result.stderr.decode().strip()}")

        for scope in scopes:
            stem = "atrinik-classic" if scope == "root" else f"atrinik-classic-{scope}"
            path = output / f"{stem}-{arguments.version}.tar.gz"
            build_archive(source_archive, path, scope, arguments.version, timestamp)
            paths.append(path)

    manifest = {
        "schema_version": 1,
        "version": arguments.version,
        "revision": commit,
        "artifacts": [
            {"name": path.name, "sha256": sha256(path), "size": path.stat().st_size}
            for path in paths
        ],
    }
    manifest_path = output / "source-artifacts.json"
    if manifest_path.exists():
        raise PackageError(f"refusing to overwrite release artifact: {manifest_path}")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    for path in paths:
        print(path)
    print(manifest_path)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PackageError as error:
        raise SystemExit(f"package sources: {error}") from error
