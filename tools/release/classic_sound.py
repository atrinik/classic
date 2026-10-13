"""Verify the released Classic restoration product without executing media."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import re
import stat

MANIFEST = "classic-runtime-manifest.json"
CHECKSUMS = "SHA256SUMS"
RUNTIME_PINS = {"runtime_manifest_sha256", "runtime_checksums_sha256"}
MAX_METADATA_BYTES = 4 * 1024**2
MAX_FILE_BYTES = 512 * 1024**2
MAX_FILES = 100_000
SHA256 = re.compile(r"[0-9a-f]{64}")
GIT_OBJECT = re.compile(r"[0-9a-f]{40}")
LOGICAL_PATH = re.compile(r"(?:background|effects)/(?:[a-z0-9][a-z0-9_.-]*/)*[a-z0-9][a-z0-9_.-]*\.(?:mid|mod|s3m|xm|ogg)")
SOURCE_PATH = re.compile(r"(?:background|effects)/(?:[a-z0-9][a-z0-9_.-]*/)*[a-z0-9][a-z0-9_.-]*\.(?:flac|mid|mod|s3m|xm|ogg)")
MANIFEST_KEYS = {"$schema", "schema_version", "publishable", "playtest_only", "release_tag",
                 "source_commit", "source_tree", "source_manifest_sha256", "toolchain_sha256",
                 "tool_versions", "schema_sha256", "remediation_report_sha256",
                 "remediation_finding_count", "logical_path_count", "copied_vorbis_count",
                 "converted_opus_count", "output_tree_sha256", "assets"}
TOOL_NAMES = {"ffmpeg", "wildmidi", "openmpt123", "opusenc", "opusinfo", "sdl3_mixer_probe"}


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def safe_path(name: object) -> str:
    if (not isinstance(name, str) or not name or "\\" in name
            or name.startswith("/") or any(part in {"", ".", ".."} for part in name.split("/"))
            or any(ord(char) < 32 or ord(char) == 127 for char in name)):
        raise ValueError("unsafe Classic sound path")
    return name


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError("duplicate Classic sound manifest key")
        result[key] = value
    return result


def _metadata(path: Path, expected: object) -> bytes:
    if (not isinstance(expected, str) or SHA256.fullmatch(expected) is None
            or path.is_symlink() or not path.is_file()
            or path.stat().st_size > MAX_METADATA_BYTES):
        raise ValueError("invalid pinned Classic sound metadata")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"Classic sound {path.name} differs from its pinned digest")
    return data


def _json(data: bytes) -> dict:
    try:
        value = json.loads(data, object_pairs_hook=_pairs,
                           parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as exc:
        raise ValueError("invalid Classic sound JSON contract") from exc
    if not isinstance(value, dict):
        raise ValueError("Classic sound JSON contract must be an object")
    return value


def _hex(value: object, pattern=SHA256) -> bool:
    return isinstance(value, str) and pattern.fullmatch(value) is not None


def _positive_duration(value: object) -> bool:
    if type(value) not in {int, float}:
        return False
    try:
        return math.isfinite(value) and value > 0
    except OverflowError:
        return False


def load_dependency(source_root: Path) -> dict[str, object]:
    value = json.loads((source_root / "client/dependencies.lock.json").read_bytes(),
                      object_pairs_hook=_pairs)
    records = value.get("dependencies") if isinstance(value, dict) else None
    if not isinstance(records, list):
        raise ValueError("invalid Classic sound dependency lock")
    matches = [record for record in records if isinstance(record, dict) and record.get("name") == "sound"]
    if len(matches) != 1:
        raise ValueError("Classic AppImage requires one released restoration sound input")
    record = matches[0]
    tag = record.get("tag")
    if (not isinstance(tag, str) or re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag) is None
            or record.get("repository") != "atrinik/sound" or record.get("destination") != "sound"
            or record.get("strip_components") != 1 or not RUNTIME_PINS.issubset(record)
            or record.get("url") != (f"https://github.com/atrinik/sound/releases/download/{tag}/"
                                      f"atrinik-sound-classic-runtime-{tag[1:]}.tar.gz")):
        raise ValueError("Classic AppImage requires the pinned released restoration sound product")
    return record


def ogg_codec(path: Path) -> str:
    # Codec identity comes from the first Ogg packet, never its legacy filename.
    # Authentic payload hashes are checked separately; this is not full decoding.
    with path.open("rb") as stream:
        header = stream.read(27)
        if (len(header) != 27 or header[:5] != b"OggS\0"
                or header[5] & 3 != 2 or header[26] == 0):
            raise ValueError("Classic sound payload must begin with an Ogg identification page")
        lacing = stream.read(header[26])
        if len(lacing) != header[26]:
            raise ValueError("truncated Classic sound Ogg page")
        packet_size = 0
        for size in lacing:
            packet_size += size
            if size < 255:
                break
        else:
            raise ValueError("unbounded Classic sound identification packet")
        packet = stream.read(min(packet_size, 8))
        if packet_size >= 8 and packet == b"OpusHead":
            return "opus"
        if packet_size >= 7 and packet[:7] == b"\x01vorbis":
            return "vorbis"
    raise ValueError("Classic sound codec is outside the packaged Vorbis/Opus decoder contract")


def verify_tree(root: Path, dependency: dict[str, object]) -> dict[str, object]:
    """Authenticate every installed sound byte and its supported media format."""
    if root.is_symlink() or not root.is_dir():
        raise ValueError("Classic sound root must be a regular directory")
    manifest_data = _metadata(root / MANIFEST, dependency.get("runtime_manifest_sha256"))
    checksums_data = _metadata(root / CHECKSUMS, dependency.get("runtime_checksums_sha256"))
    manifest = _json(manifest_data)
    versions = manifest.get("tool_versions")
    if (set(manifest) != MANIFEST_KEYS or type(manifest.get("schema_version")) is not int
            or manifest["schema_version"] != 1
            or manifest.get("$schema") != "schemas/classic-runtime-manifest-v1.schema.json"
            or not _hex(manifest.get("source_tree"), GIT_OBJECT)
            or not _hex(manifest.get("source_commit"), GIT_OBJECT)
            or not isinstance(manifest.get("release_tag"), str)
            or re.fullmatch(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", manifest["release_tag"]) is None
            or not isinstance(versions, dict) or set(versions) != TOOL_NAMES
            or any(not isinstance(value, str) or not value for value in versions.values())
            or type(manifest.get("remediation_finding_count")) is not int
            or manifest["remediation_finding_count"] < 0):
        raise ValueError("Classic sound manifest does not satisfy the supported v1 schema")
    if (manifest.get("publishable") is not True
            or manifest.get("playtest_only") is not False
            or manifest.get("source_commit") != dependency.get("commit")
            or manifest.get("release_tag") != dependency.get("tag")
            or (dependency.get("source_tree") is not None
                and manifest.get("source_tree") != dependency["source_tree"])):
        raise ValueError("Classic sound manifest publication/source identity differs from the lock")

    inventory = {}
    folded = set()
    try:
        lines = checksums_data.decode("ascii").splitlines()
    except UnicodeError as exc:
        raise ValueError("invalid Classic sound checksum inventory") from exc
    for line in lines:
        digest, separator, name = line.partition("  ")
        safe_path(name)
        if (not separator or SHA256.fullmatch(digest) is None or name.casefold() in folded
                or name in {CHECKSUMS, ".atrinik-dependency.json"}):
            raise ValueError("invalid or duplicate Classic sound checksum path")
        inventory[name] = digest
        folded.add(name.casefold())
    if not inventory or len(inventory) > MAX_FILES:
        raise ValueError("Classic sound checksum inventory exceeds limits")
    actual = set()
    for path in root.rglob("*"):
        mode = path.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode) or path.stat().st_size > MAX_FILE_BYTES:
            raise ValueError("Classic sound contains a link, special file or oversized payload")
        name = path.relative_to(root).as_posix()
        if name in {CHECKSUMS, ".atrinik-dependency.json"}:
            continue
        if name not in inventory or sha256_file(path) != inventory[name]:
            raise ValueError(f"Classic sound file differs from the pinned inventory: {name}")
        actual.add(name)
    if actual != set(inventory):
        raise ValueError("Classic sound checksum inventory is not a closed file set")

    links = {"schema_sha256": "schemas/classic-runtime-manifest-v1.schema.json",
             "source_manifest_sha256": "manifests/source-assets.json",
             "toolchain_sha256": "manifests/classic-audio-toolchain.json",
             "remediation_report_sha256": "classic-runtime-remediation.json"}
    linked = {key: _json(_metadata(root / name, manifest[key])) for key, name in links.items()}
    if linked["schema_sha256"].get("$id") != "https://atrinik.org/schemas/sound/classic-runtime-manifest-v1.schema.json":
        raise ValueError("Classic sound linked schema identity differs")
    source_manifest = linked["source_manifest_sha256"]
    originals = source_manifest.get("assets")
    if (set(source_manifest) != {"$schema", "schema_version", "source_corpus_sha256", "audio_source_count", "source_size_bytes", "assets"}
            or type(source_manifest.get("schema_version")) is not int or source_manifest.get("schema_version") != 1
            or source_manifest.get("$schema") != "../schemas/source-assets-v1.schema.json"
            or not isinstance(originals, list) or not originals or len(originals) > MAX_FILES
            or type(source_manifest.get("audio_source_count")) is not int
            or source_manifest["audio_source_count"] != len(originals)
            or not _hex(source_manifest.get("source_corpus_sha256"))
            or type(source_manifest.get("source_size_bytes")) is not int or source_manifest["source_size_bytes"] < 1):
        raise ValueError("Classic sound linked source manifest is invalid")
    source_by_path = {}
    for original in originals:
        if not isinstance(original, dict):
            raise ValueError("Classic sound source asset is invalid")
        name = safe_path(original.get("logical_path"))
        source = original.get("source")
        if (LOGICAL_PATH.fullmatch(name) is None or name in source_by_path
                or not isinstance(source, dict) or not _hex(source.get("sha256"))
                or not isinstance(source.get("codec"), str)
                or source["codec"] not in {"vorbis", "midi", "flac", "mod", "s3m", "xm"}
                or not isinstance(source.get("container"), str) or not source["container"]
                or SOURCE_PATH.fullmatch(safe_path(original.get("source_path"))) is None):
            raise ValueError("Classic sound source asset identity or codec is invalid")
        source_by_path[name] = original
    corpus = [{"source_path": original["source_path"], "sha256": original["source"]["sha256"]}
              for original in originals]
    corpus_bytes = (json.dumps(corpus, sort_keys=True, indent=2) + "\n").encode()
    if hashlib.sha256(corpus_bytes).hexdigest() != source_manifest["source_corpus_sha256"]:
        raise ValueError("Classic sound linked source-corpus digest differs")
    toolchain = linked["toolchain_sha256"]
    if (type(toolchain.get("schema_version")) is not int or toolchain.get("schema_version") != 1
            or toolchain.get("$schema") != "../schemas/classic-audio-toolchain-v1.schema.json"
            or not isinstance(toolchain.get("license_texts"), dict) or not toolchain["license_texts"]):
        raise ValueError("Classic sound linked toolchain is invalid")
    for license_record in toolchain["license_texts"].values():
        if not isinstance(license_record, dict):
            raise ValueError("Classic sound toolchain license is invalid")
        name = safe_path(license_record.get("archive_path"))
        if not _hex(license_record.get("sha256")) or inventory.get(name) != license_record["sha256"]:
            raise ValueError("Classic sound toolchain license differs from its contract")
    remediation = linked["remediation_report_sha256"]
    if (type(remediation.get("schema_version")) is not int or remediation.get("schema_version") != 1
            or remediation.get("$schema") != "schemas/classic-remediation-v1.schema.json"
            or remediation.get("source_commit") != manifest["source_commit"]
            or remediation.get("source_tree") != manifest["source_tree"]
            or remediation.get("source_manifest_sha256") != manifest["source_manifest_sha256"]
            or remediation.get("classification") != "nonblocking-modernization"
            or type(remediation.get("count")) is not int
            or remediation["count"] != manifest["remediation_finding_count"]
            or not isinstance(remediation.get("findings"), list)
            or len(remediation["findings"]) != remediation["count"]
            or type(remediation.get("source_count")) is not int
            or remediation.get("source_count") != len(source_by_path)):
        raise ValueError("Classic sound remediation report differs from its source/count contract")

    assets = manifest.get("assets")
    if not isinstance(assets, list) or not assets or len(assets) > MAX_FILES:
        raise ValueError("invalid Classic sound asset manifest")
    paths = set()
    counts = {"vorbis": 0, "opus": 0}
    for asset in assets:
        if not isinstance(asset, dict) or set(asset) != {"logical_path", "source_path", "source", "mapping", "output"}:
            raise ValueError("invalid Classic sound asset")
        name = safe_path(asset.get("logical_path"))
        output = asset.get("output")
        original = source_by_path.get(name)
        source = asset.get("source")
        if (LOGICAL_PATH.fullmatch(name) is None or name in paths or original is None
                or not isinstance(source, dict) or set(source) != {"sha256", "codec", "container"}
                or source != {key: original["source"].get(key) for key in ("sha256", "codec", "container")}
                or asset.get("source_path") != original["source_path"]
                or asset.get("mapping") != ("copy" if source["codec"] == "vorbis" else "render-opus")
                or not isinstance(output, dict) or not isinstance(output.get("codec"), str)
                or output["codec"] not in counts
                or set(output) != {"sha256", "size_bytes", "codec", "container", "sample_rate", "channels", "duration_seconds"}
                or output.get("container") != "ogg" or not _hex(output.get("sha256"))
                or name not in inventory or output["sha256"] != inventory[name]
                or type(output.get("size_bytes")) is not int
                or not 1 <= output["size_bytes"] <= MAX_FILE_BYTES
                or output["size_bytes"] != (root / name).stat().st_size
                or type(output.get("sample_rate")) is not int or output["sample_rate"] < 1
                or type(output.get("channels")) is not int or output["channels"] not in {1, 2}
                or not _positive_duration(output.get("duration_seconds"))):
            raise ValueError("Classic sound asset identity or format differs from the inventory")
        if source["codec"] == "vorbis":
            expected = {"sha256": source["sha256"], "codec": "vorbis", "container": "ogg",
                        **{key: original["source"].get(key) for key in ("sample_rate", "channels", "duration_seconds")},
                        "size_bytes": (root / name).stat().st_size}
            if output != expected:
                raise ValueError("Classic sound Vorbis copy differs from the source metadata")
        elif output["codec"] != "opus":
            raise ValueError("Classic sound rendered source must use Opus")
        if ogg_codec(root / name) != output["codec"]:
            raise ValueError("Classic sound actual codec differs from its manifest")
        counts[output["codec"]] += 1
        paths.add(name)
    # The released product retains the two folders' original notices alongside
    # media; those bytes are authenticated by the same closed inventory.
    notices = {f"{folder}/{name}" for folder in ("background", "effects")
               for name in ("LICENSE", "README.md")}
    audio_paths = {name for name in actual
                   if name.split("/", 1)[0] in {"background", "effects"} and name not in notices}
    tree = "".join(f"{inventory[name]}  {name}\n" for name in sorted(paths)).encode("ascii")
    if not _hex(manifest.get("output_tree_sha256")) or hashlib.sha256(tree).hexdigest() != manifest["output_tree_sha256"]:
        raise ValueError("Classic sound logical-tree digest differs from its manifest")
    if (paths != audio_paths or paths != set(source_by_path) or type(manifest.get("logical_path_count")) is not int
            or manifest["logical_path_count"] != len(paths)
            or type(manifest.get("copied_vorbis_count")) is not int
            or type(manifest.get("converted_opus_count")) is not int
            or manifest.get("copied_vorbis_count") != counts["vorbis"]
            or manifest.get("converted_opus_count") != counts["opus"]):
        raise ValueError("Classic sound audio closure/counts differ from the manifest")
    return {"logical_path_count": len(paths), "copied_vorbis_count": counts["vorbis"],
            "converted_opus_count": counts["opus"], "verified_file_count": len(actual)}
