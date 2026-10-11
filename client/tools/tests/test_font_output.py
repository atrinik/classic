# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the probe's real output path on either native CRT."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import sys
import tempfile


PAYLOAD = bytes((0, 10, 13, 26, 255))


def invoke(probe: Path, directory: Path) -> int:
    result = subprocess.run(
        [str(probe), "--test-output", str(directory)],
        capture_output=True, timeout=15, check=False,
    )
    if result.returncode not in (0, 1):
        raise AssertionError(f"unexpected probe exit {result.returncode}: {result.stderr!r}")
    return result.returncode


def check(probe: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="font-output-") as temporary:
        root = Path(temporary)
        directory = root / "output with spaces"
        directory.mkdir()
        output = directory / "font-output-test.bin"
        assert invoke(probe, directory) == 0, "valid exclusive creation failed"
        assert output.read_bytes() == PAYLOAD, "binary output was translated"
        output.write_bytes(b"earlier evidence\x00\xff")
        assert invoke(probe, directory) == 1, "existing output was accepted"
        assert output.read_bytes() == b"earlier evidence\x00\xff", "existing output changed"

        missing = root / "missing" / "directory"
        assert invoke(probe, missing) == 1, "missing parent was accepted"
        assert not (root / "missing").exists(), "failed open created a parent"
        parent_file = root / "regular-file"
        parent_file.write_bytes(PAYLOAD)
        assert invoke(probe, parent_file) == 1, "regular file parent was accepted"
        assert parent_file.read_bytes() == PAYLOAD, "invalid parent changed"
        invalid = root / "directory-output"
        invalid.mkdir()
        (invalid / output.name).mkdir()
        assert invoke(probe, invalid) == 1, "directory output was accepted"
        assert (invalid / output.name).is_dir(), "directory output changed"
        assert invoke(probe, root / ("x" * 4096)) == 1, "path bound was ignored"

        competing = root / "competing"
        competing.mkdir()
        with ThreadPoolExecutor(max_workers=8) as workers:
            results = list(workers.map(lambda _: invoke(probe, competing), range(8)))
        assert results.count(0) == 1, f"exclusive creators: {results}"
        assert (competing / output.name).read_bytes() == PAYLOAD, "competing creator clobbered output"
        assert list(competing.iterdir()) == [competing / output.name], "unexpected failed-open artifact"
    print(f"font output regression passed: {probe.name}")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--cleanup":
        with tempfile.TemporaryDirectory(prefix="font-output-cleanup-") as temporary:
            subprocess.run([sys.argv[2], temporary], check=True, timeout=15)
            assert (Path(temporary) / "font-output-test.bin").read_bytes() == b"", "failed evidence changed"
        print("font output stream failure cleanup passed")
    elif len(sys.argv) == 2:
        check(Path(sys.argv[1]).resolve(strict=True))
    else:
        raise SystemExit("usage: test_font_output.py FONT_PROBE")
