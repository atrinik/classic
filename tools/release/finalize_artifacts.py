#!/usr/bin/env python3
"""Validate and index the complete downloadable classic release set."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import tarfile
import zipfile


VERSION_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
MODULES = ("client", "server", "editor", "libatrinik", "protocol")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_member(name: str) -> None:
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise RuntimeError(f"unsafe packaged path: {name}")


def validate_source_archive(path: Path, package: str, version: str) -> None:
    root = f"{package}-{version}"
    with tarfile.open(path, "r:gz") as archive:
        members = archive.getmembers()
        if not members:
            raise RuntimeError(f"empty source archive: {path.name}")
        for member in members:
            validate_member(member.name)
            if member.name != root and not member.name.startswith(f"{root}/"):
                raise RuntimeError(f"{path.name} contains an unexpected root: {member.name}")
        names = {member.name for member in members}
        for required in (f"{root}/VERSION", f"{root}/LICENSE.md"):
            if required not in names:
                raise RuntimeError(f"{path.name} is missing {required}")
        version_file = archive.extractfile(f"{root}/VERSION")
        assert version_file is not None
        if version_file.read().decode().strip() != version:
            raise RuntimeError(f"{path.name} contains the wrong VERSION")


def validate_zip(path: Path) -> None:
    with zipfile.ZipFile(path) as archive:
        if not archive.infolist():
            raise RuntimeError(f"empty ZIP artifact: {path.name}")
        for member in archive.infolist():
            validate_member(member.filename)


def validate_wheel(path: Path, version: str) -> None:
    validate_zip(path)
    with zipfile.ZipFile(path) as archive:
        metadata_names = [name for name in archive.namelist() if name.endswith(".dist-info/METADATA")]
        if len(metadata_names) != 1:
            raise RuntimeError(f"{path.name} has no unique wheel METADATA")
        metadata = archive.read(metadata_names[0]).decode("utf-8")
    if "Name: atrinik-classic-protocol\n" not in metadata:
        raise RuntimeError(f"{path.name} has the wrong distribution name")
    if f"Version: {version}\n" not in metadata:
        raise RuntimeError(f"{path.name} has the wrong distribution version")


def expected_names(version: str) -> set[str]:
    names = {f"atrinik-classic-{version}.tar.gz"}
    names.update(f"atrinik-classic-{module}-{version}.tar.gz" for module in MODULES)
    names.update(
        {
            f"atrinik-classic-client-{version}-windows-x86_64.zip",
            f"atrinik-classic-server-{version}-windows-x86_64.zip",
        }
    )
    return names


def build_spdx(paths: list[Path], version: str, revision: str, source_epoch: int) -> dict[str, object]:
    created = datetime.fromtimestamp(source_epoch, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    packages = []
    relationships = []
    for index, path in enumerate(paths, start=1):
        identifier = f"SPDXRef-Artifact-{index}"
        packages.append(
            {
                "SPDXID": identifier,
                "name": path.name,
                "versionInfo": version,
                "downloadLocation": (
                    f"https://github.com/atrinik/classic/releases/download/v{version}/{path.name}"
                ),
                "filesAnalyzed": False,
                "checksums": [{"algorithm": "SHA256", "checksumValue": sha256(path)}],
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "NOASSERTION",
                "copyrightText": "NOASSERTION",
                "sourceInfo": f"Built from atrinik/classic commit {revision}",
            }
        )
        relationships.append(
            {
                "spdxElementId": "SPDXRef-DOCUMENT",
                "relationshipType": "DESCRIBES",
                "relatedSpdxElement": identifier,
            }
        )
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"Atrinik Classic {version} release artifacts",
        "documentNamespace": f"https://github.com/atrinik/classic/releases/tag/v{version}/sbom/{revision}",
        "creationInfo": {
            "created": created,
            "creators": ["Tool: atrinik-classic-release-finalizer-1"],
        },
        "packages": packages,
        "relationships": relationships,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--source-epoch", type=int, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    arguments = parser.parse_args()
    if not VERSION_RE.fullmatch(arguments.version):
        parser.error("--version must be MAJOR.MINOR.PATCH")
    if not re.fullmatch(r"[0-9a-f]{40}", arguments.revision):
        parser.error("--revision must be a full lowercase commit ID")
    if arguments.source_epoch < 0:
        parser.error("--source-epoch must not be negative")

    directory = arguments.directory.resolve(strict=True)
    generated = {
        "SHA256SUMS",
        "release-manifest.json",
        f"atrinik-classic-{arguments.version}.spdx.json",
    }
    for name in generated:
        if (directory / name).exists():
            raise RuntimeError(f"refusing to overwrite release metadata: {name}")

    paths = sorted(path for path in directory.iterdir() if path.is_file())
    names = {path.name for path in paths}
    required = expected_names(arguments.version)
    wheel_pattern = re.compile(
        rf"atrinik_classic_protocol-{re.escape(arguments.version)}-[^-]+-[^-]+-[^.]+\.whl"
    )
    wheels = [path for path in paths if wheel_pattern.fullmatch(path.name)]
    if len(wheels) != 1:
        raise RuntimeError("release set must contain exactly one classic protocol wheel")
    required.add(wheels[0].name)
    if names != required:
        missing = sorted(required - names)
        extra = sorted(names - required)
        raise RuntimeError(f"release artifact set differs: missing={missing}, extra={extra}")

    validate_source_archive(
        directory / f"atrinik-classic-{arguments.version}.tar.gz",
        "atrinik-classic",
        arguments.version,
    )
    for module in MODULES:
        validate_source_archive(
            directory / f"atrinik-classic-{module}-{arguments.version}.tar.gz",
            f"atrinik-classic-{module}",
            arguments.version,
        )
    validate_zip(directory / f"atrinik-classic-client-{arguments.version}-windows-x86_64.zip")
    validate_zip(directory / f"atrinik-classic-server-{arguments.version}-windows-x86_64.zip")
    validate_wheel(wheels[0], arguments.version)

    sbom_path = directory / f"atrinik-classic-{arguments.version}.spdx.json"
    sbom_path.write_text(
        json.dumps(
            build_spdx(paths, arguments.version, arguments.revision, arguments.source_epoch),
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    indexed_paths = sorted([*paths, sbom_path], key=lambda path: path.name)
    manifest_path = directory / "release-manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "tag": f"v{arguments.version}",
                "version": arguments.version,
                "revision": arguments.revision,
                "source_epoch": arguments.source_epoch,
                "artifacts": [
                    {"name": path.name, "sha256": sha256(path), "size": path.stat().st_size}
                    for path in indexed_paths
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    checksum_paths = sorted([*indexed_paths, manifest_path], key=lambda path: path.name)
    (directory / "SHA256SUMS").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in checksum_paths),
        encoding="utf-8",
    )
    for path in sorted(directory.iterdir()):
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
