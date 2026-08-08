#!/usr/bin/env python3
"""Validate and index the complete downloadable classic release set."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import fnmatch
import json
from pathlib import Path, PurePosixPath
import re
import tarfile
import zipfile

from locked_inputs import load_locked_inputs


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
        required_paths = {
            f"{root}/VERSION",
            f"{root}/LICENSE.md",
            f"{root}/ATTRIBUTIONS.md",
        }
        history_root = (
            "docs/history"
            if package == "atrinik-classic"
            else "PROVENANCE/history"
        )
        required_paths.update(
            f"{root}/{history_root}/{name}"
            for name in (
                "imports.json",
                "release-tags.json",
                "component-release-map.json",
            )
        )
        packaged_dependencies = {
            "atrinik-classic-client": ("protocol", "libatrinik"),
            "atrinik-classic-server": ("protocol", "libatrinik"),
            "atrinik-classic-libatrinik": ("protocol",),
        }
        for dependency in packaged_dependencies.get(package, ()):
            required_paths.update(
                {
                    f"{root}/dependencies/{dependency}/CMakeLists.txt",
                    f"{root}/dependencies/{dependency}/VERSION",
                }
            )
        for required in sorted(required_paths):
            if required not in names:
                raise RuntimeError(f"{path.name} is missing {required}")
        version_file = archive.extractfile(f"{root}/VERSION")
        assert version_file is not None
        if version_file.read().decode().strip() != version:
            raise RuntimeError(f"{path.name} contains the wrong VERSION")
        for dependency in packaged_dependencies.get(package, ()):
            dependency_version_file = archive.extractfile(
                f"{root}/dependencies/{dependency}/VERSION"
            )
            assert dependency_version_file is not None
            if dependency_version_file.read().decode().strip() != version:
                raise RuntimeError(
                    f"{path.name} contains the wrong {dependency} VERSION"
                )


def validate_zip(
    path: Path,
    package_root: str,
    required_patterns: tuple[str, ...],
) -> None:
    with zipfile.ZipFile(path) as archive:
        if not archive.infolist():
            raise RuntimeError(f"empty ZIP artifact: {path.name}")
        files: dict[str, int] = {}
        for member in archive.infolist():
            validate_member(member.filename)
            if not member.is_dir():
                prefix = f"{package_root}/"
                if not member.filename.startswith(prefix):
                    raise RuntimeError(
                        f"{path.name} contains an unexpected root: {member.filename}"
                    )
                relative = member.filename.removeprefix(prefix)
                files[relative] = member.file_size
        for pattern in required_patterns:
            matches = [
                size
                for name, size in files.items()
                if fnmatch.fnmatchcase(name, pattern)
            ]
            if not matches:
                raise RuntimeError(f"{path.name} is missing packaged {pattern}")
            if not any(size > 0 for size in matches):
                raise RuntimeError(f"{path.name} has only empty packaged {pattern}")


def validate_wheel(path: Path, version: str) -> None:
    with zipfile.ZipFile(path) as archive:
        metadata_names = [name for name in archive.namelist() if name.endswith(".dist-info/METADATA")]
        if len(metadata_names) != 1:
            raise RuntimeError(f"{path.name} has no unique wheel METADATA")
        metadata = archive.read(metadata_names[0]).decode("utf-8")
    if "Name: atrinik-classic-protocol\n" not in metadata:
        raise RuntimeError(f"{path.name} has the wrong distribution name")
    if f"Version: {version}\n" not in metadata:
        raise RuntimeError(f"{path.name} has the wrong distribution version")
    if "License-Expression: GPL-2.0-or-later\n" not in metadata:
        raise RuntimeError(f"{path.name} has the wrong license expression")
    expected_metadata = (
        f"atrinik_classic_protocol-{version}.dist-info/METADATA"
    )
    if metadata_names[0] != expected_metadata:
        raise RuntimeError(f"{path.name} has the wrong dist-info directory")
    license_path = (
        f"atrinik_classic_protocol-{version}.dist-info/licenses/LICENSE.md"
    )
    with zipfile.ZipFile(path) as archive:
        required = (
            "atrinik_protocol/__init__.py",
            "atrinik_protocol/game.py",
            expected_metadata,
            f"atrinik_classic_protocol-{version}.dist-info/WHEEL",
            f"atrinik_classic_protocol-{version}.dist-info/RECORD",
            license_path,
        )
        for name in required:
            if name not in archive.namelist() or not archive.read(name):
                raise RuntimeError(f"{path.name} has no packaged {name}")


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


def build_spdx(
    paths: list[Path],
    version: str,
    revision: str,
    source_epoch: int,
    locked_inputs: list[dict[str, object]],
) -> dict[str, object]:
    created = datetime.fromtimestamp(source_epoch, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    packages = []
    relationships = []
    artifact_ids: dict[str, str] = {}
    for index, path in enumerate(paths, start=1):
        identifier = f"SPDXRef-Artifact-{index}"
        artifact_ids[path.name] = identifier
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
    for record in locked_inputs:
        name = str(record["name"])
        identifier = f"SPDXRef-LockedInput-{re.sub(r'[^A-Za-z0-9.-]', '-', name)}"
        packages.append(
            {
                "SPDXID": identifier,
                "name": name,
                "versionInfo": str(record["tag"]),
                "downloadLocation": str(record["url"]),
                "filesAnalyzed": False,
                "checksums": [
                    {"algorithm": "SHA256", "checksumValue": record["sha256"]}
                ],
                "externalRefs": [
                    {
                        "referenceCategory": "PACKAGE-MANAGER",
                        "referenceType": "purl",
                        "referenceLocator": (
                            f"pkg:github/{record['repository']}@{record['commit']}"
                        ),
                    }
                ],
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "NOASSERTION",
                "copyrightText": "NOASSERTION",
                "sourceInfo": f"Locked by {record['lock']}",
            }
        )
        for affected in record["affects"]:
            artifact_id = artifact_ids.get(str(affected))
            if artifact_id is not None:
                relationships.append(
                    {
                        "spdxElementId": artifact_id,
                        "relationshipType": "DEPENDS_ON",
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
    locked_inputs = load_locked_inputs(arguments.version)
    names = {path.name for path in paths}
    required = expected_names(arguments.version)
    wheel_name = f"atrinik_classic_protocol-{arguments.version}-py3-none-any.whl"
    wheel_path = directory / wheel_name
    if not wheel_path.is_file():
        raise RuntimeError("release set has no exact py3-none-any classic protocol wheel")
    required.add(wheel_name)
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
    validate_zip(
        directory / f"atrinik-classic-client-{arguments.version}-windows-x86_64.zip",
        f"atrinik-classic-client-{arguments.version}-windows-x86_64",
        (
            "atrinik.exe",
            "client.cfg",
            "ca-bundle.crt",
            "LICENSE.md",
            "ATTRIBUTIONS.md",
            "SDL3.dll",
            "SDL3_image.dll",
            "SDL3_mixer.dll",
            "SDL3_ttf.dll",
            "fonts/*",
            "textures/*",
            "data/*",
            "settings/*",
            "sound/*",
        ),
    )
    validate_zip(
        directory / f"atrinik-classic-server-{arguments.version}-windows-x86_64.zip",
        f"atrinik-classic-server-{arguments.version}-windows-x86_64",
        (
            "server/atrinik-server.exe",
            "server/LICENSE.md",
            "server/ATTRIBUTIONS.md",
            "server/server.cfg",
            "server/permissions.cfg",
            "server/ca-bundle.crt",
            "server/*plugin_arena*.dll",
            "server/*plugin_python*.dll",
            "server/python*.dll",
            "server/Lib/*",
            "maps/*",
            "server/lib/*",
            "server/resources/*",
            "server/install_data/*",
            "server/install_data/http/client-maps/*",
        ),
    )
    validate_wheel(wheel_path, arguments.version)

    sbom_path = directory / f"atrinik-classic-{arguments.version}.spdx.json"
    sbom_path.write_text(
        json.dumps(
            build_spdx(
                paths,
                arguments.version,
                arguments.revision,
                arguments.source_epoch,
                locked_inputs,
            ),
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
                "locked_inputs": locked_inputs,
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
