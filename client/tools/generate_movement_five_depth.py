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
QUALIFICATION_DEPTHS = (-3, -2, -1, 0, 1, 2, 3)
QUALIFICATION_WIRE_SIZE = 29
QUALIFICATION_ORIGIN = (14, 14)
QUALIFICATION_COORDINATES = range(2, 27)
BENCHMARK_17_DEPTHS = (-2, -1, 0, 1, 2)
BENCHMARK_28_DEPTHS = tuple(range(-6, 7))
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


def object_layers(
    depth: int, x: int, y: int, roof_heavy: bool = False
) -> tuple[tuple[int, int], ...]:
    """Return representative socket-layer/face pairs for one map cell."""
    seed = x * 13 + y * 7 + (depth + 2) * 17
    layers: list[tuple[int, int]] = []

    # About two thirds of the base level is ordinary floor. Lower levels form
    # narrow traversable bands; upper levels retain scattered roofs/effects.
    # The roof-heavy variant keeps the same bounded 17-by-17 geometry while
    # matching the denser upper-depth painter mix seen in the motivating town.
    if roof_heavy and depth == 0 and seed % 4 != 0:
        layers.append((0, object_face(0, seed)))
    elif depth == 0 and seed % 3 != 0:
        layers.append((0, object_face(0, seed)))
    elif depth == -1 and (x + 2 * y) % 13 == 0:
        layers.append((0, object_face(0, seed)))
    elif depth == -2 and (2 * x + y) % 19 == 0:
        layers.append((0, object_face(0, seed)))
    elif depth == 1 and (x + y) % (11 if roof_heavy else 17) == 0:
        layers.append((4, object_face(4, seed)))
    elif depth == 2 and (3 * x + y) % (17 if roof_heavy else 23) == 0:
        layers.append((6, object_face(6, seed)))

    if roof_heavy and depth == 1 and (2 * x + y) % 19 == 0:
        layers.append((6, object_face(6, seed + 5)))
    elif roof_heavy and depth == 2 and (x + 3 * y) % 19 == 0:
        layers.append((4, object_face(4, seed + 7)))

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


def dense_payload(depth: int, roof_heavy: bool = False) -> bytes:
    """Build dense scalar lighting plus representative geometry for one depth."""
    depth_index = DEPTHS.index(depth)
    result = bytearray()
    for y in VISIBLE_COORDINATES:
        for x in VISIBLE_COORDINATES:
            mask = x << 11 | y << 6 | 0x4
            scalar = 0x0700 + ((x * 29 + y * 17 + depth_index * 31) % 0x0180)
            layers = object_layers(depth, x, y, roof_heavy)
            result.extend(struct.pack(">HHB", mask, scalar, len(layers)))
            for layer, face in layers:
                result.extend(struct.pack(">BHBB", layer, face, 0, 0))

            radiance = colored_radiance(depth_index, x, y)
            if radiance is None:
                result.append(0)
            else:
                result.extend(struct.pack(">BBHHH", 0x2, 0x1, *radiance))
    return bytes(result)


def qualification_layer(
    socket_layer: int,
    face: int,
    *,
    name: str | None = None,
    animation: bool = False,
    height: int | None = None,
    alpha: int | None = None,
    rotate: int | None = None,
    zoom: tuple[int, int] | None = None,
    draw_double: bool = False,
    roof: bool = False,
    door: bool = False,
    exit_tile: bool = False,
    target: int | None = None,
) -> bytes:
    """Encode the bounded MAP2 layer combinations used by GPU qualification."""
    flags = 0
    flags2 = 0
    suffix = bytearray()
    if name is not None:
        flags |= 0x02
        suffix.extend(name.encode("ascii") + b"\0ffffff\0")
    if animation:
        flags |= 0x04
        suffix.extend(b"\x01\x01\x00")
    if height is not None:
        flags |= 0x08
        suffix.extend(struct.pack(">h", height))
    if draw_double:
        flags |= 0x40
    if alpha is not None:
        flags2 |= 0x00000001
    if rotate is not None:
        flags2 |= 0x00000002
    if zoom is not None:
        flags2 |= 0x00000004
    if target is not None:
        flags2 |= 0x00000008 | 0x00000040
    if roof:
        flags2 |= 0x00000100
    if door:
        flags2 |= 0x00000200
    if exit_tile:
        flags2 |= 0x00000400
    if flags2:
        flags |= 0x80
        suffix.extend(struct.pack(">I", flags2))
        if alpha is not None:
            suffix.append(alpha)
        if rotate is not None:
            suffix.extend(struct.pack(">h", rotate))
        if zoom is not None:
            suffix.extend(struct.pack(">HH", *zoom))
        if target is not None:
            suffix.extend(struct.pack(">IB", target, 0))
    return struct.pack(">BHBB", socket_layer, face, 0, flags) + suffix


