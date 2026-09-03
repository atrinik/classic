#!/usr/bin/env python3
"""Validate the Classic GPU fixture and content-artifact provenance contract."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys
import xml.etree.ElementTree as ElementTree
from typing import Any


COMMIT_RE = re.compile("[0-9a-f]{40}")
SHA256_RE = re.compile("[0-9a-f]{64}")
CONTENT_REPOSITORY = "atrinik/content"
CONTENT_BRANCH = "main"
CONTENT_FORMAT = "classic-ads-v1"
CONTENT_ARTIFACT_FORMAT = "atrinik-classic-runtime-content-v1"
CONTENT_CONSUMERS = ["classic/client", "classic/editor", "classic/server"]
CONTENT_COMPATIBILITY = ">=5.10.1 <6.0.0"
LOCK_CONTENT_KEYS = {
    "name", "repository", "tag", "commit", "url", "sha256",
    "destination", "strip_components",
}
MANIFEST_KEYS = {
    "schema_version", "target", "source", "release_version",
    "content_format", "artifact_format", "compatible_classic_releases",
    "consumers", "replacement_ready", "replacement_toolkit_package",
    "license_files", "files", "celestial_schema_version",
    "celestial_runtime_factory_version", "celestial_migration_index",
    "celestial_migration_index_sha256", "celestial_manifest_files_sha256",
}
MANIFEST_FILE_KEYS = {"path", "sha256", "size"}


class ProvenanceError(ValueError):
    """A fixture or content artifact violates the versioned provenance contract."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ProvenanceError(message)


def _object(value: Any, context: str) -> dict[str, Any]:
    _require(isinstance(value, dict), f"{context} must be an object")
    return value


def _keys(value: dict[str, Any], expected: set[str], context: str) -> None:
    actual = set(value)
    _require(
        actual == expected,
        f"{context} keys differ: missing={sorted(expected - actual)}, "
        f"unexpected={sorted(actual - expected)}",
    )


def _unique_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ProvenanceError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ProvenanceError(f"non-finite JSON value is not allowed: {value}")


def _load_json_bytes(data: bytes, context: str) -> Any:
    try:
        text = data.decode("utf-8")
        return json.loads(
            text,
            object_pairs_hook=_unique_pairs,
            parse_constant=_reject_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, ProvenanceError) as error:
        raise ProvenanceError(f"{context} is not canonical UTF-8 JSON: {error}") from error


def load_json(path: Path) -> Any:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ProvenanceError(f"cannot read {path}: {error}") from error
    return _load_json_bytes(data, str(path))


def _string(value: Any, context: str) -> str:
    _require(isinstance(value, str) and value != "", f"{context} must be a non-empty string")
    return value


def _sha256(value: Any, context: str) -> str:
    value = _string(value, context)
    _require(SHA256_RE.fullmatch(value) is not None,
             f"{context} is not a lowercase SHA-256 digest")
    return value


def _commit(value: Any, context: str) -> str:
    value = _string(value, context)
    _require(COMMIT_RE.fullmatch(value) is not None,
             f"{context} is not a lowercase commit")
    return value


def _size(value: Any, context: str) -> int:
    _require(isinstance(value, int) and not isinstance(value, bool) and value > 0,
             f"{context} must be a positive integer")
    return value


def _safe_relative(value: Any, context: str) -> str:
    value = _string(value, context)
    _require(chr(0) not in value and chr(92) not in value,
             f"{context} contains an unsafe path separator")
    path = PurePosixPath(value)
    _require(not path.is_absolute() and all(part not in {"", ".", ".."} for part in path.parts),
             f"{context} must be a safe relative POSIX path")
    _require(value == path.as_posix(),
             f"{context} is not in canonical POSIX form")
    return path.as_posix()


