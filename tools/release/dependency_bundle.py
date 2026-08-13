#!/usr/bin/env python3
"""Build, verify, and install Classic's durable immutable dependency bundle."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
from typing import Iterable


SCHEMA_VERSION = 1
IMAGE = "ghcr.io/atrinik/classic-dependencies"
OCI_IMAGE_MANIFEST = "application/vnd.oci.image.manifest.v1+json"
OCI_IMAGE_CONFIG = "application/vnd.oci.image.config.v1+json"
OCI_LAYER = "application/vnd.oci.image.layer.v1.tar+gzip"
ROOT = Path(__file__).resolve().parents[2]
LOCK_PATHS = (
    Path("client/dependencies.lock.json"),
    Path("server/dependencies.lock.json"),
    Path("server/cmake/immutable_sources.lock.json"),
)
ACQUISITION_CONTRACT_PATHS = (
    Path("server/tools/dependencies.py"),
    Path("tools/release/dependency_bundle.py"),
)
EXPECTED_NAMES = ("content", "libpcpnatpmp", "resources", "sound")


class BundleError(RuntimeError):
    """The durable dependency bundle is missing, stale, or malformed."""


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, object]:
    try:
        with path.open(encoding="utf-8") as stream:
            value = json.load(stream, object_pairs_hook=reject_duplicate_keys)
    except (OSError, json.JSONDecodeError) as error:
        raise BundleError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise BundleError(f"{path} must contain a JSON object")
    return value


def reject_duplicate_keys(pairs: Iterable[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise BundleError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def require_keys(value: dict[str, object], expected: set[str], context: str) -> None:
    if set(value) != expected:
        raise BundleError(
            f"{context} has unexpected fields: expected {sorted(expected)}, got {sorted(value)}"
        )


def dependency_module(root: Path):
    path = root / "server" / "tools" / "dependencies.py"
    specification = importlib.util.spec_from_file_location("classic_dependencies", path)
    if specification is None or specification.loader is None:
        raise BundleError(f"cannot load authoritative dependency fetcher: {path}")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def locked_material(root: Path) -> tuple[dict[str, str], list[dict[str, object]]]:
    fetcher = dependency_module(root)
    lock_digests = {
        path.as_posix(): sha256_file(root / path)
        for path in LOCK_PATHS
    }
    records = fetcher.bundle_materials(
        root / "client" / "dependencies.lock.json",
        root / "server" / "dependencies.lock.json",
        root / "server" / "cmake" / "immutable_sources.lock.json",
    )
    records.sort(key=lambda item: str(item["name"]))
    if tuple(record["name"] for record in records) != EXPECTED_NAMES:
        raise BundleError(
            "locked dependency set must be content, libpcpnatpmp, resources, and sound"
        )
    return lock_digests, records


def material_document(root: Path) -> dict[str, object]:
    lock_digests, records = locked_material(root)
    fetcher = dependency_module(root)
    verified_inputs = fetcher.bundle_materials(
        root / "client" / "dependencies.lock.json",
        root / "server" / "dependencies.lock.json",
        root / "server" / "cmake" / "immutable_sources.lock.json",
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "source_locks": lock_digests,
        "acquisition_contracts": {
            path.as_posix(): sha256_file(root / path)
            for path in ACQUISITION_CONTRACT_PATHS
        },
        "verified_input_bundle_digest": (
            "sha256:" + fetcher.bundle_digest(verified_inputs)
        ),
        "inputs": records,
    }


def material_digest(root: Path) -> str:
    return "sha256:" + sha256_bytes(canonical_json(material_document(root)))


def archive_name(record: dict[str, object]) -> str:
    return f"{record['name']}-{record['sha256']}.tar.gz"


def acquire_archives(
    root: Path, cache: Path, staging: Path
) -> list[tuple[dict[str, object], Path]]:
    fetcher = dependency_module(root)
    _locks, records = locked_material(root)
    fetcher.stage_bundle(records, cache, staging)
    fetcher.verify_bundle(records, staging)
    return [
        (record, staging / "downloads" / archive_name(record))
        for record in records
    ]


def provenance_document(material: dict[str, object]) -> dict[str, object]:
    dependencies = []
    subjects = []
    inputs = material["inputs"]
    assert isinstance(inputs, list)
    for record in inputs:
        assert isinstance(record, dict)
        dependencies.append(
            {
                "uri": record["url"],
                "digest": {"sha256": record["sha256"]},
                "annotations": {
                    "name": record["name"],
                    "kind": record["kind"],
                },
            }
        )
        subjects.append(
            {
                "name": archive_name(record),
                "digest": {"sha256": record["sha256"]},
            }
        )
    return {
        "_type": "https://in-toto.io/Statement/v1",
        "subject": subjects,
        "predicateType": "https://slsa.dev/provenance/v1",
        "predicate": {
            "buildDefinition": {
                "buildType": "https://atrinik.org/buildtypes/classic-dependency-bundle/v1",
                "externalParameters": {
                    "source_locks": material["source_locks"],
                    "material_digest": "sha256:" + sha256_bytes(canonical_json(material)),
                },
                "resolvedDependencies": dependencies,
            },
            "runDetails": {
                "builder": {"id": "https://github.com/atrinik/classic/actions"},
                "metadata": {"invocationId": "deterministic-local-or-trusted-workflow"},
            },
        },
    }


def write_layer(bundle: Path, output: Path) -> tuple[bytes, str]:
    with tempfile.NamedTemporaryFile() as uncompressed:
        with tarfile.open(
            fileobj=uncompressed, mode="w", format=tarfile.GNU_FORMAT
        ) as archive:
            for path in sorted(item for item in bundle.rglob("*") if item.is_file()):
                relative = Path("bundle") / path.relative_to(bundle)
                info = archive.gettarinfo(str(path), arcname=relative.as_posix())
                info.uid = 0
                info.gid = 0
                info.uname = ""
                info.gname = ""
                info.mtime = 0
                info.mode = 0o644
                info.pax_headers = {}
                with path.open("rb") as stream:
                    archive.addfile(info, stream)
        uncompressed.flush()
        uncompressed.seek(0)
        raw = uncompressed.read()
    with output.open("wb") as stream:
        with gzip.GzipFile(fileobj=stream, mode="wb", filename="", mtime=0) as compressed:
            compressed.write(raw)
    return output.read_bytes(), sha256_bytes(raw)


def descriptor_for(
    root: Path, manifest_digest: str, bundle_manifest: bytes
) -> dict[str, object]:
    digest = material_digest(root)
    return {
        "schema_version": SCHEMA_VERSION,
        "image": IMAGE,
        "tag": f"materials-{digest.removeprefix('sha256:')}",
        "digest": manifest_digest,
        "material_digest": digest,
        "bundle_manifest_sha256": "sha256:" + sha256_bytes(bundle_manifest),
        "source_locks": material_document(root)["source_locks"],
        "acquisition_contracts": material_document(root)["acquisition_contracts"],
        "verified_input_bundle_digest": material_document(root)[
            "verified_input_bundle_digest"
        ],
    }


def build_layout(root: Path, cache: Path, output: Path) -> dict[str, object]:
    if output.exists() and any(output.iterdir()):
        raise BundleError(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    material = material_document(root)
    staged_inputs = output / "staged-dependency-inputs"
    acquired = acquire_archives(root, cache, staged_inputs)
    bundle = output / "bundle"
    archives = bundle / "archives"
    archives.mkdir(parents=True)
    artifact_records = []
    for record, source in acquired:
        name = archive_name(record)
        destination = archives / name
        shutil.copyfile(source, destination)
        artifact_records.append(
            {
                "name": record["name"],
                "path": f"archives/{name}",
                "sha256": record["sha256"],
                "size": destination.stat().st_size,
            }
        )
    bundle_document = {
        "schema_version": SCHEMA_VERSION,
        "material_digest": "sha256:" + sha256_bytes(canonical_json(material)),
        "source_locks": material["source_locks"],
        "acquisition_contracts": material["acquisition_contracts"],
        "verified_input_bundle_digest": material[
            "verified_input_bundle_digest"
        ],
        "inputs": material["inputs"],
        "artifacts": artifact_records,
    }
    bundle_manifest = canonical_json(bundle_document)
    (bundle / "manifest.json").write_bytes(bundle_manifest)
    (bundle / "provenance.json").write_bytes(
        canonical_json(provenance_document(material))
    )
    shutil.rmtree(staged_inputs)

    blobs = output / "blobs" / "sha256"
    blobs.mkdir(parents=True)
    layer_path = output / "layer.tar.gz"
    layer, diff_id = write_layer(bundle, layer_path)
    layer_digest = sha256_bytes(layer)
    shutil.move(layer_path, blobs / layer_digest)
    config = canonical_json(
        {
            "architecture": "amd64",
            "os": "linux",
            "config": {
                "Labels": {
                    "org.opencontainers.image.source": (
                        "https://github.com/atrinik/classic"
                    ),
                    "org.opencontainers.image.title": (
                        "Atrinik Classic dependency bundle"
                    ),
                    "org.atrinik.dependencies.material-digest": (
                        bundle_document["material_digest"]
                    ),
                }
            },
            "rootfs": {"type": "layers", "diff_ids": [f"sha256:{diff_id}"]},
            "history": [{"created_by": "tools/release/dependency_bundle.py"}],
        }
    )
    config_digest = sha256_bytes(config)
    (blobs / config_digest).write_bytes(config)
    manifest = canonical_json(
        {
            "schemaVersion": 2,
            "mediaType": OCI_IMAGE_MANIFEST,
            "config": {
                "mediaType": OCI_IMAGE_CONFIG,
                "digest": f"sha256:{config_digest}",
                "size": len(config),
            },
            "layers": [
                {
                    "mediaType": OCI_LAYER,
                    "digest": f"sha256:{layer_digest}",
                    "size": len(layer),
                    "annotations": {
                        "org.opencontainers.image.title": (
                            "classic-dependencies.tar.gz"
                        )
                    },
                }
            ],
            "annotations": {
                "org.opencontainers.image.source": "https://github.com/atrinik/classic",
                "org.opencontainers.image.title": "Atrinik Classic dependency bundle",
                "org.atrinik.dependencies.material-digest": (
                    bundle_document["material_digest"]
                ),
            },
        }
    )
    manifest_digest = sha256_bytes(manifest)
    (blobs / manifest_digest).write_bytes(manifest)
    descriptor = descriptor_for(root, f"sha256:{manifest_digest}", bundle_manifest)
    index = canonical_json(
        {
            "schemaVersion": 2,
            "manifests": [
                {
                    "mediaType": OCI_IMAGE_MANIFEST,
                    "digest": descriptor["digest"],
                    "size": len(manifest),
                    "annotations": {
                        "org.opencontainers.image.ref.name": descriptor["tag"]
                    },
                }
            ],
        }
    )
    (output / "index.json").write_bytes(index)
    (output / "oci-layout").write_bytes(
        canonical_json({"imageLayoutVersion": "1.0.0"})
    )
    shutil.rmtree(bundle)
    return descriptor


def load_descriptor(path: Path) -> dict[str, object]:
    descriptor = load_json(path)
    require_keys(
        descriptor,
        {
            "schema_version", "image", "tag", "digest", "material_digest",
            "bundle_manifest_sha256", "source_locks", "acquisition_contracts",
            "verified_input_bundle_digest",
        },
        "dependency bundle descriptor",
    )
    if descriptor["schema_version"] != SCHEMA_VERSION or descriptor["image"] != IMAGE:
        raise BundleError("dependency bundle descriptor has unsupported coordinates")
    for field in (
        "digest",
        "material_digest",
        "bundle_manifest_sha256",
        "verified_input_bundle_digest",
    ):
        value = descriptor[field]
        if (
            not isinstance(value, str)
            or not value.startswith("sha256:")
            or len(value) != 71
            or any(character not in "0123456789abcdef" for character in value[7:])
        ):
            raise BundleError(f"dependency bundle descriptor {field} is not SHA-256")
    expected_tag = (
        "materials-"
        + str(descriptor["material_digest"]).removeprefix("sha256:")
    )
    if descriptor["tag"] != expected_tag:
        raise BundleError("dependency bundle descriptor tag does not match its materials")
    return descriptor


def verify_descriptor(root: Path, descriptor: dict[str, object]) -> None:
    current = material_document(root)
    current_digest = "sha256:" + sha256_bytes(canonical_json(current))
    if descriptor["material_digest"] != current_digest:
        raise BundleError(
            "dependency bundle is stale for the current locks; run the trusted bundle publication workflow"
        )
    if descriptor["source_locks"] != current["source_locks"]:
        raise BundleError("dependency bundle source-lock digests do not match the current checkout")
    if descriptor["acquisition_contracts"] != current["acquisition_contracts"]:
        raise BundleError(
            "dependency bundle acquisition contracts do not match the current checkout"
        )
    if (
        descriptor["verified_input_bundle_digest"]
        != current["verified_input_bundle_digest"]
    ):
        raise BundleError(
            "dependency bundle does not match the verified CI input-bundle contract"
        )


def verify_bundle(
    root: Path, descriptor_path: Path, bundle: Path
) -> dict[str, object]:
    if bundle.is_symlink():
        raise BundleError("dependency bundle root must not be a symbolic link")
    descriptor = load_descriptor(descriptor_path)
    verify_descriptor(root, descriptor)
    manifest_path = bundle / "manifest.json"
    if (
        "sha256:" + sha256_file(manifest_path)
        != descriptor["bundle_manifest_sha256"]
    ):
        raise BundleError("dependency bundle manifest does not match the checked descriptor")
    manifest = load_json(manifest_path)
    require_keys(
        manifest,
        {
            "schema_version", "material_digest", "source_locks",
            "acquisition_contracts", "verified_input_bundle_digest", "inputs",
            "artifacts",
        },
        "dependency bundle manifest",
    )
    expected_material = material_document(root)
    if (
        manifest["schema_version"] != SCHEMA_VERSION
        or manifest["material_digest"] != descriptor["material_digest"]
        or manifest["source_locks"] != expected_material["source_locks"]
        or manifest["acquisition_contracts"]
        != expected_material["acquisition_contracts"]
        or manifest["verified_input_bundle_digest"]
        != expected_material["verified_input_bundle_digest"]
        or manifest["inputs"] != expected_material["inputs"]
    ):
        raise BundleError("dependency bundle manifest does not match current locked materials")
    artifacts = manifest["artifacts"]
    if not isinstance(artifacts, list) or len(artifacts) != 4:
        raise BundleError("dependency bundle must contain exactly four artifacts")
    expected_files = {"manifest.json", "provenance.json"}
    expected_digests = {
        str(record["name"]): str(record["sha256"])
        for record in expected_material["inputs"]
        if isinstance(record, dict)
    }
    names = []
    if any(path.is_symlink() for path in bundle.rglob("*")):
        raise BundleError("dependency bundle contains a symbolic link")
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            raise BundleError("dependency bundle artifact record must be an object")
        require_keys(artifact, {"name", "path", "sha256", "size"}, "bundle artifact")
        name = artifact["name"]
        path = artifact["path"]
        digest = artifact["sha256"]
        size = artifact["size"]
        if (
            name not in EXPECTED_NAMES
            or not isinstance(path, str)
            or not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
            or not isinstance(size, int)
            or isinstance(size, bool)
            or size <= 0
            or path != f"archives/{name}-{digest}.tar.gz"
            or expected_digests.get(str(name)) != digest
        ):
            raise BundleError(
                "dependency bundle artifact record does not match its locked input"
            )
        candidate = bundle / path
        if not candidate.is_file() or candidate.is_symlink():
            raise BundleError(f"dependency bundle artifact is missing: {name}")
        if candidate.stat().st_size != size or sha256_file(candidate) != digest:
            raise BundleError(f"dependency bundle artifact failed verification: {name}")
        expected_files.add(path)
        names.append(name)
    if tuple(sorted(names)) != EXPECTED_NAMES:
        raise BundleError("dependency bundle artifact set is incomplete or duplicated")
    actual_files = {
        path.relative_to(bundle).as_posix()
        for path in bundle.rglob("*")
        if path.is_file() or path.is_symlink()
    }
    if actual_files != expected_files:
        raise BundleError("dependency bundle contains missing or unexpected files")
    provenance = load_json(bundle / "provenance.json")
    if provenance != provenance_document(expected_material):
        raise BundleError("dependency bundle provenance does not match current locked materials")
    return manifest


def install_bundle(root: Path, descriptor_path: Path, bundle: Path) -> None:
    manifest = verify_bundle(root, descriptor_path, bundle)
    destinations = {
        "sound": root / "client/build/dependencies/downloads",
        "content": root / "server/build/dependencies/downloads",
        "resources": root / "server/build/dependencies/downloads",
        "libpcpnatpmp": root / "server/build/dependency-cache/downloads",
    }
    artifacts = manifest["artifacts"]
    assert isinstance(artifacts, list)
    for artifact in artifacts:
        assert isinstance(artifact, dict)
        name = str(artifact["name"])
        destination_dir = destinations[name]
        destination_dir.mkdir(parents=True, exist_ok=True)
        destination = destination_dir / f"{name}-{artifact['sha256']}.tar.gz"
        source = bundle / str(artifact["path"])
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{name}-", dir=destination_dir
        )
        os.close(descriptor)
        temporary = Path(temporary_name)
        try:
            shutil.copyfile(source, temporary)
            if sha256_file(temporary) != artifact["sha256"]:
                raise BundleError(f"dependency bundle copy failed verification: {name}")
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
    print(f"installed verified dependency bundle {material_digest(root)}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument(
        "--descriptor", type=Path, default=Path("dependencies.bundle.json")
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    build = subparsers.add_parser("build")
    build.add_argument("--cache", type=Path, required=True)
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--write-descriptor", action="store_true")
    verify = subparsers.add_parser("verify")
    verify.add_argument("--bundle", type=Path, required=True)
    install = subparsers.add_parser("install")
    install.add_argument("--bundle", type=Path, required=True)
    show = subparsers.add_parser("show")
    show.add_argument(
        "--field", choices=("image", "tag", "digest", "material_digest")
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    root = arguments.root.resolve(strict=True)
    descriptor_path = arguments.descriptor
    if not descriptor_path.is_absolute():
        descriptor_path = root / descriptor_path
    try:
        if arguments.command == "build":
            descriptor = build_layout(root, arguments.cache, arguments.output)
            if arguments.write_descriptor:
                descriptor_path.write_bytes(
                    json.dumps(descriptor, indent=2).encode() + b"\n"
                )
            elif descriptor != load_descriptor(descriptor_path):
                raise BundleError(
                    "rebuilt OCI digest differs from dependencies.bundle.json; regenerate the descriptor"
                )
            print(json.dumps(descriptor, sort_keys=True))
        elif arguments.command == "verify":
            verify_bundle(root, descriptor_path, arguments.bundle)
            print(f"verified dependency bundle {material_digest(root)}")
        elif arguments.command == "install":
            install_bundle(root, descriptor_path, arguments.bundle)
        else:
            descriptor = load_descriptor(descriptor_path)
            verify_descriptor(root, descriptor)
            if arguments.field:
                print(descriptor[arguments.field])
            else:
                print(json.dumps(descriptor, indent=2, sort_keys=True))
    except (BundleError, OSError) as error:
        print(f"dependency bundle error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
