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
            "viewport": [1024, 780],
            "book_editor_ui": [
                {"name": name, "pixels_sha256": f"{index:064x}",
                 "output_size": [1024, 780], "artifact": f"review/{name}.png",
                 "artifact_sha256": "b" * 64, "command": "", "asynchronous": False,
                 "steady_state": {"uploads": 1, "upload_bytes": 16,
                                  "slot_uniform_uploads": 1, "slot_uniform_upload_bytes": 16,
                                  "resource_creations": 0, "resource_destructions": 0,
                                  "readbacks": 0, "fallbacks": 0}}
                for index, name in enumerate(coverage_verifier.BOOK_EDITOR_UI_STATES)
            ],
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

    def test_rejects_missing_production_book_popup_state(self) -> None:
        record = self.record()
        record["book_editor_ui"].pop()
        with self.assertRaisesRegex(ValueError, "book popup UI sweep is incomplete"):
            self.verify_record(record)

    def test_rejects_book_popup_missing_dimensions(self) -> None:
        record = self.record()
        del record["viewport"]
        for state in record["book_editor_ui"]:
            del state["output_size"]
        with self.assertRaisesRegex(ValueError, "viewport is invalid"):
            self.verify_record(record)

    def test_rejects_book_popup_noninteger_dimensions(self) -> None:
        record = self.record()
        record["book_editor_ui"][0]["output_size"] = [1024.0, 780.0]
        with self.assertRaisesRegex(ValueError, "pixel checkpoint is invalid"):
            self.verify_record(record)

    def test_rejects_book_popup_without_review_artifact(self) -> None:
        record = self.record()
        record["book_editor_ui"][0]["artifact_sha256"] = ""
        with self.assertRaisesRegex(ValueError, "book popup review artifact is absent"):
            self.verify_record(record)

    def test_rejects_book_popup_fallback(self) -> None:
        record = self.record()
        record["book_editor_ui"][0]["steady_state"]["fallbacks"] = 1
        with self.assertRaisesRegex(ValueError, "steady frame is not retained"):
            self.verify_record(record)

    def test_rejects_indistinguishable_book_popup_confirmation(self) -> None:
        record = self.record()
        record["book_editor_ui"][1]["pixels_sha256"] = record["book_editor_ui"][0]["pixels_sha256"]
        with self.assertRaisesRegex(ValueError, "visual transitions are indistinguishable"):
            self.verify_record(record)


if __name__ == "__main__":
    unittest.main()