def _validate_provenance(value: Any) -> dict[str, Any]:
    root = _object(value, "provenance")
    _keys(root, {
        "schema_version", "fixture_family", "fixture_source", "archdef",
        "content", "worldmaker",
    }, "provenance")
    _require(root["schema_version"] == 1, "unsupported provenance schema")
    _require(root["fixture_family"] == "gpu-player-view",
             "provenance fixture family is incorrect")

    fixture_source = _object(root["fixture_source"], "fixture_source")
    _keys(fixture_source, {"repository", "branch", "introduced_commit"}, "fixture_source")
    _require(fixture_source["repository"] == "atrinik/classic",
             "fixture source repository is incorrect")
    _require(fixture_source["branch"] == "main", "fixture source branch is incorrect")
    _commit(fixture_source["introduced_commit"], "fixture_source.introduced_commit")

    archdef = _object(root["archdef"], "archdef")
    _keys(archdef, {"path", "fixture_path", "sha256", "size", "owner"}, "archdef")
    _safe_relative(archdef["path"], "archdef.path")
    _safe_relative(archdef["fixture_path"], "archdef.fixture_path")
    _require(archdef["path"] == "client/data/archdef.dat",
             "archdef path is incorrect")
    _require(archdef["fixture_path"] == "data/archdef.dat",
             "archdef fixture path is incorrect")
    _sha256(archdef["sha256"], "archdef.sha256")
    _size(archdef["size"], "archdef.size")
    _require(archdef["owner"] == "classic/client", "archdef owner is incorrect")

    content = _object(root["content"], "content")
    _keys(content, {"selected", "observed_issue_coordinate"}, "content")
    selected = _object(content["selected"], "content.selected")
    _keys(selected, {
        "repository", "branch", "tag", "commit", "release_version",
        "artifact", "runtime_manifests",
    }, "content.selected")
    _require(selected["repository"] == CONTENT_REPOSITORY,
             "selected content repository is incorrect")
    _require(selected["branch"] == CONTENT_BRANCH, "selected content branch is incorrect")
    _string(selected["tag"], "content.selected.tag")
    _commit(selected["commit"], "content.selected.commit")
    _string(selected["release_version"], "content.selected.release_version")

    artifact = _object(selected["artifact"], "content.selected.artifact")
    _keys(artifact, {
        "content_format", "format", "url", "sha256", "manifest", "archetypes",
    }, "content.selected.artifact")
    _require(artifact["content_format"] == CONTENT_FORMAT,
             "selected content format is incorrect")
    _require(artifact["format"] == CONTENT_ARTIFACT_FORMAT,
             "selected content artifact format is incorrect")
    url = _string(artifact["url"], "content.selected.artifact.url")
    _require(url.startswith(
        "https://github.com/atrinik/content/releases/download/"
    ), "selected content artifact URL is not canonical")
    _require(selected["tag"] == f"v{selected['release_version']}",
             "selected content tag does not match its release version")
    expected_url = (
        "https://github.com/atrinik/content/releases/download/"
        f"{selected['tag']}/atrinik-content-{selected['release_version']}-classic-runtime.tar.gz"
    )
    _require(url == expected_url,
             "selected content artifact URL does not match its release coordinate")
    _sha256(artifact["sha256"], "content.selected.artifact.sha256")

    manifest = _object(artifact["manifest"], "content.selected.artifact.manifest")
    _keys(manifest, {"path", "sha256", "size", "files_sha256"},
          "content.selected.artifact.manifest")
    _safe_relative(manifest["path"], "content.selected.artifact.manifest.path")
    _sha256(manifest["sha256"], "content.selected.artifact.manifest.sha256")
    _size(manifest["size"], "content.selected.artifact.manifest.size")
    _sha256(manifest["files_sha256"],
            "content.selected.artifact.manifest.files_sha256")

    archetypes = _object(artifact["archetypes"], "content.selected.artifact.archetypes")
    _keys(archetypes, {"path", "sha256", "size"},
          "content.selected.artifact.archetypes")
    _safe_relative(archetypes["path"], "content.selected.artifact.archetypes.path")
    _sha256(archetypes["sha256"], "content.selected.artifact.archetypes.sha256")
    _size(archetypes["size"], "content.selected.artifact.archetypes.size")

    runtime_manifests = selected["runtime_manifests"]
    _require(isinstance(runtime_manifests, list) and runtime_manifests,
             "content.selected.runtime_manifests must be a non-empty array")
    seen_versions: set[str] = set()
    for index, variant_value in enumerate(runtime_manifests):
        variant = _object(variant_value, f"content.selected.runtime_manifests[{index}]")
        _keys(variant, {"release_version", "sha256", "size", "files_sha256"},
              f"content.selected.runtime_manifests[{index}]")
        version = _string(variant["release_version"],
                          f"content.selected.runtime_manifests[{index}].release_version")
        _require(version not in seen_versions,
                 f"duplicate runtime manifest release version: {version}")
        seen_versions.add(version)
        _sha256(variant["sha256"],
                f"content.selected.runtime_manifests[{index}].sha256")
        _size(variant["size"], f"content.selected.runtime_manifests[{index}].size")
        _sha256(variant["files_sha256"],
                f"content.selected.runtime_manifests[{index}].files_sha256")
    published = next(
        (item for item in runtime_manifests
         if item["release_version"] == selected["release_version"]),
        None,
    )
    _require(published is not None,
             "selected release version has no runtime manifest variant")
    for field in ("sha256", "size", "files_sha256"):
        _require(manifest[field] == published[field],
                 f"selected artifact manifest {field} disagrees with its release variant")

    observed = _object(content["observed_issue_coordinate"],
                       "content.observed_issue_coordinate")
    _keys(observed, {
        "observed_on", "classic_revision", "content_revision", "content_release",
    }, "content.observed_issue_coordinate")
    _string(observed["observed_on"], "content.observed_issue_coordinate.observed_on")
    _commit(observed["classic_revision"],
            "content.observed_issue_coordinate.classic_revision")
    _commit(observed["content_revision"],
            "content.observed_issue_coordinate.content_revision")
    _string(observed["content_release"], "content.observed_issue_coordinate.content_release")

    worldmaker = _object(root["worldmaker"], "worldmaker")
    _keys(worldmaker, {
        "archdef_generated", "archdef_source", "generated_outputs",
    }, "worldmaker")
    _require(worldmaker["archdef_generated"] is False,
             "worldmaker must not claim to generate archdef.dat")
    _safe_relative(worldmaker["archdef_source"], "worldmaker.archdef_source")
    _require(worldmaker["archdef_source"] == "classic/client/data/archdef.dat",
             "worldmaker archdef source is incorrect")
    outputs = worldmaker["generated_outputs"]
    _require(isinstance(outputs, list) and outputs and
             all(isinstance(item, str) and item for item in outputs),
             "worldmaker.generated_outputs must be a non-empty string array")
    _require(outputs == ["client-maps", "data/*.zz"],
             "worldmaker generated output boundary is incorrect")
    return root