def qualification_payload(depth: int, coordinates: range = QUALIFICATION_COORDINATES) -> bytes:
    """Build a 25-by-25 town level with dense mixed GPU painter semantics."""
    result = bytearray()
    actor = 0
    for y in coordinates:
        for x in coordinates:
            seed = x * 31 + y * 17 + (depth + 3) * 43
            fow = depth != 0 and (x + y + depth) % 19 == 0
            support = seed % 37 == 0
            mask = x << 11 | y << 6 | 0x04 | (0x01 if support else 0) | (0x20 if fow else 0)
            result.extend(struct.pack(">H", mask))
            if support:
                result.extend(struct.pack(">h", (seed % 7) - 3))
            if fow:
                result.append(1)
            scalar = 0x0680 + seed % 0x0280
            result.extend(struct.pack(">H", scalar))

            layers = []
            floor_height = None
            if depth == 0 and (x + y) % 11 in (0, 1):
                floor_height = 8 + ((x * 3 + y * 5) % 5) * 4
                layers.append(qualification_layer(0, 1 + seed % 4, height=floor_height))
            if seed % 3 != 0:
                variant = seed % 5
                if variant == 0 and floor_height is None:
                    layers.append(qualification_layer(0, 1 + seed % 4))
                elif variant == 1:
                    layers.append(
                        qualification_layer(
                            1,
                            2 + seed % 5,
                            alpha=96 + seed % 128,
                            rotate=(seed % 7 - 3) * 15,
                            zoom=(75 + seed % 76, 75 + (seed * 3) % 76),
                        )
                    )
                elif variant == 2:
                    layers.append(
                        qualification_layer(2, 2 + (seed * 3) % 5, draw_double=True)
                    )
                elif variant == 3:
                    layers.append(
                        qualification_layer(4, 8, height=16 + depth * 8, roof=True,
                                            door=depth == 0 and seed % 23 == 0)
                    )
                else:
                    layers.append(
                        qualification_layer(6, 3 + seed % 4,
                                            exit_tile=depth == 0 and seed % 29 == 0)
                    )
            if depth == 0 and actor < 64:
                layers.append(
                    qualification_layer(
                        5,
                        10,
                        name=f"Actor {actor:02d}",
                        animation=True,
                        target=0x47700000 + actor,
                    )
                )
                actor += 1
            result.append(len(layers))
            result.extend(b"".join(layers))
            result.append(0)
    if depth == 0 and actor != 64:
        raise AssertionError("qualification fixture must contain exactly 64 live actors")
    return bytes(result)


def wire_ceiling_payload(depth: int) -> bytes:
    """Build one sparse 28-by-28 level while staying within one wire packet."""
    result = bytearray()
    actor = 0
    for y in range(2, 30):
        for x in range(2, 30):
            seed = x * 31 + y * 17 + (depth + 6) * 43
            support = depth == 0 and seed % 97 == 0
            mask = x << 11 | y << 6 | 0x04 | (0x01 if support else 0)
            result.extend(struct.pack(">H", mask))
            if support:
                result.extend(struct.pack(">h", 1 + seed % 7))
            result.extend(struct.pack(">H", 0x0680 + seed % 0x0280))
            layers = []
            if depth == 0 and actor < 64:
                layers.append(
                    qualification_layer(
                        5,
                        10,
                        name=f"Actor {actor:02d}",
                        animation=True,
                        target=0x47710000 + actor,
                    )
                )
                actor += 1
            elif ((x, y) in ((20, 20), (21, 20)) or seed % 211 == 0):
                layers.append(
                    qualification_layer(
                        0,
                        1 + seed % 4,
                        height=8 + ((x * 3 + y * 5) % 5) * 4,
                        draw_double=True,
                    )
                )
            result.append(len(layers))
            result.extend(b"".join(layers))
            result.append(0)
    if depth == 0 and actor != 64:
        raise AssertionError("wire-ceiling fixture must contain exactly 64 live actors")
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


