#!/usr/bin/env python3
"""Generate a closed five-depth MAP2 scroll delta for the movement replay fixture."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


DEPTHS = (-2, -1, 0, 1, 2)


def packet(x: int, y: int, clear_x: int, clear_y: int) -> bytes:
    """One MAP_UPDATE_CMD_SAME packet with a clear delta for every active depth."""
    header = bytes((0, x, y, 0, 0, 0, len(DEPTHS)))
    clear_mask = clear_x << 11 | clear_y << 6 | 0x2
    result = bytearray(header)
    for depth in DEPTHS:
        result.extend(struct.pack(">bI", depth, 2))
        result.extend(struct.pack(">H", clear_mask))
    return bytes(result)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--x", type=int, default=7)
    parser.add_argument("--y", type=int, default=6)
    parser.add_argument("--clear-x", type=int, default=6)
    parser.add_argument("--clear-y", type=int, default=6)
    arguments = parser.parse_args()
    if any(not 0 <= value < 32 for value in
           (arguments.x, arguments.y, arguments.clear_x, arguments.clear_y)):
        parser.error("coordinates must be in [0, 31]")
    arguments.output.write_text(packet(arguments.x, arguments.y,
                                       arguments.clear_x, arguments.clear_y).hex() + "\n",
                                encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