def load_provenance(path: Path) -> dict[str, Any]:
    return _validate_provenance(load_json(path))


def _resolve_inside(root: Path, relative: str, context: str) -> Path:
    root = root.resolve()
    candidate = root / PurePosixPath(relative)
    current = root
    for part in PurePosixPath(relative).parts:
        current /= part
        _require(not current.is_symlink(),
                 f"{context} contains a symbolic link: {current}")
    resolved = candidate.resolve()
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise ProvenanceError(f"{context} escapes its root") from error
    return resolved


def _file_digest(path: Path, context: str) -> tuple[str, int]:
    _require(path.is_file() and not path.is_symlink(),
             f"{context} is not a regular file")
    digest = hashlib.sha256()
    size = 0
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
    except OSError as error:
        raise ProvenanceError(f"cannot read {context}: {error}") from error
    return digest.hexdigest(), size


def _verify_lock(lock_path: Path, provenance: dict[str, Any]) -> None:
    lock = _object(load_json(lock_path), "content dependency lock")
    _keys(lock, {"schema_version", "dependencies"}, "content dependency lock")
    _require(lock["schema_version"] == 1, "unsupported content dependency lock schema")
    dependencies = lock["dependencies"]
    _require(isinstance(dependencies, list),
             "content dependency lock dependencies must be an array")
    entries = [
        _object(item, f"content dependency lock entry {index}")
        for index, item in enumerate(dependencies)
        if isinstance(item, dict) and item.get("name") == "content"
    ]
    _require(len(entries) == 1,
             "content dependency lock must contain exactly one content entry")
    entry = entries[0]
    _keys(entry, LOCK_CONTENT_KEYS, "content dependency lock content entry")

    selected = provenance["content"]["selected"]
    artifact = selected["artifact"]
    expected = {
        "name": "content",
        "repository": selected["repository"],
        "tag": selected["tag"],
        "commit": selected["commit"],
        "url": artifact["url"],
        "sha256": artifact["sha256"],
        "destination": "runtime/content",
        "strip_components": 1,
    }
    for key, value in expected.items():
        _require(entry[key] == value,
                 f"content dependency lock {key} disagrees with fixture provenance")


