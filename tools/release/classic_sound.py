"""Verify the released Classic restoration product without executing media."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations

import hashlib
import json
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
    try:
        manifest = json.loads(manifest_data, object_pairs_hook=_pairs,
                              parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as exc:
        raise ValueError("invalid Classic sound manifest") from exc
    if (not isinstance(manifest, dict) or manifest.get("publishable") is not True
            or manifest.get("playtest_only") is not False
            or manifest.get("source_commit") != dependency.get("commit")
            or manifest.get("release_tag") != dependency.get("tag")):
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

    assets = manifest.get("assets")
    if not isinstance(assets, list) or not assets or len(assets) > MAX_FILES:
        raise ValueError("invalid Classic sound asset manifest")
    paths = set()
    counts = {"vorbis": 0, "opus": 0}
    for asset in assets:
        if not isinstance(asset, dict):
            raise ValueError("invalid Classic sound asset")
        name = safe_path(asset.get("logical_path"))
        output = asset.get("output")
        if (name.split("/", 1)[0] not in {"background", "effects"} or name in paths
                or not isinstance(output, dict) or output.get("codec") not in counts
                or output.get("container") != "ogg" or output.get("sha256") != inventory.get(name)
                or type(output.get("size_bytes")) is not int
                or output["size_bytes"] != (root / name).stat().st_size):
            raise ValueError("Classic sound asset identity or format differs from the inventory")
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
    if (paths != audio_paths or type(manifest.get("logical_path_count")) is not int
            or manifest["logical_path_count"] != len(paths)
            or type(manifest.get("copied_vorbis_count")) is not int
            or type(manifest.get("converted_opus_count")) is not int
            or manifest.get("copied_vorbis_count") != counts["vorbis"]
            or manifest.get("converted_opus_count") != counts["opus"]):
        raise ValueError("Classic sound audio closure/counts differ from the manifest")
    return {"logical_path_count": len(paths), "copied_vorbis_count": counts["vorbis"],
            "converted_opus_count": counts["opus"], "verified_file_count": len(actual)}
