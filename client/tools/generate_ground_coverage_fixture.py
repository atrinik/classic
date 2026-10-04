#!/usr/bin/env python3
"""Generate the closed MAP2 ground-coverage presentation fixture.

Copyright 2026 The Atrinik Project
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import xml.etree.ElementTree as ET

import generate_living_outline_fixtures as wire


ASSET_HASHES = {
    "presentation-floor.png": "bfec639adc5141669ddfb13c0cbf2083a86704a7711fe05fa242841974b3c507",
    "presentation-player.png": "0e059acd7b08d5b8410c4c4542ac818bdaf87b6c1df1d55f9107808e0319fa01",
    "presentation-tall.png": "b7a44dfa1d8aead10f90d99b6092826e7b684ab18899d9cfc7b3041103c893f5",
}
HOLES = {
    (10, 5), (11, 5), (10, 6),
    (3, 3), (4, 3), (5, 3), (3, 4), (5, 4), (3, 5), (4, 5), (5, 5),
    (3, 9), (5, 9), (3, 10), (5, 10), (3, 11), (5, 11), (3, 12), (5, 12),
    (11, 11), (12, 12),
}


def validate_assets(fixtures: Path) -> None:
    """Refuse to emit a manifest for changed analytic assets."""
    for name, expected in ASSET_HASHES.items():
        observed = hashlib.sha256((fixtures / name).read_bytes()).hexdigest()
        if observed != expected:
            raise ValueError(f"{name} hash changed: {observed}")


def scene() -> bytes:
    """Return the complete known-floor scene with deliberately omitted tiles."""
    wire.MAP_SIZE = 17
    wire.ORIGIN = 8
    tiles = bytearray()
    for x in range(wire.MAP_SIZE):
        for y in range(wire.MAP_SIZE):
            if (x, y) in HOLES:
                continue
            layers = [wire.layer(0, 1)]
            if (x, y) == (9, 5):
                layers.append(wire.layer(1, 1))
            if (x, y) == (8, 8):
                layers.append(wire.layer(wire.LAYER_LIVING, 2))
            if (x, y) == (13, 7):
                layers.append(wire.layer(wire.LAYER_WALL, 3))
            if (x, y) == (13, 4):
                layers.append(wire.layer(wire.LAYER_WALL, 3, roof=True))
            tiles.extend(wire.tile(x, y, *layers, radiance=2048))
    return wire.packet(((0, bytes(tiles)),))


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "src/tests/fixtures/player_view"
    validate_assets(fixtures)

    snapshot = fixtures / "ground-coverage.map2.hex"
    snapshot.write_text(scene().hex() + "\n", encoding="ascii")

    manifest = ET.parse(fixtures / "presentation-day.xml").getroot()
    manifest.set("snapshot", str(snapshot.relative_to(root)))
    manifest.set("snapshot-sha256", hashlib.sha256(snapshot.read_bytes()).hexdigest())
    manifest.set("ground-coverage-test", "true")
    ET.SubElement(manifest, "asset", {
        "face": "3",
        "path": "src/tests/fixtures/player_view/presentation-tall.png",
        "sha256": ASSET_HASHES["presentation-tall.png"],
    })
    ET.indent(manifest, space="  ")
    ET.ElementTree(manifest).write(
        fixtures / "ground-coverage.xml", encoding="utf-8", xml_declaration=True
    )


if __name__ == "__main__":
    main()
