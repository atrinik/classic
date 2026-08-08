from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "check_latest_release.py"
SPEC = importlib.util.spec_from_file_location("check_latest_release", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
check_latest_release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_latest_release)


def release(tag: str = "v5.6.0") -> dict[str, object]:
    release_version = tag.removeprefix("v")
    return {
        "id": 1,
        "tag_name": tag,
        "draft": False,
        "prerelease": False,
        "immutable": True,
        "assets": [
            {
                "name": name,
                "state": "uploaded",
                "size": 1,
                "digest": "sha256:" + "a" * 64,
            }
            for name in check_latest_release.expected_names(release_version)
        ],
    }


class CheckLatestReleaseTests(unittest.TestCase):
    def test_complete_immutable_latest_release_is_accepted(self) -> None:
        self.assertEqual(
            check_latest_release.validate_latest(release()),
            ("v5.6.0", "5.6.0"),
        )

    def test_draft_mutable_or_incomplete_release_fails_closed(self) -> None:
        cases = [
            {**release(), "draft": True},
            {**release(), "immutable": False},
            {**release(), "assets": []},
        ]
        for value in cases:
            with self.subTest(value=value):
                with self.assertRaises(check_latest_release.LatestTagError):
                    check_latest_release.validate_latest(value)

    def test_pre_unification_or_new_major_tag_fails_closed(self) -> None:
        for tag in ("v5.5.1", "v6.0.0"):
            with self.subTest(tag=tag):
                with self.assertRaises(check_latest_release.LatestTagError):
                    check_latest_release.validate_latest(release(tag))

    def test_highest_semantic_release_wins_over_publication_order(self) -> None:
        older = {**release("v5.6.0"), "id": 10}
        newer = {**release("v5.6.1"), "id": 11}
        self.assertEqual(
            check_latest_release.select_latest([older, newer]),
            newer,
        )

    def test_highest_published_release_must_be_complete_and_immutable(self) -> None:
        incomplete = {**release("v5.6.1"), "id": 11, "immutable": False}
        with self.assertRaises(check_latest_release.LatestTagError):
            check_latest_release.select_latest([release("v5.6.0"), incomplete])

    def test_drafts_do_not_displace_the_highest_published_release(self) -> None:
        draft = {**release("v5.7.0"), "id": 12, "draft": True}
        current = {**release("v5.6.1"), "id": 11}
        self.assertEqual(
            check_latest_release.select_latest([draft, current]),
            current,
        )


if __name__ == "__main__":
    unittest.main()
