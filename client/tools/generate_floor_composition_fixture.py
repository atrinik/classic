#!/usr/bin/env python3
"""Generate a closed night fixture for transparent item/floor composition.

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


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "src/tests/fixtures/player_view"
    wire.MAP_SIZE = 17
    wire.ORIGIN = 8
    tiles = bytearray()
    for x in range(17):
        for y in range(17):
            layers = [wire.layer(0, 1)]
            if (x, y) == (6, 10):
                layers.append(wire.layer(2, 3))  # ITEM, tall opaque analytic marker.
                # Remote actor metadata must be revoked immediately by soft FOW.
                layers.append(struct.pack(">BHBBIIBB", 5, 2, 0, 0x80, 24, 0x11223344, 0, 80))
            if (x, y) == (10, 10):
                layers.append(wire.layer(1, 2))  # Remembered FMASK decoration.
            tiles.extend(wire.tile(x, y, *layers, radiance=32))
    snapshot = fixtures / "floor-composition.map2.hex"
    snapshot.write_text(wire.packet(((0, bytes(tiles)),)).hex() + "\n", encoding="ascii")
    manifest = ET.parse(fixtures / "presentation-day.xml").getroot()
    manifest.set("snapshot", str(snapshot.relative_to(root)))
    manifest.set("snapshot-sha256", hashlib.sha256(snapshot.read_bytes()).hexdigest())
    manifest.set("floor-composition-test", "true")
    for child in list(manifest):
        manifest.remove(child)
    for face, name in ((1, "presentation-floor"), (2, "presentation-player"), (3, "presentation-tall")):
        path = fixtures / f"{name}.png"
        ET.SubElement(manifest, "asset", {
            "face": str(face), "path": str(path.relative_to(root)),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        })
    ET.indent(manifest, space="  ")
    ET.ElementTree(manifest).write(fixtures / "floor-composition.xml", encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    main()
