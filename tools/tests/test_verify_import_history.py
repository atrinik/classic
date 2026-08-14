from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "verify_import_history.py"
SPEC = importlib.util.spec_from_file_location("verify_import_history", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
verify_import_history = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify_import_history)


class ReleaseTagPolicyTests(unittest.TestCase):
    def git(self, root: Path, *arguments: str) -> str:
        return subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        ).stdout.strip()

    def test_semantic_versions_sort_numerically(self) -> None:
        tags = ["v5.10.0", "v5.6.2", "v5.7.0", "v5.6.0"]
        self.assertEqual(
            sorted(tags, key=verify_import_history.semantic_version),
            ["v5.6.0", "v5.6.2", "v5.7.0", "v5.10.0"],
        )

    def test_prefixed_or_partial_versions_are_rejected(self) -> None:
        for tag in ("client-v5.6.0", "5.6.0", "v5.6", "v5.6.0-rc.1"):
            with self.subTest(tag=tag):
                with self.assertRaisesRegex(RuntimeError, "unprefixed semantic version"):
                    verify_import_history.semantic_version(tag)

    def test_release_history_ref_defaults_to_head(self) -> None:
        self.assertEqual(
            verify_import_history.parse_args([]).release_history_ref,
            "HEAD",
        )
        self.assertEqual(
            verify_import_history.parse_args(
                ["--release-history-ref", "origin/main"]
            ).release_history_ref,
            "origin/main",
        )

    def test_main_first_parent_survives_feature_merge_topology(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.git(root, "init", "--initial-branch=main")
            self.git(root, "config", "user.name", "Atrinik CI")
            self.git(root, "config", "user.email", "ci@atrinik.org")
            marker = root / "marker"
            marker.write_text("base\n", encoding="utf-8")
            self.git(root, "add", "marker")
            self.git(root, "commit", "-m", "base")
            base = self.git(root, "rev-parse", "HEAD")
            self.git(root, "branch", "feature", base)

            marker.write_text("base\nmain\n", encoding="utf-8")
            self.git(root, "commit", "-am", "main release")
            main_release = self.git(root, "rev-parse", "HEAD")

            self.git(root, "switch", "feature")
            feature = root / "feature"
            feature.write_text("feature\n", encoding="utf-8")
            self.git(root, "add", "feature")
            self.git(root, "commit", "-m", "feature change")
            self.git(root, "merge", "--no-ff", "main", "-m", "merge main")

            with mock.patch.object(verify_import_history, "ROOT", root):
                self.assertNotIn(
                    main_release,
                    verify_import_history.first_parent_commits("HEAD"),
                )
                self.assertIn(
                    main_release,
                    verify_import_history.first_parent_commits("main"),
                )


if __name__ == "__main__":
    unittest.main()
