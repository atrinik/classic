#!/usr/bin/env python3

from __future__ import annotations

import json
import importlib.util
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "verify_gpu_coverage_record.py"
SPEC = importlib.util.spec_from_file_location("verify_gpu_coverage_record", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
coverage_verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(coverage_verifier)
ROOT_GLYPHS = coverage_verifier.ROOT_GLYPHS
verify = coverage_verifier.verify


REVISION = "a" * 40


class VerifyGpuCoverageRecordTests(unittest.TestCase):
    def record(self) -> dict:
        return {
            "fixture": "gpu-ui-closure",
            "revision": REVISION,
            "dirty": False,
            "ui_closure": [
                {"name": name, "root_glyphs": values.copy()}
                for name, values in ROOT_GLYPHS.items()
            ],
        }

    def verify_record(self, record: dict) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "record.jsonl"
            path.write_text(json.dumps(record) + "\n", encoding="utf-8")
            verify(path, REVISION)

    def test_accepts_exact_clean_record(self) -> None:
        self.verify_record(self.record())

    def test_rejects_stale_revision(self) -> None:
        record = self.record()
        record["revision"] = "b" * 40
        with self.assertRaisesRegex(ValueError, "exact clean source revision"):
            self.verify_record(record)

    def test_rejects_suppressed_root_glyph(self) -> None:
        record = self.record()
        record["ui_closure"][0]["root_glyphs"]["count"] -= 1
        with self.assertRaisesRegex(ValueError, "root glyph submission contract changed"):
            self.verify_record(record)


if __name__ == "__main__":
    unittest.main()
