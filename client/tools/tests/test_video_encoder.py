#!/usr/bin/env python3
"""Integration tests for the production client's isolated video encoder."""

from __future__ import annotations

import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import unittest

if sys.platform.startswith("linux"):
    import fcntl
    import resource


if len(sys.argv) < 2:
    raise SystemExit("usage: test_video_encoder.py CLIENT_EXECUTABLE")
CLIENT_EXECUTABLE = Path(sys.argv.pop(1)).resolve(strict=True)


def frame(
    index: int,
    width: int,
    height: int,
    pixels: bytes,
    byte_count: int | None = None,
) -> bytes:
    count = len(pixels) if byte_count is None else byte_count
    return b"FRAM" + struct.pack("<IIII", index, width, height, count) + pixels


def run_encoder(output: Path | str, payload: bytes) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(CLIENT_EXECUTABLE), "--video-encoder", os.fspath(output)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=15,
    )


def riff_chunks(data: bytes, start: int, end: int):
    offset = start
    while offset + 8 <= end:
        chunk_id = data[offset : offset + 4]
        size = struct.unpack_from("<I", data, offset + 4)[0]
        body = offset + 8
        limit = body + size
        if limit > end:
            raise AssertionError(f"chunk {chunk_id!r} exceeds its RIFF container")
        yield chunk_id, body, limit
        offset = limit + (size & 1)
    if offset != end:
        raise AssertionError("unaligned bytes at end of RIFF container")


def avi_chunks(data: bytes):
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"AVI ":
        raise AssertionError("missing AVI RIFF header")
    if struct.unpack_from("<I", data, 4)[0] + 8 != len(data):
        raise AssertionError("incorrect RIFF size")
    found: dict[bytes, list[bytes]] = {}

    def visit(start: int, end: int) -> None:
        for chunk_id, body, limit in riff_chunks(data, start, end):
            if chunk_id == b"LIST":
                if body + 4 > limit:
                    raise AssertionError("truncated LIST")
                visit(body + 4, limit)
            else:
                found.setdefault(chunk_id, []).append(data[body:limit])

    visit(12, len(data))
    return found


class VideoEncoderTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform.startswith("linux") and Path("/proc/self/fd").is_dir(),
                         "requires Linux procfs descriptor inspection")
    def test_closes_high_inherited_descriptor_above_lowered_limit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            secret = root / "private-input"
            secret.write_bytes(b"must not be inherited")
            output = root / "recording.avi"
            soft_limit, hard_limit = resource.getrlimit(resource.RLIMIT_NOFILE)
            if soft_limit <= 256 or (hard_limit != resource.RLIM_INFINITY and hard_limit < 64):
                self.skipTest("descriptor limit cannot provide the required high descriptor")
            source_descriptor = os.open(secret, os.O_RDONLY)
            try:
                descriptor = fcntl.fcntl(source_descriptor, fcntl.F_DUPFD, 256)
            finally:
                os.close(source_descriptor)
            self.assertGreaterEqual(descriptor, 256)

            def lower_descriptor_limit() -> None:
                resource.setrlimit(resource.RLIMIT_NOFILE, (64, hard_limit))

            process = None
            try:
                process = subprocess.Popen(
                    [str(CLIENT_EXECUTABLE), "--video-encoder", os.fspath(output)],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    pass_fds=(descriptor,),
                    preexec_fn=lower_descriptor_limit,
                )
                self.assertIsNotNone(process.stdout)
                readable, _, _ = select.select([process.stdout], [], [], 15)
                self.assertTrue(readable, "encoder did not report READY within 15 seconds")
                self.assertEqual(process.stdout.readline(), b"READY\n")
                inherited_targets = []
                for entry in Path(f"/proc/{process.pid}/fd").iterdir():
                    try:
                        inherited_targets.append(entry.resolve(strict=True))
                    except FileNotFoundError:
                        pass
                self.assertNotIn(secret.resolve(), inherited_targets)
                pixel = b"\x10\x20\x30\xff"
                payload = b"AVR1" + frame(0, 1, 1, pixel) + b"STOP" + struct.pack("<I", 1)
                remaining_stdout, stderr = process.communicate(payload, timeout=15)
                self.assertEqual(process.returncode, 0, stderr.decode(errors="replace"))
                self.assertEqual(remaining_stdout, b"SAVED\n")
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=15)
                os.close(descriptor)

    def test_sparse_frames_produce_timed_mjpeg_avi(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "recording.avi"
            red = bytes((255, 0, 0, 255)) * 4
            blue = bytes((0, 0, 255, 255)) * 4
            payload = b"AVR1" + frame(0, 2, 2, red) + frame(3, 2, 2, blue)
            payload += b"STOP" + struct.pack("<I", 4)
            result = run_encoder(output, payload)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            self.assertEqual(result.stdout, b"READY\nSAVED\n")
            chunks = avi_chunks(output.read_bytes())
            self.assertEqual(len(chunks.get(b"avih", [])), 1)
            avih = chunks[b"avih"][0]
            self.assertGreaterEqual(len(avih), 40)
            self.assertEqual(struct.unpack_from("<I", avih, 0)[0], 50_000)
            self.assertEqual(struct.unpack_from("<I", avih, 16)[0], 4)
            self.assertEqual(struct.unpack_from("<II", avih, 32), (2, 2))
            self.assertEqual(len(chunks.get(b"strh", [])), 1)
            stream_header = chunks[b"strh"][0]
            self.assertGreaterEqual(len(stream_header), 36)
            self.assertEqual(stream_header[:8], b"vidsMJPG")
            self.assertEqual(struct.unpack_from("<II", stream_header, 20), (1, 20))
            self.assertEqual(struct.unpack_from("<I", stream_header, 32)[0], 4)
            self.assertEqual(len(chunks.get(b"idx1", [])), 1)
            index = chunks[b"idx1"][0]
            self.assertEqual(len(index), 4 * 16)
            self.assertTrue(all(index[i : i + 4] == b"00dc" for i in range(0, len(index), 16)))
            jpeg_chunks = chunks.get(b"00dc", [])
            self.assertEqual(len(jpeg_chunks), 4)
            for jpeg in jpeg_chunks:
                self.assertTrue(jpeg.startswith(b"\xff\xd8"))
                self.assertTrue(jpeg.endswith(b"\xff\xd9"))
            self.assertEqual(jpeg_chunks[0], jpeg_chunks[1])
            self.assertEqual(jpeg_chunks[1], jpeg_chunks[2])
            self.assertNotEqual(jpeg_chunks[2], jpeg_chunks[3])

    def test_rejects_malformed_and_bounded_inputs_and_preserves_partial_file(self) -> None:
        cases = {
            "bad-magic": b"NOPE",
            "unknown": b"AVR1WHAT",
            "truncated-header": b"AVR1FRAM\0",
            "first-index": b"AVR1" + frame(1, 1, 1, b"\0" * 4),
            "huge-width": b"AVR1" + frame(0, 4097, 1, b"", 4097 * 4),
            "huge-pixels": b"AVR1" + frame(0, 4096, 4096, b"", 0),
            "bad-byte-count": b"AVR1" + frame(0, 1, 1, b"\0" * 3),
            "truncated-pixels": b"AVR1" + frame(0, 2, 2, b"\0" * 4, 16),
            "index-limit": b"AVR1"
            + frame(0, 1, 1, b"\0" * 4)
            + frame(72000, 1, 1, b"", 4),
            "stop-too-short": b"AVR1" + frame(0, 1, 1, b"\0" * 4) + b"STOP\0\0\0\0",
        }
        with tempfile.TemporaryDirectory() as directory:
            for name, payload in cases.items():
                with self.subTest(name=name):
                    output = Path(directory) / f"{name}.avi"
                    result = run_encoder(output, payload)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, b"READY\nFAILED\n")
                    self.assertTrue(result.stderr)
                    self.assertTrue(output.exists())

    def test_rejects_duplicate_dimensions_change_and_final_count_overflow(self) -> None:
        pixel = b"\x10\x20\x30\xff"
        cases = {
            "duplicate": b"AVR1" + frame(0, 1, 1, pixel) + frame(0, 1, 1, pixel),
            "dimensions": b"AVR1" + frame(0, 1, 1, pixel) + frame(1, 2, 1, pixel * 2),
            "frame-count": b"AVR1" + frame(0, 1, 1, pixel) + b"STOP" + struct.pack("<I", 72001),
            "eof": b"AVR1" + frame(0, 1, 1, pixel),
        }
        with tempfile.TemporaryDirectory() as directory:
            for name, payload in cases.items():
                with self.subTest(name=name):
                    output = Path(directory) / f"{name}.avi"
                    result = run_encoder(output, payload)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, b"READY\nFAILED\n")
                    self.assertGreater(output.stat().st_size, 0)
                    chunks = avi_chunks(output.read_bytes())
                    self.assertEqual(struct.unpack_from("<I", chunks[b"avih"][0], 16)[0], 1)
                    self.assertEqual(len(chunks[b"idx1"][0]), 16)
                    self.assertEqual(len(chunks[b"00dc"]), 1)
                    self.assertTrue(chunks[b"00dc"][0].startswith(b"\xff\xd8"))
                    self.assertTrue(chunks[b"00dc"][0].endswith(b"\xff\xd9"))

    def test_output_path_is_literal_exclusive_and_private(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            literal = root / "video $(touch injected); [x].avi"
            payload = b"AVR1" + frame(0, 1, 1, b"\x01\x02\x03\xff")
            payload += b"STOP" + struct.pack("<I", 1)
            result = run_encoder(literal, payload)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            self.assertTrue(literal.is_file())
            self.assertFalse((root / "injected").exists())
            if os.name != "nt":
                self.assertEqual(literal.stat().st_mode & 0o777, 0o600)

            occupied = root / "occupied.avi"
            occupied.write_bytes(b"keep")
            result = run_encoder(occupied, payload)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"FAILED\n")
            self.assertEqual(occupied.read_bytes(), b"keep")

            if hasattr(os, "symlink"):
                target = root / "target.avi"
                target.write_bytes(b"target")
                link = root / "link.avi"
                try:
                    link.symlink_to(target)
                except OSError:
                    pass
                else:
                    result = run_encoder(link, payload)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(target.read_bytes(), b"target")

    def test_rejects_relative_and_newline_paths_before_creation(self) -> None:
        payload = b"AVR1"
        for path in ("", "relative.avi"):
            with self.subTest(path=path):
                result = run_encoder(path, payload)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"FAILED\n")
        with tempfile.TemporaryDirectory() as directory:
            newline = f"{directory}/bad\nname.avi"
            result = run_encoder(newline, payload)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"FAILED\n")
            self.assertFalse(Path(newline).exists())
            if os.name == "nt":
                long_path = "C:\\" + "a" * 4093
            else:
                long_path = "/" + "a" * 4095
            self.assertGreater(len(long_path), 4095)
            result = run_encoder(long_path, payload)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"FAILED\n")


if __name__ == "__main__":
    unittest.main()
