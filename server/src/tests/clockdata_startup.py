#!/usr/bin/env python3
"""Verify malformed persisted world-clock state fails closed at startup."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys


SERVER_TIMEOUT_SECONDS = 30


def run_server(executable: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            executable,
            "--content_benchmark=/tests/content_benchmark",
            "--content_benchmark_iterations=1",
            "--logger_filter_stdout=-info,-devel",
            "--port_mapping=off",
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=SERVER_TIMEOUT_SECONDS,
    )


def require_rejection(
    executable: str, clockdata: Path, contents: bytes, surface: str
) -> None:
    clockdata.write_bytes(contents)
    result = run_server(executable)
    output = result.stdout + result.stderr
    if result.returncode == 0:
        raise RuntimeError(f"server accepted malformed {surface} clockdata")
    if "Malformed persisted world clock" not in output:
        raise RuntimeError(f"missing {surface} diagnostic:\n{output}")
    if clockdata.read_bytes() != contents:
        raise RuntimeError(f"server replaced malformed {surface} clockdata")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: clockdata_startup.py SERVER")

    executable = sys.argv[1]
    clockdata = Path("data/clockdata")

    result = run_server(executable)
    if result.returncode != 0 or clockdata.read_bytes() != b"0":
        raise RuntimeError("missing clockdata did not initialize to zero")

    valid_clock = b" \t42\r\n"
    clockdata.write_bytes(valid_clock)
    result = run_server(executable)
    if result.returncode != 0 or clockdata.read_bytes() != valid_clock:
        raise RuntimeError("valid clockdata was not accepted intact")

    require_rejection(executable, clockdata, b"", "empty")
    require_rejection(executable, clockdata, b"123 trailing evidence\n", "trailing-junk")
    require_rejection(executable, clockdata, b"9" * 200, "oversized")

    clockdata.unlink()
    clockdata.mkdir()
    result = run_server(executable)
    output = result.stdout + result.stderr
    if result.returncode == 0 or "Malformed persisted world clock" not in output:
        raise RuntimeError(f"server accepted unreadable clockdata:\n{output}")
    if not clockdata.is_dir():
        raise RuntimeError("server replaced unreadable clockdata")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