def expanded(
    packet: bytes,
    map_name: str | None = None,
    map_path: str | None = None,
    roof_heavy: bool = False,
) -> bytes:
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
        payload = dense_payload(depth, roof_heavy) + payloads[index % len(payloads)]
        result.extend(struct.pack(">bI", depth, len(payload)))
        result.extend(payload)
    return bytes(result)


def qualification_scene(packet: bytes) -> bytes:
    """Return the frozen 25-by-25/seven-depth complete-client town fixture."""
    packet = with_map_identity(packet, "GPU qualification town", "/tests/gpu/qualification-town")
    header_end = new_header_end(packet)
    header = bytearray(packet[:header_end])
    header[-5:] = bytes((*QUALIFICATION_ORIGIN, *QUALIFICATION_ORIGIN, header[-1]))
    header[-5] = QUALIFICATION_WIRE_SIZE
    header[-4] = QUALIFICATION_WIRE_SIZE
    result = bytearray(header)
    result.extend(b"\0\0")
    result.append(len(QUALIFICATION_DEPTHS))
    for depth in QUALIFICATION_DEPTHS:
        payload = qualification_payload(depth)
        result.extend(struct.pack(">bI", depth, len(payload)))
        result.extend(payload)
    return bytes(result)


def benchmark_scene(packet: bytes, logical_size: int) -> bytes:
    """Return a production MAP2 benchmark scene for one required matrix row."""
    if logical_size == 17:
        depths = BENCHMARK_17_DEPTHS
        wire_size = 21
        origin = 10
        coordinates = range(2, 19)
        payload = lambda depth: qualification_payload(depth, coordinates)
    elif logical_size == 28:
        depths = BENCHMARK_28_DEPTHS
        wire_size = 32
        origin = 16
        payload = wire_ceiling_payload
    else:
        raise ValueError("benchmark scene must use logical size 17 or 28")
    packet = with_map_identity(packet,
                               f"GPU benchmark {logical_size}",
                               f"/tests/gpu/benchmark-{logical_size}")
    header_end = new_header_end(packet)
    header = bytearray(packet[:header_end])
    header[-5:] = bytes((wire_size, wire_size, origin, origin, header[-1]))
    result = bytearray(header)
    result.extend(b"\0\0")
    result.append(len(depths))
    for depth in depths:
        level = payload(depth)
        result.extend(struct.pack(">bI", depth, len(level)))
        result.extend(level)
    if len(result) > 65535:
        raise AssertionError(f"benchmark MAP2 packet exceeds the wire bound: {len(result)}")
    return bytes(result)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--transition", action="store_true")
    parser.add_argument("--roof-heavy", action="store_true")
    parser.add_argument("--qualification-25x25", action="store_true")
    parser.add_argument("--benchmark-17x17", action="store_true")
    parser.add_argument("--benchmark-28x28", action="store_true")
    arguments = parser.parse_args()
    source = bytes.fromhex(arguments.input.read_text(encoding="ascii"))
    map_name = TRANSITION_MAP_NAME if arguments.transition else None
    map_path = TRANSITION_MAP_PATH if arguments.transition else None
    qualification_25x25 = getattr(arguments, "qualification_25x25", False) is True
    benchmark_17x17 = getattr(arguments, "benchmark_17x17", False) is True
    benchmark_28x28 = getattr(arguments, "benchmark_28x28", False) is True
    modes = sum((qualification_25x25, benchmark_17x17, benchmark_28x28))
    if modes > 1:
        parser.error("choose at most one qualification/benchmark scene")
    if qualification_25x25:
        generated = qualification_scene(source)
    elif benchmark_17x17:
        generated = benchmark_scene(source, 17)
    elif benchmark_28x28:
        generated = benchmark_scene(source, 28)
    else:
        generated = expanded(source, map_name, map_path, arguments.roof_heavy)
    arguments.output.write_bytes((generated.hex() + "\n").encode("ascii"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
