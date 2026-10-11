#!/usr/bin/env python3
"""Export bounded graphical evidence from a Docker copy tar stream."""

from __future__ import annotations

import argparse
import io
import os
from pathlib import Path
import sys
import tarfile


FILES = frozenset({
    "xvfb.log", "pulse.log", "display.log", "sinks.log", "extraction.log",
    "client-help.log", "client-persistent.log", "probe.log", "window.log",
    "software-vulkan-frame.xwd", "audio-inputs.log", "host-packages.tsv", "result.json",
})
MAX_ARCHIVE_BYTES = 32 * 1024 * 1024
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_TOTAL_BYTES = 16 * 1024 * 1024
MAX_HEADERS = 64
MAX_METADATA_BYTES = 64 * 1024


class EvidenceError(ValueError):
    """Graphical evidence is unsafe, incomplete, or outside the closed contract."""


def validate_tar_bounds(data: bytes) -> None:
    # Bound extended metadata before tarfile constructs PAX dictionaries or
    # sparse maps. Docker's short fixed filenames need no GNU long-name fields.
    offset = headers = metadata = 0
    while offset + 512 <= len(data):
        block = data[offset:offset + 512]
        if not any(block):
            if len(data) - offset < 1024 or any(data[offset:]):
                raise EvidenceError("invalid graphical evidence tar trailer")
            return
        headers += 1
        size_field = block[124:136].strip(b"\0 ")
        if (headers > MAX_HEADERS or not size_field
                or any(value not in b"01234567" for value in size_field)):
            raise EvidenceError("invalid graphical evidence tar header bounds")
        size = int(size_field, 8)
        kind = block[156:157]
        if kind in {b"x", b"g"}:
            metadata += size
            if metadata > MAX_METADATA_BYTES:
                raise EvidenceError("oversized graphical evidence tar metadata")
        elif kind in {b"L", b"K", b"S"} or size > MAX_FILE_BYTES:
            raise EvidenceError("unsupported or oversized graphical evidence tar member")
        offset += 512 + ((size + 511) // 512) * 512
        if offset > len(data):
            raise EvidenceError("truncated graphical evidence tar payload")
    raise EvidenceError("missing graphical evidence tar trailer")


def read_evidence(stream, complete: bool = False) -> dict[str, bytes]:
    data = stream.read(MAX_ARCHIVE_BYTES + 1)
    if not data or len(data) > MAX_ARCHIVE_BYTES:
        raise EvidenceError("missing or oversized graphical evidence archive")
    validate_tar_bounds(data)
    records: dict[str, bytes] = {}
    total = 0
    root_seen = False
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as archive:
            for member in archive:
                if member.isdir() and member.name in {".", "./", "graphical-evidence", "graphical-evidence/"}:
                    if root_seen:
                        raise EvidenceError("duplicate graphical evidence root")
                    root_seen = True
                    continue
                name = member.name
                if name.startswith("./"):
                    name = name[2:]
                elif name.startswith("graphical-evidence/"):
                    name = name[len("graphical-evidence/"):]
                if name not in FILES or name in records or member.type not in {tarfile.REGTYPE, tarfile.AREGTYPE} or member.sparse is not None:
                    raise EvidenceError("unknown, duplicate, or nonregular graphical evidence member")
                if member.size < 0 or member.size > MAX_FILE_BYTES:
                    raise EvidenceError("oversized graphical evidence member")
                total += member.size
                if total > MAX_TOTAL_BYTES:
                    raise EvidenceError("oversized graphical evidence payload")
                source = archive.extractfile(member)
                if source is None:
                    raise EvidenceError("missing graphical evidence member data")
                payload = source.read(member.size + 1)
                if len(payload) != member.size:
                    raise EvidenceError("truncated graphical evidence member")
                records[name] = payload
    except (tarfile.TarError, OSError, OverflowError) as error:
        raise EvidenceError("invalid graphical evidence tar stream") from error
    if complete and (records.keys() != FILES or not records.get("result.json")
                     or not records.get("software-vulkan-frame.xwd")):
        raise EvidenceError("successful graphical qualification has incomplete evidence")
    return records


def export_evidence(stream, output: Path, complete: bool = False) -> list[str]:
    records = read_evidence(stream, complete)
    # The caller owns the parent. Refuse a substituted final parent or occupied
    # destination; candidate-controlled names never become filesystem paths.
    parent = os.open(output.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.mkdir(output.name, 0o700, dir_fd=parent)
        destination = os.open(output.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                              dir_fd=parent)
        try:
            for name, payload in sorted(records.items()):
                descriptor = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                                     0o600, dir_fd=destination)
                with os.fdopen(descriptor, "wb") as file:
                    file.write(payload)
        finally:
            os.close(destination)
    finally:
        os.close(parent)
    return sorted(records)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--complete", action="store_true")
    arguments = parser.parse_args()
    try:
        files = export_evidence(sys.stdin.buffer, arguments.output, arguments.complete)
    except (EvidenceError, OSError) as error:
        print(f"graphical evidence export failed: {error}", file=sys.stderr)
        return 2
    print(f"retained {len(files)} bounded graphical evidence files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
