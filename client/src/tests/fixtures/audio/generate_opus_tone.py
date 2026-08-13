#!/usr/bin/env python3
"""Generate the canonical PCM input for the client Opus decoder fixture."""

import math
import struct
import sys
import wave


SAMPLE_RATE = 48_000
FRAME_COUNT = SAMPLE_RATE * 120 // 1_000
AMPLITUDE = 12_000


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} OUTPUT.wav")

    with wave.open(sys.argv[1], "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE)
        for frame in range(FRAME_COUNT):
            left = round(AMPLITUDE * math.sin(2 * math.pi * 440 * frame / SAMPLE_RATE))
            right = round(AMPLITUDE * math.sin(2 * math.pi * 660 * frame / SAMPLE_RATE))
            output.writeframesraw(struct.pack("<hh", left, right))


if __name__ == "__main__":
    main()
