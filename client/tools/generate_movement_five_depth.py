#!/usr/bin/env python3
"""Expand a validated MAP_UPDATE_CMD_NEW fixture to all five physical depths."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


DEPTHS = (-2, -1, 0, 1, 2)


def new_header_end(packet: bytes) -> int:
    if not packet or packet[0] != 1:
        raise ValueError("source must begin with MAP_UPDATE_CMD_NEW")
    cursor = 1
    for _ in range(3):
        cursor = packet.index(b"\0", cursor) + 1
    cursor += 2
    for _ in range(3):
        cursor = packet.index(b"\0", cursor) + 1
    cursor += 5
    return cursor


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


def expanded(packet: bytes) -> bytes:
    header_end = new_header_end(packet)
    payloads = level_payloads(packet)
    result = bytearray(packet[:header_end])
    result.extend(b"\0\0")
    result.append(len(DEPTHS))
    for index, depth in enumerate(DEPTHS):
        payload = payloads[index % len(payloads)]
        result.extend(struct.pack(">bI", depth, len(payload)))
        result.extend(payload)
    return bytes(result)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    source = bytes.fromhex(arguments.input.read_text(encoding="ascii"))
    arguments.output.write_text(expanded(source).hex() + "\n", encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
