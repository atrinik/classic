from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
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

    def test_paginated_release_lines_parse_without_unsupported_slurp(self) -> None:
        result = subprocess.CompletedProcess(
            [],
            0,
            stdout='{"tag_name":"v5.8.1","draft":true}\n'
            '{"tag_name":"v5.8.0","draft":false}\n',
            stderr="",
        )
        self.assertEqual(
            github_release.parse_json_lines(result, "releases"),
            [
                {"tag_name": "v5.8.1", "draft": True},
                {"tag_name": "v5.8.0", "draft": False},
            ],
        )

    def test_paginated_release_lines_reject_invalid_values(self) -> None:
        for output in ("not-json\n", "[]\n"):
            with self.subTest(output=output):
                with self.assertRaises(github_release.GitHubReleaseError):
                    github_release.parse_json_lines(
                        subprocess.CompletedProcess([], 0, stdout=output, stderr=""),
                        "releases",
                    )


if __name__ == "__main__":
    unittest.main()
