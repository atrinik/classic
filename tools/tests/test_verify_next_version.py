from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "verify_next_version.py"
SPEC = importlib.util.spec_from_file_location("verify_next_version", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
verify_next_version = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify_next_version)


class VerifyNextVersionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.policy = Path(self.temporary.name) / "release-tags.json"
        self.policy.write_text(
            json.dumps(
                {
                    "future_tags": {
                        "first_version": "v5.6.0",
                        "minimum_version": "v5.6.0",
                        "maximum_major": 5,
                    },
                    "tags": {},
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_initial_release_must_be_exactly_the_first_version(self) -> None:
        verify_next_version.verify_next_version("5.6.0", self.policy, set())
        for version in ("5.6.1", "5.7.0", "5.10.0"):
            with self.subTest(version=version):
                with self.assertRaisesRegex(
                    verify_next_version.VersionPolicyError, "first unified tag"
                ):
                    verify_next_version.verify_next_version(version, self.policy, set())

    def test_later_release_must_increase_on_the_same_major_line(self) -> None:
        active = {"v5.6.0", "v5.6.1"}
        verify_next_version.verify_next_version("5.7.0", self.policy, active)
        with self.assertRaisesRegex(
            verify_next_version.VersionPolicyError, "must exceed"
        ):
            verify_next_version.verify_next_version("5.6.1", self.policy, active)

    def test_rejects_versions_outside_the_unified_line(self) -> None:
        for version in ("5.5.9", "6.0.0", "4.99.0", "v5.6.0", "5.6"):
            with self.subTest(version=version):
                with self.assertRaises(verify_next_version.VersionPolicyError):
                    verify_next_version.verify_next_version(version, self.policy, set())


if __name__ == "__main__":
    unittest.main()