def _verify_static_inputs(classic_root: Path, provenance: dict[str, Any]) -> int:
    archdef = provenance["archdef"]
    archdef_path = _resolve_inside(classic_root, archdef["path"], "archdef.path")
    actual_sha, actual_size = _file_digest(archdef_path, str(archdef_path))
    _require(actual_sha == archdef["sha256"],
             f"archdef digest mismatch: expected {archdef['sha256']}, got {actual_sha}")
    _require(actual_size == archdef["size"],
             f"archdef size mismatch: expected {archdef['size']}, got {actual_size}")

    client_root = _resolve_inside(classic_root, "client", "client root")
    fixture_root = _resolve_inside(
        client_root, "src/tests/fixtures/player_view", "fixture root"
    )
    _require(fixture_root.is_dir() and not fixture_root.is_symlink(),
             "fixture root is not a regular directory")
    manifests = sorted(fixture_root.glob("*.xml"))
    gpu_count = 0
    for manifest_path in manifests:
        _require(not manifest_path.is_symlink(), f"fixture manifest is a symlink: {manifest_path}")
        try:
            document_root = ElementTree.parse(manifest_path).getroot()
        except (OSError, ElementTree.ParseError) as error:
            raise ProvenanceError(f"cannot parse fixture {manifest_path}: {error}") from error
        if document_root.get("renderer") != "gpu":
            continue
        gpu_count += 1
        input_root_value = document_root.get("input-root")
        _require(input_root_value is not None,
                 f"{manifest_path.name} is missing input-root")
        input_root = (manifest_path.parent / input_root_value).resolve()
        _require(input_root == client_root.resolve(),
                 f"{manifest_path.name} input-root does not resolve to client root")
        _require(
            document_root.get("archdef") == provenance["archdef"]["fixture_path"],
            f"{manifest_path.name} archdef path disagrees with provenance",
        )
        _require(
            document_root.get("archdef-sha256") == provenance["archdef"]["sha256"],
            f"{manifest_path.name} archdef digest disagrees with provenance",
        )
    _require(gpu_count > 0, "no GPU fixture manifests were found")
    return gpu_count


def _manifest_files_digest(entries: list[dict[str, Any]]) -> str:
    digest = hashlib.sha256()
    for entry in entries:
        payload = (
            entry["path"] + chr(0) + entry["sha256"] + chr(0) +
            str(entry["size"]) + chr(10)
        ).encode("utf-8")
        digest.update(payload)
    return digest.hexdigest()


