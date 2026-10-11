#!/usr/bin/env python3
"""Failure and bounds checks for exact font evidence comparison."""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "compare_font_render", Path(__file__).parents[1] / "ci" / "compare_font_render.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class FontEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.baseline = Path(cls.temporary.name) / "baseline"
        cls.candidate = Path(cls.temporary.name) / "candidate"
        cls.manifest = {"schema": 1, "cases": []}
        for index, identity in enumerate(probe.CASES):
            cls.manifest["cases"].append(dict(
                zip(("id", "font", "point", "style", "mode", "sample"), (index, *identity)),
                text_size=[1, 1], surface_size=[1, 1], font_metrics=[1, 1, -1, 1],
                glyphs=[[codepoint, 1, 0, 1, -1, 1, 1] for codepoint in probe.GLYPHS]))
        for directory in (cls.baseline, cls.candidate):
            directory.mkdir()
            (directory / "manifest.json").write_text(json.dumps(cls.manifest))
            for index in range(len(probe.CASES)):
                (directory / f"case{index:04d}.rgba").write_bytes(b"\xd4\xa5\x53\xff")

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def tearDown(self):
        (self.candidate / "manifest.json").write_text(json.dumps(self.manifest))
        path = self.candidate / "case0000.rgba"
        if path.is_symlink():
            path.unlink()
        path.write_bytes(b"\xd4\xa5\x53\xff")
        (self.candidate / "unexpected").unlink(missing_ok=True)

    def mutate(self, change):
        manifest = copy.deepcopy(self.manifest)
        change(manifest)
        (self.candidate / "manifest.json").write_text(json.dumps(manifest))

    def rejects(self, message):
        with self.assertRaisesRegex(probe.EvidenceError, message):
            probe.compare(self.baseline, self.candidate)

    def test_complete_exact_comparison(self):
        self.assertEqual(probe.compare(self.baseline, self.candidate), 1680)

    def test_metric_difference(self):
        self.mutate(lambda m: m["cases"][0]["glyphs"][0].__setitem__(6, 2))
        self.rejects("metadata differs")

    def test_pixel_difference(self):
        (self.candidate / "case0000.rgba").write_bytes(b"\xd5\xa5\x53\xff")
        self.rejects("pixels differ")

    def test_noncanonical_transparent_rgb(self):
        (self.candidate / "case0000.rgba").write_bytes(b"\x01\0\0\0")
        self.rejects("noncanonical")

    def test_short_pixel_output(self):
        (self.candidate / "case0000.rgba").write_bytes(b"\0")
        self.rejects("byte count")

    def test_oversized_pixel_output(self):
        (self.candidate / "case0000.rgba").write_bytes(b"\0" * 5)
        self.rejects("too large")

    def test_missing_case(self):
        self.mutate(lambda m: m["cases"].pop())
        self.rejects("incomplete")

    def test_changed_case_identity(self):
        self.mutate(lambda m: m["cases"][0].__setitem__("font", "other.ttf"))
        self.rejects("identity")

    def test_dimension_bound(self):
        self.mutate(lambda m: m["cases"][0].__setitem__("surface_size", [1025, 1]))
        self.rejects("surface bound")

    def test_total_byte_bound(self):
        self.mutate(lambda m: [case.__setitem__("surface_size", [1024, 1024])
                               for case in m["cases"]])
        self.rejects("total pixel bound")

    def test_boolean_is_not_integer(self):
        self.mutate(lambda m: m["cases"][0].__setitem__("id", False))
        self.rejects("case ID")

    def test_unknown_fields(self):
        self.mutate(lambda m: m["cases"][0].__setitem__("extra", 1))
        self.rejects("case fields")

    def test_duplicate_json_field(self):
        (self.candidate / "manifest.json").write_text('{"schema":1,"schema":1,"cases":[]}')
        self.rejects("duplicate JSON")

    def test_manifest_byte_bound(self):
        (self.candidate / "manifest.json").write_bytes(b" " * (probe.MAX_MANIFEST + 1))
        self.rejects("too large")

    def test_unexpected_output(self):
        (self.candidate / "unexpected").touch()
        self.rejects("unexpected evidence")

    def test_missing_output(self):
        (self.candidate / "case0000.rgba").unlink()
        self.rejects("missing or unexpected")

    def test_symlink_output(self):
        path = self.candidate / "case0000.rgba"
        path.unlink()
        path.symlink_to(self.baseline / "case0000.rgba")
        self.rejects("regular evidence")


if __name__ == "__main__":
    unittest.main()
