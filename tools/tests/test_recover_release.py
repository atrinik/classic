from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import call, patch


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "recover_release.py"
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("recover_release", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
recover_release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(recover_release)


class RecoverReleaseTests(unittest.TestCase):
    def test_source_branch_accepts_main_and_maintenance_lines(self) -> None:
        self.assertEqual(recover_release.source_branch("main"), ("main", None))
        self.assertEqual(
            recover_release.source_branch("8.3.x"), ("8.3.x", (8, 3))
        )
        with self.assertRaisesRegex(
            recover_release.RecoveryError, "source branch must be"
        ):
            recover_release.source_branch("feature/release")

    def test_recovery_main_uses_the_source_maintenance_line(self) -> None:
        tag_commit = "a" * 40
        source_commit = "b" * 40
        with (
            patch.object(
                sys,
                "argv",
                [
                    "recover_release.py",
                    "--tag",
                    "v8.3.1",
                    "--repository",
                    "atrinik/classic",
                    "--source-branch",
                    "8.3.x",
                ],
            ),
            patch.object(recover_release, "verify_version_policy"),
            patch.object(recover_release, "lookup_release", return_value=None),
            patch.object(
                recover_release,
                "command",
                side_effect=[
                    tag_commit,
                    source_commit,
                    source_commit,
                    f"{tag_commit}\n",
                    json.dumps(
                        {
                            "check_runs": [
                                {
                                    "name": "Classic validation",
                                    "conclusion": "success",
                                    "app": {"id": 15368},
                                }
                            ]
                        }
                    ),
                    "v8.3.0\n",
                ],
            ) as command,
            patch.object(recover_release.subprocess, "run") as process_run,
        ):
            process_run.return_value.returncode = 0
            self.assertEqual(recover_release.main(), 0)

        self.assertIn(
            call("git", "rev-parse", "refs/remotes/origin/8.3.x"),
            command.call_args_list,
        )

    def test_recovery_rejects_a_tag_outside_the_source_maintenance_line(self) -> None:
        with (
            patch.object(
                sys,
                "argv",
                [
                    "recover_release.py",
                    "--tag",
                    "v8.4.1",
                    "--repository",
                    "atrinik/classic",
                    "--source-branch",
                    "8.3.x",
                ],
            ),
            patch.object(recover_release, "verify_version_policy"),
        ):
            with self.assertRaises(SystemExit) as error:
                recover_release.main()
        self.assertEqual(error.exception.code, 1)

    def test_recovery_requires_the_current_source_maintenance_commit(self) -> None:
        with (
            patch.object(
                sys,
                "argv",
                [
                    "recover_release.py",
                    "--tag",
                    "v8.3.1",
                    "--repository",
                    "atrinik/classic",
                    "--source-branch",
                    "8.3.x",
                ],
            ),
            patch.object(recover_release, "verify_version_policy"),
            patch.object(
                recover_release,
                "command",
                side_effect=["a" * 40, "b" * 40, "c" * 40],
            ),
        ):
            with self.assertRaises(SystemExit) as error:
                recover_release.main()
        self.assertEqual(error.exception.code, 1)

    def test_recovery_version_stays_on_unified_line(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            policy = Path(temporary) / "policy.json"
            policy.write_text(
                json.dumps(
                    {
                        "future_tags": {
                            "minimum_version": "v5.6.0",
                            "maximum_major": 5,
                        }
                    }
                ),
                encoding="utf-8",
            )
            recover_release.verify_version_policy("v5.6.0", policy)
            for tag in ("v5.5.9", "v6.0.0", "server-v5.6.0"):
                with self.subTest(tag=tag):
                    with self.assertRaises(recover_release.RecoveryError):
                        recover_release.verify_version_policy(tag, policy)

    def test_classic_check_must_be_exact_success(self) -> None:
        self.assertTrue(
            recover_release.has_successful_classic_check(
                {
                    "check_runs": [
                        {
                            "name": "Classic validation",
                            "conclusion": "success",
                            "app": {"id": 15368},
                        }
                    ]
                }
            )
        )
        self.assertFalse(
            recover_release.has_successful_classic_check(
                {
                    "check_runs": [
                        {
                            "name": "Classic validation",
                            "conclusion": "skipped",
                            "app": {"id": 15368},
                        }
                    ]
                }
            )
        )
        self.assertFalse(
            recover_release.has_successful_classic_check(
                {
                    "check_runs": [
                        {
                            "name": "Classic validation",
                            "conclusion": "success",
                            "app": {"id": 1},
                        }
                    ]
                }
            )
        )

    def test_previous_release_is_highest_reachable_version_on_same_major(self) -> None:
        self.assertEqual(
            recover_release.select_previous_release_tag(
                "v5.6.0",
                ["v1.0.11", "v5.4.2", "v5.5.1", "server-v5.9.0", "v6.0.0"],
            ),
            "v5.5.1",
        )
        with self.assertRaisesRegex(
            recover_release.RecoveryError, "no earlier reachable unified release"
        ):
            recover_release.select_previous_release_tag("v5.6.0", ["v6.0.0"])

    def test_recovery_outputs_are_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "github-output"
            recover_release.write_github_output(output, "v5.5.1", "a" * 40)
            self.assertEqual(
                output.read_text(encoding="utf-8"),
                f"previous_tag=v5.5.1\ntag_commit={'a' * 40}\n",
            )


if __name__ == "__main__":
    unittest.main()