def _validate_runtime_manifest(
    runtime_root: Path, provenance: dict[str, Any]
) -> dict[str, Any]:
    runtime_path = Path(runtime_root)
    _require(runtime_path.is_dir() and not runtime_path.is_symlink(),
             "content runtime root is not a regular directory")
    runtime_root = runtime_path.resolve()
    selected = provenance["content"]["selected"]
    artifact = selected["artifact"]
    manifest_relative = _safe_relative(
        artifact["manifest"]["path"], "content runtime manifest path"
    )
    manifest_path = _resolve_inside(runtime_root, manifest_relative,
                                    "content runtime manifest")
    manifest_data = manifest_path.read_bytes()
    manifest = _object(_load_json_bytes(manifest_data, str(manifest_path)),
                       "content runtime manifest")
    _keys(manifest, MANIFEST_KEYS, "content runtime manifest")

    release_version = _string(manifest["release_version"],
                              "content runtime manifest release_version")
    variants = selected["runtime_manifests"]
    variant = next(
        (item for item in variants if item["release_version"] == release_version),
        None,
    )
    _require(variant is not None,
             f"content runtime release version is not approved: {release_version}")
    actual_manifest_sha = hashlib.sha256(manifest_data).hexdigest()
    _require(actual_manifest_sha == variant["sha256"],
             f"content runtime manifest digest mismatch: expected {variant['sha256']}, "
             f"got {actual_manifest_sha}")
    _require(len(manifest_data) == variant["size"],
             f"content runtime manifest size mismatch: expected {variant['size']}, "
             f"got {len(manifest_data)}")

    _require(manifest["schema_version"] == 2, "content runtime manifest schema is incorrect")
    _require(manifest["target"] == "classic", "content runtime manifest target is incorrect")
    _require(manifest["source"] == {
        "repository": selected["repository"],
        "branch": selected["branch"],
        "commit": selected["commit"],
    }, "content runtime source coordinate disagrees with fixture provenance")
    for key, expected in (
        ("content_format", CONTENT_FORMAT),
        ("artifact_format", CONTENT_ARTIFACT_FORMAT),
        ("compatible_classic_releases", CONTENT_COMPATIBILITY),
    ):
        _require(manifest[key] == expected,
                 f"content runtime manifest {key} is incorrect")
    _require(manifest["consumers"] == CONTENT_CONSUMERS,
             "content runtime manifest consumers are incorrect")
    _require(manifest["replacement_ready"] is False and
             manifest["replacement_toolkit_package"] is False,
             "content runtime replacement flags are incorrect")
    _require(manifest["celestial_schema_version"] == 1 and
             manifest["celestial_runtime_factory_version"] == 1,
             "content runtime celestial schema is incorrect")
    _require(
        manifest["celestial_migration_index"] == "maps/celestial-migration-index.json",
        "content runtime migration index path is incorrect",
    )

    raw_entries = manifest["files"]
    _require(isinstance(raw_entries, list) and raw_entries,
             "content runtime manifest files must be a non-empty array")
    entries: list[dict[str, Any]] = []
    paths: list[str] = []
    for index, raw_entry in enumerate(raw_entries):
        entry = _object(raw_entry, f"content runtime manifest files[{index}]")
        _keys(entry, MANIFEST_FILE_KEYS, f"content runtime manifest files[{index}]")
        path = _safe_relative(entry["path"],
                               f"content runtime manifest files[{index}].path")
        digest = _sha256(entry["sha256"],
                         f"content runtime manifest files[{index}].sha256")
        size = _size(entry["size"],
                     f"content runtime manifest files[{index}].size")
        entries.append({"path": path, "sha256": digest, "size": size})
        paths.append(path)
    _require(paths == sorted(paths) and len(paths) == len(set(paths)),
             "content runtime manifest files are not sorted and unique")
    _require(manifest["celestial_manifest_files_sha256"] == _manifest_files_digest(entries),
             "content runtime manifest file-list digest is incorrect")
    migration_entry = next(
        (entry for entry in entries
         if entry["path"] == manifest["celestial_migration_index"]),
        None,
    )
    _require(migration_entry is not None,
             "content runtime manifest is missing the migration index")
    _require(manifest["celestial_migration_index_sha256"] == migration_entry["sha256"],
             "content runtime migration index digest is incorrect")

    expected_archetypes = artifact["archetypes"]
    archetype_entry = next(
        (entry for entry in entries if entry["path"] == expected_archetypes["path"]),
        None,
    )
    _require(archetype_entry == expected_archetypes,
             "content runtime archetype artifact disagrees with fixture provenance")

    expected_paths = set(paths)
    actual_paths: set[str] = set()
    for candidate in runtime_root.rglob("*"):
        _require(not candidate.is_symlink(),
                 f"content runtime contains a symbolic link: {candidate}")
        if candidate.is_file():
            relative = candidate.relative_to(runtime_root).as_posix()
            if relative != manifest_relative:
                actual_paths.add(relative)
    _require(actual_paths == expected_paths,
             "content runtime files differ from the manifest")

    for entry in entries:
        path = _resolve_inside(runtime_root, entry["path"],
                               f"content runtime file {entry['path']}")
        actual_sha, actual_size = _file_digest(path, str(path))
        _require(actual_sha == entry["sha256"],
                 f"content runtime digest mismatch for {entry['path']}: "
                 f"expected {entry['sha256']}, got {actual_sha}")
        _require(actual_size == entry["size"],
                 f"content runtime size mismatch for {entry['path']}: "
                 f"expected {entry['size']}, got {actual_size}")

    return {
        "release_version": release_version,
        "manifest_sha256": actual_manifest_sha,
        "manifest_size": len(manifest_data),
        "files": len(entries),
        "manifest_files_sha256": manifest["celestial_manifest_files_sha256"],
        "archetypes_sha256": expected_archetypes["sha256"],
    }


