#!/usr/bin/env python3
"""Expand a MAP_UPDATE_CMD_NEW fixture into a dense five-depth movement scene."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


DEPTHS = (-2, -1, 0, 1, 2)
WIRE_SIZE = 21
ORIGIN = (10, 10)
VISIBLE_COORDINATES = range(2, 19)
TRANSITION_MAP_NAME = "Movement transition"
TRANSITION_MAP_PATH = "/tests/player-view/movement-transition"

# The source scene remains authoritative for the more involved protocol
# combinations.  This generated field adds a realistic amount of ordinary map
# content around it: every visible cell has scalar illumination, the base map
# is densely furnished, upper/lower depths are deliberately sparser, and only
# a few cells per depth override the neutral scalar with colored radiance.
COLORED_SOURCE_COORDINATES = (
    ((4, 4), (10, 10), (16, 16)),
    ((4, 10), (10, 16), (16, 4)),
    ((4, 16), (10, 4), (16, 10)),
    ((6, 6), (10, 14), (14, 8)),
    ((6, 14), (10, 6), (14, 12)),
)


def object_face(layer: int, seed: int) -> int:
    """Select a suitably sized fixture asset for the generated object layer."""
    faces_by_layer = {
        0: (1, 2, 3, 4),
        2: (2, 3, 4),
        4: (4, 6),
        6: (3, 4, 6, 7),
    }
    faces = faces_by_layer[layer]
    return faces[seed % len(faces)]


def object_layers(depth: int, x: int, y: int) -> tuple[tuple[int, int], ...]:
    """Return representative socket-layer/face pairs for one map cell."""
    seed = x * 13 + y * 7 + (depth + 2) * 17
    layers: list[tuple[int, int]] = []

    # About two thirds of the base level is ordinary floor. Lower levels form
    # narrow traversable bands; upper levels retain scattered roofs/effects.
    if depth == 0 and seed % 3 != 0:
        layers.append((0, object_face(0, seed)))
    elif depth == -1 and (x + 2 * y) % 13 == 0:
        layers.append((0, object_face(0, seed)))
    elif depth == -2 and (2 * x + y) % 19 == 0:
        layers.append((0, object_face(0, seed)))
    elif depth == 1 and (x + y) % 17 == 0:
        layers.append((4, object_face(4, seed)))
    elif depth == 2 and (3 * x + y) % 23 == 0:
        layers.append((6, object_face(6, seed)))

    # A small number of secondary layers keeps ordinary object painter-order
    # and effect paths active without turning every depth into five stacked
    # full maps, which is unlike normal play.
    if depth == 0 and seed % 37 == 0:
        layers.append((2, object_face(2, seed + 3)))
    elif depth == 1 and seed % 41 == 0:
        layers.append((6, object_face(6, seed + 5)))

    return tuple(layers)


def colored_radiance(depth_index: int, x: int, y: int) -> tuple[int, int, int] | None:
    """Return one of three subtle colored-light accents for this depth."""
    coordinates = COLORED_SOURCE_COORDINATES[depth_index]
    if (x, y) not in coordinates:
        return None

    source_index = coordinates.index((x, y))
    accents = (
        (0x02A0, 0x0180, 0x0140),
        (0x0140, 0x0270, 0x0180),
        (0x0160, 0x0190, 0x0290),
    )
    return accents[(source_index + depth_index) % len(accents)]


def dense_payload(depth: int) -> bytes:
    """Build dense scalar lighting plus representative geometry for one depth."""
    depth_index = DEPTHS.index(depth)
    result = bytearray()
    for y in VISIBLE_COORDINATES:
        for x in VISIBLE_COORDINATES:
            mask = x << 11 | y << 6 | 0x4
            scalar = 0x0700 + ((x * 29 + y * 17 + depth_index * 31) % 0x0180)
            layers = object_layers(depth, x, y)
            result.extend(struct.pack(">HHB", mask, scalar, len(layers)))
            for layer, face in layers:
                result.extend(struct.pack(">BHBB", layer, face, 0, 0))

            radiance = colored_radiance(depth_index, x, y)
            if radiance is None:
                result.append(0)
            else:
                result.extend(struct.pack(">BBHHH", 0x2, 0x1, *radiance))
    return bytes(result)


def new_header_fields(packet: bytes) -> tuple[list[tuple[int, int]], int]:
    if not packet or packet[0] != 1:
        raise ValueError("source must begin with MAP_UPDATE_CMD_NEW")
    cursor = 1
    fields = []
    for _ in range(3):
        end = packet.index(b"\0", cursor)
        fields.append((cursor, end))
        cursor = end + 1
    cursor += 2
    for _ in range(3):
        end = packet.index(b"\0", cursor)
        fields.append((cursor, end))
        cursor = end + 1
    return fields, cursor + 5


def new_header_end(packet: bytes) -> int:
    return new_header_fields(packet)[1]


def with_map_identity(packet: bytes, map_name: str, map_path: str) -> bytes:
    """Return a NEW packet with a distinct stable map name and path."""
    fields, _ = new_header_fields(packet)
    encoded_name = map_name.encode("utf-8")
    encoded_path = map_path.encode("utf-8")
    if not encoded_name or not encoded_path or b"\0" in encoded_name or b"\0" in encoded_path:
        raise ValueError("map identity must contain non-empty NUL-free UTF-8 text")
    name_start, name_end = fields[0]
    path_start, path_end = fields[5]
    return (packet[:name_start] + encoded_name + b"\0" + packet[name_end + 1:path_start] +
            encoded_path + b"\0" + packet[path_end + 1:])


def level_payloads(packet: bytes) -> list[bytes]:
    cursor = new_header_end(packet)
    cursor += 2  # continuation marker
    levels = packet[cursor]
    cursor += 1
    result = []
    for _ in range(levels):
        cursor += 1
        size = struct.unpack_from(">I", packet, cursor)[0]
        cursor += 4
        result.append(packet[cursor:cursor + size])
        cursor += size
    if cursor != len(packet) or not result:
        raise ValueError("source MAP level framing is invalid")
    return result


def expanded(packet: bytes, map_name: str | None = None, map_path: str | None = None) -> bytes:
    if (map_name is None) != (map_path is None):
        raise ValueError("map name and path must be supplied together")
    if map_name is not None and map_path is not None:
        packet = with_map_identity(packet, map_name, map_path)
    header_end = new_header_end(packet)
    payloads = level_payloads(packet)
    header = bytearray(packet[:header_end])
    header[-5:] = bytes((WIRE_SIZE, WIRE_SIZE, *ORIGIN, header[-1]))
    result = bytearray(header)
    result.extend(b"\0\0")
    result.append(len(DEPTHS))
    for index, depth in enumerate(DEPTHS):
        payload = dense_payload(depth) + payloads[index % len(payloads)]
        result.extend(struct.pack(">bI", depth, len(payload)))
        result.extend(payload)
    return bytes(result)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--transition", action="store_true")
    arguments = parser.parse_args()
    source = bytes.fromhex(arguments.input.read_text(encoding="ascii"))
    map_name = TRANSITION_MAP_NAME if arguments.transition else None
    map_path = TRANSITION_MAP_PATH if arguments.transition else None
    arguments.output.write_text(expanded(source, map_name, map_path).hex() + "\n",
                                encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
