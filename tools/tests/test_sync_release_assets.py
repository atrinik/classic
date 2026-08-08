from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


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


if __name__ == "__main__":
    unittest.main()
