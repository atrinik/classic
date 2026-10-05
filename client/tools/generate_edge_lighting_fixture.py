#!/usr/bin/env python3
"""Generate the analytic smooth-lighting wire-edge fixture.

Copyright 2026 The Atrinik Project
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import xml.etree.ElementTree as ET

if __package__:
    from . import generate_living_outline_fixtures as wire
else:
    import generate_living_outline_fixtures as wire


MAP_SIZE = 17
ORIGIN = 8
RADIANCE = 2048
MARKERS = ((3, 16), (8, 15), (13, 14))


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "src/tests/fixtures/player_view"

    wire.MAP_SIZE = MAP_SIZE
    wire.ORIGIN = ORIGIN
    tiles = bytearray()
    for x in range(MAP_SIZE):
        for y in range(MAP_SIZE):
            layers = [wire.layer(0, 1)]
            if (x, y) == (ORIGIN, ORIGIN):
                layers.append(wire.layer(wire.LAYER_LIVING, 2))
            if (x, y) in MARKERS:
                layers.append(wire.layer(wire.LAYER_WALL, 3))
            tiles.extend(wire.tile(x, y, *layers, radiance=RADIANCE))

    packet = wire.packet(((0, bytes(tiles)),))
    snapshot = fixtures / "edge-lighting.map2.hex"
    snapshot.write_text(packet.hex() + "\n", encoding="ascii")

    manifest = ET.parse(fixtures / "presentation-day.xml").getroot()
    manifest.set("snapshot", str(snapshot.relative_to(root)))
    manifest.set("snapshot-sha256", hashlib.sha256(snapshot.read_bytes()).hexdigest())
    manifest.set("edge-lighting-test", "true")
    for child in list(manifest):
        manifest.remove(child)
    for face, name in (
        (1, "presentation-floor"),
        (2, "presentation-player"),
        (3, "presentation-tall"),
    ):
        path = fixtures / f"{name}.png"
        ET.SubElement(
            manifest,
            "asset",
            {
                "face": str(face),
                "path": str(path.relative_to(root)),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            },
        )
    ET.indent(manifest, space="  ")
    ET.ElementTree(manifest).write(
        fixtures / "edge-lighting.xml", encoding="utf-8", xml_declaration=True
    )


if __name__ == "__main__":
    main()
