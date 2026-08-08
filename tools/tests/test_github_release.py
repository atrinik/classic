from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "github_release.py"
SPEC = importlib.util.spec_from_file_location("github_release", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
github_release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(github_release)


class GitHubReleaseTests(unittest.TestCase):
    def test_finds_authenticated_draft_by_tag(self) -> None:
        draft = {"tag_name": "v5.6.0", "draft": True}
        self.assertIs(
            github_release.find_unique_release([draft], "v5.6.0"), draft
        )

    def test_duplicate_tag_state_fails_closed(self) -> None:
        releases = [{"tag_name": "v5.6.0"}, {"tag_name": "v5.6.0"}]
        with self.assertRaises(github_release.GitHubReleaseError):
            github_release.find_unique_release(releases, "v5.6.0")


if __name__ == "__main__":
    unittest.main()
