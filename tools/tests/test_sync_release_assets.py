from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "sync_release_assets.py"
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("sync_release_assets", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
sync_release_assets = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sync_release_assets)


class SyncReleaseAssetsTests(unittest.TestCase):
    def test_matching_partial_draft_returns_only_missing_assets(self) -> None:
        expected = {
            "one.zip": (3, "sha256:abc"),
            "two.zip": (4, "sha256:def"),
        }
        assets = [
            {
                "name": "one.zip",
                "size": 3,
                "digest": "sha256:abc",
                "state": "uploaded",
            }
        ]
        self.assertEqual(
            sync_release_assets.compare_assets(expected, assets), ["two.zip"]
        )

    def test_mismatch_or_unexpected_asset_is_rejected(self) -> None:
        expected = {"one.zip": (3, "sha256:abc")}
        for assets in (
            [{"name": "one.zip", "size": 3, "digest": "sha256:bad", "state": "uploaded"}],
            [{"name": "extra.zip", "size": 3, "digest": "sha256:abc", "state": "uploaded"}],
        ):
            with self.subTest(assets=assets):
                with self.assertRaises(sync_release_assets.AssetSyncError):
                    sync_release_assets.compare_assets(expected, assets)

    def test_published_release_must_be_immutable(self) -> None:
        release = {
            "tag_name": "v5.6.0",
            "draft": False,
            "prerelease": False,
            "immutable": True,
        }
        self.assertIs(
            sync_release_assets.require_published_immutable(release, "v5.6.0"),
            release,
        )
        with self.assertRaises(sync_release_assets.AssetSyncError):
            sync_release_assets.require_published_immutable(
                {**release, "immutable": False}, "v5.6.0"
            )

    def test_verified_release_id_is_positive_and_numeric(self) -> None:
        release = {"tag_name": "v5.37.0", "id": 371791046}
        self.assertEqual(
            sync_release_assets.require_release_id(release, "v5.37.0"), 371791046
        )
        for invalid in (
            None,
            {"tag_name": "v5.36.0", "id": 371791046},
            {"tag_name": "v5.37.0", "id": 0},
            {"tag_name": "v5.37.0", "id": True},
            {"tag_name": "v5.37.0", "id": "371791046"},
            {"tag_name": "v5.37.0"},
        ):
            with self.subTest(invalid=invalid):
                with self.assertRaises(sync_release_assets.AssetSyncError):
                    sync_release_assets.require_release_id(invalid, "v5.37.0")

    def test_write_output_propagates_the_verified_release_id(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "github-output"
            sync_release_assets.write_output(output, "draft", 371791046)
            self.assertEqual(
                output.read_text(encoding="utf-8"),
                "state=draft\nrelease_id=371791046\n",
            )

    def test_verify_only_writes_verified_release_id_through_cli(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            assets_directory = Path(directory) / "assets"
            assets_directory.mkdir()
            for index in range(12):
                (assets_directory / f"asset-{index}.bin").write_bytes(bytes([index]))
            expected = sync_release_assets.expected_assets(assets_directory)
            release = {
                "tag_name": "v5.37.0",
                "draft": True,
                "prerelease": False,
                "id": 371791046,
                "assets": [
                    {
                        "name": name,
                        "size": size,
                        "digest": digest,
                        "state": "uploaded",
                    }
                    for name, (size, digest) in expected.items()
                ],
            }
            output = Path(directory) / "github-output"
            arguments = [
                "sync_release_assets.py",
                "--directory",
                str(assets_directory),
                "--repository",
                "atrinik/classic",
                "--tag",
                "v5.37.0",
                "--verify-only",
                "--github-output",
                str(output),
            ]
            with mock.patch.object(
                sync_release_assets, "lookup_release", return_value=release
            ):
                with mock.patch.object(sys, "argv", arguments):
                    self.assertEqual(sync_release_assets.main(), 0)
            self.assertEqual(
                output.read_text(encoding="utf-8"),
                "state=draft\nrelease_id=371791046\n",
            )


if __name__ == "__main__":
    unittest.main()
