#!/usr/bin/env python3
"""Check actual production GPU captures of the closed presentation fixtures.

Copyright 2026 The Atrinik Project
"""

from __future__ import annotations

import argparse
import collections
import json
import struct
import zlib
from pathlib import Path


def pixels(path: Path) -> tuple[int, int, list[tuple[int, int, int]]]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not PNG")
    offset = 8
    compressed = bytearray()
    width = height = channels = 0
    while offset + 12 <= len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        if len(body) != length or offset + 12 + length > len(data):
            raise ValueError(f"{path}: truncated chunk")
        crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if zlib.crc32(kind + body) != crc:
            raise ValueError(f"{path}: corrupt chunk")
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", body
            )
            if (width, height) != (1024, 640) or depth != 8 or color not in (2, 6) or \
                    compression or filtering or interlace:
                raise ValueError(f"{path}: expected noninterlaced 1024x640 RGB(A)8 capture")
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            break
        offset += length + 12
    if not channels:
        raise ValueError(f"{path}: missing image header")
    stride = width * channels
    expected = height * (stride + 1)
    decoder = zlib.decompressobj()
    raw = decoder.decompress(compressed, expected + 1)
    if len(raw) != expected or not decoder.eof:
        raise ValueError(f"{path}: invalid raster size")
    result = []
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filter_type = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            above = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if filter_type == 0:
                prediction = 0
            elif filter_type == 1:
                prediction = left
            elif filter_type == 2:
                prediction = above
            elif filter_type == 3:
                prediction = (left + above) // 2
            elif filter_type == 4:
                estimate = left + above - corner
                distances = (abs(estimate - left), abs(estimate - above), abs(estimate - corner))
                prediction = (left, above, corner)[distances.index(min(distances))]
            else:
                raise ValueError(f"{path}: invalid PNG filter")
            row[x] = (row[x] + prediction) & 255
        result.extend(tuple(row[x:x + 3]) for x in range(0, stride, channels))
        previous = row
    return width, height, result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for label in ("day", "dusk", "night"):
        parser.add_argument(f"--{label}", type=Path, required=True)
    parser.add_argument("--saturated", type=Path,
                        help="also require saturated edge falloff to match ordinary daylight")
    arguments = parser.parse_args()
    metrics = {}
    captures = {}
    for label, expected_maximum in (("day", 255), ("dusk", 165), ("night", 120)):
        width, height, image = pixels(getattr(arguments, label))
        # The production capture includes the real HUD. This fixed world-only
        # rectangle encloses the player and gray floor without touching any
        # widgets in the pinned 1024x640 layout. Do not mask by pixel color:
        # that could hide a renderer color-contamination regression.
        world = [image[y * width + x] for y in range(240, 401) for x in range(350, 701)]
        histogram = collections.Counter(red for red, green, blue in world)
        if any(red != green or red != blue for red, green, blue in world):
            raise SystemExit(f"{label}: neutral world rectangle has unexpected colored pixels")
        maximum = max(histogram)
        if maximum != expected_maximum or histogram[maximum] < 100:
            raise SystemExit(f"{label}: player maximum/count {maximum}/{histogram[maximum]}, "
                             f"expected {expected_maximum}/at least 100")
        captures[label] = world
        metrics[label] = {"maximum": maximum, "maximum_pixels": histogram[maximum],
                          "nonblack_pixels": len(world) - histogram[0],
                          "floor_rgb": image[350 * width + 500]}
    brighter = sum(day[0] > night[0] for day, night in zip(captures["day"], captures["night"]))
    if brighter < metrics["day"]["nonblack_pixels"] * 0.9:
        raise SystemExit("day/night difference does not affect at least 90% of the visible world")
    metrics["day_brighter_than_night_pixels"] = brighter
    metrics["world_rectangle"] = [350, 240, 701, 401]
    if arguments.saturated:
        width, height, day = pixels(arguments.day)
        _, _, saturated = pixels(arguments.saturated)
        # Three exposed boundaries avoid the inventory that covers the right
        # side. Tall test pillars do not intersect these diagnostic rays.
        rays = {
            "left": [(x, 320) for x in range(130, 221)],
            "top": [(512, y) for y in range(130, 177)],
            "bottom": [(512, y) for y in range(464, 511)],
        }
        metrics["saturated_edges"] = {}
        for name, coordinates in rays.items():
            maximum_difference = max(
                abs(day[y * width + x][channel] - saturated[y * width + x][channel])
                for x, y in coordinates for channel in range(3)
            )
            metrics["saturated_edges"][name] = maximum_difference
            if maximum_difference > 2:
                raise SystemExit(f"{name}: saturated edge differs from daylight by "
                                 f"{maximum_difference} codes (maximum 2)")
    print(json.dumps(metrics, sort_keys=True))


if __name__ == "__main__":
    main()
