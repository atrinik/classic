#!/usr/bin/env python3
"""Generate a closed five-depth MAP2 movement stream for the replay fixture."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


DEPTHS = (-2, -1, 0, 1, 2)
STREAM_MAGIC = b"PVM1"


def tile_delta(x: int, y: int, face: int, scalar: int, rgb: tuple[int, int, int]) -> bytes:
    """One representative floor and colored-light update."""
    mask = x << 11 | y << 6 | 0x4
    result = bytearray(struct.pack(">HHB", mask, scalar, 1))
    result.extend(struct.pack(">BHBB", 0, face, 0, 0))
    result.extend(struct.pack(">BBHHH", 0x2, 0x1, *rgb))
    return bytes(result)


def packet(
    x: int,
    y: int,
    clear_x: int | None = None,
    clear_y: int | None = None,
    update_x: int | None = None,
    update_y: int | None = None,
    face: int = 1,
    scalar: int = 0x0255,
    rgb: tuple[int, int, int] = (0x02D7, 0x0036, 0x0036),
) -> bytes:
    """One MAP_UPDATE_CMD_SAME packet for every active depth."""
    header = bytes((0, x, y, 0, 0, 0, len(DEPTHS)))
    result = bytearray(header)
    for depth in DEPTHS:
        payload = bytearray()
        if clear_x is not None and clear_y is not None:
            payload.extend(struct.pack(">H", clear_x << 11 | clear_y << 6 | 0x2))
        if update_x is not None and update_y is not None:
            payload.extend(tile_delta(update_x, update_y, face, scalar, rgb))
        result.extend(struct.pack(">bI", depth, len(payload)))
        result.extend(payload)
    return bytes(result)


def stream(x: int, y: int) -> bytes:
    """A→B→A→C→A movement followed by an unchanged idle packet."""
    packets = (
        packet(x + 1, y, 20, y, 20, y + 1, 1, 0x0255, (0x02D7, 0x0036, 0x0036)),
        packet(x, y, 0, y, 0, y - 1, 2, 0x01B3, (0x0031, 0x0291, 0x0031)),
        packet(x, y + 1, x, 20, x + 1, 20, 3, 0x014D, (0x0030, 0x0080, 0x0080)),
        packet(x, y, x, 0, x - 1, 0, 4, 0x01F5, (0x01F5, 0x002F, 0x01F5)),
        packet(x, y),
    )
    result = bytearray(STREAM_MAGIC)
    result.append(len(packets))
    for movement_packet in packets:
        result.extend(struct.pack(">I", len(movement_packet)))
        result.extend(movement_packet)
    return bytes(result)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--x", type=int, default=10)
    parser.add_argument("--y", type=int, default=10)
    arguments = parser.parse_args()
    if not 1 <= arguments.x <= 11 or not 1 <= arguments.y <= 11:
        parser.error("origin coordinates must be in [1, 11]")
    arguments.output.write_text(stream(arguments.x, arguments.y).hex() + "\n",
                                encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
