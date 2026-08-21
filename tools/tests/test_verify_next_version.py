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

    def policy_for_eight(self) -> Path:
        policy = Path(self.temporary.name) / "release-tags-8.json"
        policy.write_text(
            json.dumps(
                {
                    "future_tags": {
                        "first_version": "v8.0.0",
                        "minimum_version": "v8.0.0",
                        "maximum_major": 8,
                    },
                    "tags": {},
                }
            ),
            encoding="utf-8",
        )
        return policy

    def test_initial_release_must_be_exactly_the_first_version(self) -> None:
        verify_next_version.verify_next_version("5.6.0", self.policy, set())
        for version in ("5.6.1", "5.7.0", "5.10.0"):
            with self.subTest(version=version):
                with self.assertRaisesRegex(
                    verify_next_version.VersionPolicyError,
                    "patch zero|first unified tag",
                ):
                    verify_next_version.verify_next_version(version, self.policy, set())

    def test_later_release_must_increase_on_the_same_major_line(self) -> None:
        active = {"v5.6.0", "v5.6.1"}
        verify_next_version.verify_next_version("5.7.0", self.policy, active)
        with self.assertRaisesRegex(
            verify_next_version.VersionPolicyError, "patch zero|must exceed"
        ):
            verify_next_version.verify_next_version("5.6.1", self.policy, active)

    def test_rejects_versions_outside_the_unified_line(self) -> None:
        for version in ("5.5.9", "6.0.0", "4.99.0", "v5.6.0", "5.6"):
            with self.subTest(version=version):
                with self.assertRaises(verify_next_version.VersionPolicyError):
                    verify_next_version.verify_next_version(version, self.policy, set())

    def test_mainline_patch_like_commit_starts_the_next_minor_line(self) -> None:
        policy = self.policy_for_eight()
        verify_next_version.verify_next_version(
            "8.1.0", policy, {"v8.0.0"}, branch="main"
        )
        with self.assertRaisesRegex(
            verify_next_version.VersionPolicyError, "patch zero"
        ):
            verify_next_version.verify_next_version(
                "8.0.1", policy, {"v8.0.0"}, branch="main"
            )

    def test_maintenance_branch_stays_in_its_patch_range(self) -> None:
        policy = self.policy_for_eight()
        verify_next_version.verify_next_version(
            "8.3.1", policy, {"v8.0.0", "v8.3.0"}, branch="8.3.x"
        )
        verify_next_version.verify_next_version(
            "8.3.2", policy, {"v8.0.0", "v8.3.0", "v8.3.1"}, branch="8.3.x"
        )
        for version in ("8.3.0", "8.3.1", "8.4.0", "8.2.1"):
            with self.subTest(version=version):
                with self.assertRaises(verify_next_version.VersionPolicyError):
                    verify_next_version.verify_next_version(
                        version, policy, {"v8.0.0", "v8.3.0", "v8.3.1"}, branch="8.3.x"
                    )

    def test_maintenance_branch_requires_a_baseline_and_valid_branch_name(self) -> None:
        policy = self.policy_for_eight()
        with self.assertRaisesRegex(
            verify_next_version.VersionPolicyError, "baseline tag is missing"
        ):
            verify_next_version.verify_next_version(
                "8.3.1", policy, {"v8.0.0"}, branch="8.3.x"
            )
        with self.assertRaisesRegex(
            verify_next_version.VersionPolicyError, "release branch must be"
        ):
            verify_next_version.verify_next_version(
                "8.3.1", policy, {"v8.0.0", "v8.3.0"}, branch="feature/release"
            )


if __name__ == "__main__":
    unittest.main()