def verify(
    classic_root: Path,
    provenance_path: Path | None = None,
    lock_path: Path | None = None,
    content_runtime: Path | None = None,
) -> dict[str, Any]:
    classic_path = Path(classic_root)
    _require(classic_path.is_dir() and not classic_path.is_symlink(),
             "Classic root is not a regular directory")
    classic_root = classic_path.resolve()
    if provenance_path is None:
        provenance_path = classic_root / "client/src/tests/fixtures/player_view/content-provenance.json"
    if lock_path is None:
        lock_path = classic_root / "server/dependencies.lock.json"
    provenance = load_provenance(Path(provenance_path))
    _verify_lock(Path(lock_path), provenance)
    fixture_count = _verify_static_inputs(classic_root, provenance)
    runtime = None
    if content_runtime is not None:
        runtime = _validate_runtime_manifest(Path(content_runtime), provenance)
    return {
        "schema_version": 1,
        "fixture_family": provenance["fixture_family"],
        "gpu_fixture_count": fixture_count,
        "content_coordinate": {
            "repository": provenance["content"]["selected"]["repository"],
            "branch": provenance["content"]["selected"]["branch"],
            "tag": provenance["content"]["selected"]["tag"],
            "commit": provenance["content"]["selected"]["commit"],
            "release_version": provenance["content"]["selected"]["release_version"],
        },
        "archdef": {
            "path": provenance["archdef"]["path"],
            "sha256": provenance["archdef"]["sha256"],
            "size": provenance["archdef"]["size"],
        },
        "runtime_verified": runtime is not None,
        "runtime": runtime,
    }


def main(argv: list[str] | None = None) -> int:
    script_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(
        description="Verify Classic GPU fixture and content runtime provenance."
    )
    parser.add_argument("--classic-root", type=Path, default=script_root)
    parser.add_argument("--provenance", type=Path)
    parser.add_argument("--lock", type=Path)
    parser.add_argument("--content-runtime", type=Path)
    args = parser.parse_args()
    try:
        result = verify(
            args.classic_root,
            args.provenance,
            args.lock,
            args.content_runtime,
        )
    except (OSError, ProvenanceError) as error:
        print(f"content provenance error: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
