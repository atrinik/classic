#!/usr/bin/env python3
"""Generate bounded MAP2 scenes for living-actor occlusion outlines."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


MAP_SIZE = 13
ORIGIN = 6
SCALAR_RADIANCE = 0x0800
MAP2_MASK_FOW = 0x20
LAYER_WALL = 4
LAYER_LIVING = 5
FACE_ACTOR = 4
FACE_WALL = 8
FACE_ITEM = 2
FACE_EFFECT = 3
FLAG_HEIGHT = 0x08
FLAG_DOUBLE = 0x40
FLAG_MORE = 0x80
FLAG2_ALPHA = 0x00000001
FLAG2_ROOF = 0x00000100


def layer(
    socket_layer: int,
    face: int,
    *,
    height: int | None = None,
    alpha: int | None = None,
    roof: bool = False,
    draw_double: bool = False,
) -> bytes:
    """Encode the small MAP2 layer subset used by these scenes."""
    flags = 0
    flags2 = 0
    suffix = bytearray()
    if height is not None:
        flags |= FLAG_HEIGHT
        suffix.extend(struct.pack(">h", height))
    if draw_double:
        flags |= FLAG_DOUBLE
    if alpha is not None:
        flags2 |= FLAG2_ALPHA
    if roof:
        flags2 |= FLAG2_ROOF
    if flags2:
        flags |= FLAG_MORE
        suffix.extend(struct.pack(">I", flags2))
        if alpha is not None:
            suffix.append(alpha)
    return struct.pack(">BHBB", socket_layer, face, 0, flags) + suffix


def tile(x: int, y: int, *layers: bytes, radiance: int = SCALAR_RADIANCE) -> bytes:
    """Encode one lit MAP2 tile and its closed extended-flags byte."""
    mask = x << 11 | y << 6 | 0x4
    return struct.pack(">HHB", mask, radiance, len(layers)) + b"".join(layers) + b"\0"


def packet(levels: tuple[tuple[int, bytes], ...]) -> bytes:
    """Frame one complete MAP_UPDATE_CMD_NEW packet."""
    result = bytearray(b"\x01Living actor outline\0no_music\0none\0")
    result.extend(b"\0\0\0\0\0")
    result.extend(bytes((MAP_SIZE, MAP_SIZE, ORIGIN, ORIGIN, 0)))
    result.extend(b"\0\0")
    result.append(len(levels))
    for depth, payload in levels:
        result.extend(struct.pack(">bI", depth, len(payload)))
        result.extend(payload)
    return bytes(result)


def same_packet(payload: bytes) -> bytes:
    """Create a same-position MAP2 packet for a retained-cache update."""
    result = bytearray(b"\0" + bytes((ORIGIN, ORIGIN, 0)) + b"\0\0")
    result.append(1)
    result.extend(struct.pack(">bI", 0, len(payload)))
    result.extend(payload)
    return bytes(result)


def fow_tile(x: int, y: int) -> bytes:
    """Mark one cached tile as fog without discarding its remembered layers."""
    mask = x << 11 | y << 6 | MAP2_MASK_FOW
    return struct.pack(">HB", mask, 1) + b"\0\0"


def base(*records: bytes) -> bytes:
    """Create a base-level scene with an unobscured local actor."""
    local = tile(ORIGIN, ORIGIN, layer(LAYER_LIVING, FACE_ACTOR))
    return packet(((0, local + b"".join(records)),))


def centered_visibility_fade() -> bytes:
    """Create center-authorized item, actor, and effect records for fade tests."""
    center = tile(
        ORIGIN,
        ORIGIN,
        layer(0, 1),
        layer(2, FACE_ITEM),
        layer(5, FACE_ACTOR),
        layer(6, FACE_EFFECT),
    )
    return packet(((0, center),))


def scenes() -> dict[str, bytes]:
    """Return every deterministic acceptance and benchmark scene."""
    actor = tile(7, 6, layer(LAYER_LIVING, FACE_ACTOR))
    same_level_wall = tile(8, 7, layer(LAYER_WALL, FACE_WALL, height=16))
    nearby_wall = tile(9, 7, layer(LAYER_WALL, FACE_WALL, height=16))
    translucent_wall = tile(
        8,
        7,
        layer(LAYER_WALL, FACE_WALL, height=16, alpha=127),
    )
    roof = tile(10, 9, layer(LAYER_WALL, FACE_WALL, height=16, roof=True))
    double_actor = tile(7, 6, layer(LAYER_LIVING, FACE_ACTOR, draw_double=True))
    upper_copy_wall = tile(8, 7, layer(LAYER_WALL, FACE_WALL, height=48))
    dark_actor = tile(7, 6, layer(LAYER_LIVING, FACE_ACTOR), radiance=0)
    dark_wall = tile(8, 7, layer(LAYER_WALL, FACE_WALL, height=16), radiance=0)

    crowded_unobscured = bytearray(tile(ORIGIN, ORIGIN, layer(LAYER_LIVING, FACE_ACTOR)))
    crowded = bytearray(crowded_unobscured)
    for y in range(1, 10, 2):
        for x in range(1, 10, 2):
            if (x, y) == (5, 5):
                continue
            actor_tile = tile(x, y, layer(LAYER_LIVING, FACE_ACTOR))
            crowded_unobscured.extend(actor_tile)
            crowded.extend(actor_tile)
            crowded.extend(tile(x + 1, y + 1, layer(LAYER_WALL, FACE_WALL, height=16)))

    return {
        "living-outline-unobscured": base(actor),
        "living-outline-same-level": base(actor, same_level_wall),
        "living-outline-upper-level": packet(
            (
                (0, tile(ORIGIN, ORIGIN, layer(LAYER_LIVING, FACE_ACTOR)) + actor),
                (1, roof),
            )
        ),
        "living-outline-nearby-wall": base(actor, nearby_wall),
        "living-outline-translucent": base(actor, translucent_wall),
        "living-outline-double": base(double_actor, upper_copy_wall),
        "living-outline-dark-actor": base(dark_actor, same_level_wall),
        "living-outline-dark-wall": base(actor, dark_wall),
        "living-outline-multiple": base(
            tile(5, 6, layer(LAYER_LIVING, FACE_ACTOR)),
            actor,
            same_level_wall,
        ),
        "living-outline-crowded-unobscured": packet(((0, bytes(crowded_unobscured)),)),
        "living-outline-crowded": packet(((0, bytes(crowded)),)),
        "living-outline-retained-fow": base(actor, same_level_wall),
        "living-outline-retained-fow-next": same_packet(fow_tile(7, 6)),
        "visibility-fade-centered": centered_visibility_fade(),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir", type=Path)
    arguments = parser.parse_args()
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    for name, data in scenes().items():
        (arguments.output_dir / f"{name}.map2.hex").write_text(data.hex() + "\n", encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
