#!/usr/bin/env python3
"""Generate closed production-MAP2 lighting presentation comparison scenes.

Copyright 2026 The Atrinik Project
"""

from __future__ import annotations

import hashlib
import struct
import zlib
from pathlib import Path
import xml.etree.ElementTree as ET

import generate_living_outline_fixtures as wire


def png(width: int, height: int, pixel) -> bytes:
    """Encode tiny analytic test geometry, without external image dependencies."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(
            ">I", zlib.crc32(kind + data)
        )

    rows = b"".join(
        b"\0" + bytes(channel for x in range(width) for channel in pixel(x, y))
        for y in range(height)
    )
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, 9))
        + chunk(b"IEND", b"")
    )


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "src/tests/fixtures/player_view"
    wire.MAP_SIZE = 17
    wire.ORIGIN = 8
    floor = png(48, 24, lambda x, y: (192, 192, 192, 255)
                if abs(2 * x - 47) + 2 * abs(2 * y - 23) <= 48
                else (0, 0, 0, 0))
    actor = png(24, 40, lambda x, y: (255, 255, 255, 255)
                if (7 <= x <= 16 and y < 10) or (3 <= x <= 20 and 10 <= y < 29)
                or (5 <= x <= 10 and y >= 29) or (14 <= x <= 19 and y >= 29)
                else (0, 0, 0, 0))
    for name, contents in (("presentation-floor", floor), ("presentation-player", actor)):
        (fixtures / f"{name}.png").write_bytes(contents)

    template = ET.parse(fixtures / "radial-light-smooth.xml").getroot()
    for label, radiance in (("day", 2048), ("dusk", 128), ("night", 0)):
        tiles = bytearray()
        for x in range(wire.MAP_SIZE):
            for y in range(wire.MAP_SIZE):
                layers = [wire.layer(0, 1)]
                if x == wire.ORIGIN and y == wire.ORIGIN:
                    layers.append(wire.layer(wire.LAYER_LIVING, 2))
                tiles.extend(wire.tile(x, y, *layers, radiance=radiance))
        packet = wire.packet(((0, bytes(tiles)),))
        snapshot = fixtures / f"presentation-{label}.map2.hex"
        snapshot.write_text(packet.hex() + "\n", encoding="ascii")
        manifest = ET.fromstring(ET.tostring(template))
        manifest.set("snapshot", str(snapshot.relative_to(root)))
        manifest.set("snapshot-sha256", hashlib.sha256(snapshot.read_bytes()).hexdigest())
        manifest.set("viewport-width", "1024")
        manifest.set("viewport-height", "640")
        manifest.attrib.pop("archived-software-pixels-sha256", None)
        for child in list(manifest):
            manifest.remove(child)
        for face, name in ((1, "presentation-floor"), (2, "presentation-player")):
            path = fixtures / f"{name}.png"
            ET.SubElement(manifest, "asset", {
                "face": str(face), "path": str(path.relative_to(root)),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            })
        ET.indent(manifest, space="  ")
        ET.ElementTree(manifest).write(fixtures / f"presentation-{label}.xml",
                                      encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    main()
