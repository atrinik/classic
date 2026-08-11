#!/usr/bin/env python3
"""Verify malformed persisted world-clock state fails closed at startup."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys


MALFORMED_CLOCK = b"123 trailing evidence\n"


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: clockdata_startup.py SERVER")

    clockdata = Path("data/clockdata")
    clockdata.write_bytes(MALFORMED_CLOCK)
    result = subprocess.run(
        [
            sys.argv[1],
            "--content_benchmark=/tests/content_benchmark",
            "--content_benchmark_iterations=1",
            "--logger_filter_stdout=-info,-devel",
            "--port_mapping=off",
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    output = result.stdout + result.stderr
    if result.returncode == 0:
        raise RuntimeError("server accepted malformed persisted world clock")
    if "Malformed persisted world clock" not in output:
        raise RuntimeError(f"missing malformed-clock diagnostic:\n{output}")
    if clockdata.read_bytes() != MALFORMED_CLOCK:
        raise RuntimeError("server replaced malformed persisted world clock")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
