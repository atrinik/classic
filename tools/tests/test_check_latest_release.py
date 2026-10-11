from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "release"))


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "check_latest_release.py"
SPEC = importlib.util.spec_from_file_location("check_latest_release", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
check_latest_release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_latest_release)


def release(tag: str = "v5.6.0", schema: int = 1) -> dict[str, object]:
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
            for name in check_latest_release.expected_names(release_version, schema)
        ],
    }


class CheckLatestReleaseTests(unittest.TestCase):
    def test_complete_immutable_latest_release_is_accepted(self) -> None:
        self.assertEqual(
            check_latest_release.validate_latest(release(), 1),
            ("v5.6.0", "5.6.0"),
        )

    def test_schema_two_release_requires_deb_and_historical_does_not(self) -> None:
        value = release()
        deb = {"name": "atrinik-classic-client-5.6.0-linux-amd64.deb",
               "state": "uploaded", "size": 1, "digest": "sha256:" + "a" * 64}
        with self.assertRaises(check_latest_release.LatestTagError):
            check_latest_release.validate_latest(value, 2)
        value["assets"].append(deb)
        self.assertEqual(check_latest_release.validate_latest(value, 2), ("v5.6.0", "5.6.0"))
        with self.assertRaises(check_latest_release.LatestTagError):
            check_latest_release.validate_latest(value, 1)

    def test_schema_three_requires_appimage_and_rejects_debian(self) -> None:
        value = release(schema=3)
        self.assertEqual(check_latest_release.validate_latest(value, 3), ("v5.6.0", "5.6.0"))
        image = next(asset for asset in value["assets"] if asset["name"].endswith(".AppImage"))
        image["name"] = "atrinik-classic-client-5.6.0-linux-amd64.deb"
        with self.assertRaises(check_latest_release.LatestTagError):
            check_latest_release.validate_latest(value, 3)

    def test_mixed_history_selects_schema_from_immutable_source_resolver(self) -> None:
        history = [release("v5.6.0", 1), release("v5.7.0", 2), release("v5.8.0", 3)]
        schemas = {"v5.6.0": 1, "v5.7.0": 2, "v5.8.0": 3}
        for index in (1, 2, 3):
            self.assertEqual(check_latest_release.select_latest(history[:index], schemas.__getitem__), history[index - 1])
        with self.assertRaises(check_latest_release.LatestTagError):
            check_latest_release.select_latest(history, lambda _tag: 2)

    def test_latest_requires_an_immutable_source_schema_resolver(self) -> None:
        with self.assertRaisesRegex(check_latest_release.LatestTagError, "resolver"):
            check_latest_release.select_latest([release()], None)

    def test_draft_mutable_or_incomplete_release_fails_closed(self) -> None:
        cases = [
            {**release(), "draft": True},
            {**release(), "immutable": False},
            {**release(), "assets": []},
        ]
        for value in cases:
            with self.subTest(value=value):
                with self.assertRaises(check_latest_release.LatestTagError):
                    check_latest_release.validate_latest(value, 1)

    def test_pre_unification_or_new_major_tag_fails_closed(self) -> None:
        for tag in ("v5.5.1", "v6.0.0"):
            with self.subTest(tag=tag):
                with self.assertRaises(check_latest_release.LatestTagError):
                    check_latest_release.validate_latest(release(tag), 1)

    def test_highest_semantic_release_wins_over_publication_order(self) -> None:
        older = {**release("v5.6.0"), "id": 10}
        newer = {**release("v5.6.1"), "id": 11}
        self.assertEqual(
            check_latest_release.select_latest([older, newer], lambda _tag: 1),
            newer,
        )

    def test_highest_published_release_must_be_complete_and_immutable(self) -> None:
        incomplete = {**release("v5.6.1"), "id": 11, "immutable": False}
        with self.assertRaises(check_latest_release.LatestTagError):
            check_latest_release.select_latest([release("v5.6.0"), incomplete], lambda _tag: 1)

    def test_drafts_do_not_displace_the_highest_published_release(self) -> None:
        draft = {**release("v5.7.0"), "id": 12, "draft": True}
        current = {**release("v5.6.1"), "id": 11}
        self.assertEqual(
            check_latest_release.select_latest([draft, current], lambda _tag: 1),
            current,
        )


if __name__ == "__main__":
    unittest.main()
