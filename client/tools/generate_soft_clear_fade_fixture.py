#!/usr/bin/env python3
"""Generate the closed last-authorized soft-clear fade fixture.

Copyright 2026 The Atrinik Project
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

if __package__:
    from . import generate_living_outline_fixtures as wire
else:
    import generate_living_outline_fixtures as wire


def scene() -> bytes:
    """Author opaque items and a named remote actor in a colored, dim field."""
    wire.MAP_SIZE = 17
    wire.ORIGIN = 8
    tiles = bytearray()
    for x in range(17):
        for y in range(17):
            layers = [wire.layer(0, 1)]
            if (x, y) == (5, 10):
                layers.append(wire.layer(2, 3))  # ITEM
            if (x, y) == (12, 7):
                layers.append(wire.layer(3, 3))  # ITEM2
            if (x, y) == (8, 8):
                layers.append(wire.layer(wire.LAYER_LIVING, 2))
            if (x, y) == (10, 10):
                # NAME, MORE; TARGET, PROBE. Strings retain MAP2 NUL framing.
                layers.append(struct.pack(">BHBB", 5, 2, 0, 0x82) +
                              b"Soft clear actor\0ffffff\0" +
                              struct.pack(">IIBB", 24, 0x11223344, 0, 80))
            tile = wire.tile(x, y, *layers, radiance=128)
            # Replace the empty extension with the canonical scalar-owner RGB bitmap.
            tiles.extend(tile[:-1] + struct.pack(">BBHHH", 2, 1, 192, 64, 32))
    return wire.packet(((0, bytes(tiles)),))


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "src/tests/fixtures/player_view"
    snapshot = fixtures / "soft-clear-fade.map2.hex"
    snapshot.write_text(scene().hex() + "\n", encoding="ascii")
    manifest = ET.parse(fixtures / "presentation-day.xml").getroot()
    manifest.set("snapshot", str(snapshot.relative_to(root)))
    manifest.set("snapshot-sha256", hashlib.sha256(snapshot.read_bytes()).hexdigest())
    manifest.set("soft-clear-fade-test", "true")
    for child in list(manifest):
        manifest.remove(child)
    for face, name in ((1, "presentation-floor"), (2, "presentation-player"), (3, "presentation-tall")):
        path = fixtures / f"{name}.png"
        ET.SubElement(manifest, "asset", {
            "face": str(face), "path": str(path.relative_to(root)),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        })
    ET.indent(manifest, space="  ")
    ET.ElementTree(manifest).write(fixtures / "soft-clear-fade.xml", encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    main()
