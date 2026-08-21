#!/usr/bin/env python3
"""Resume a draft upload or verify an already-published immutable release."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess

from github_release import GitHubReleaseError, lookup_release


class AssetSyncError(RuntimeError):
    """Raised when draft assets differ from the locally finalized candidate."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def expected_assets(directory: Path) -> dict[str, tuple[int, str]]:
    return {
        path.name: (path.stat().st_size, f"sha256:{sha256(path)}")
        for path in directory.iterdir()
        if path.is_file()
    }


def compare_assets(
    expected: dict[str, tuple[int, str]], assets: object
) -> list[str]:
    if not isinstance(assets, list):
        raise AssetSyncError("GitHub release has invalid asset metadata")
    by_name: dict[str, dict[str, object]] = {}
    for asset in assets:
        if not isinstance(asset, dict) or not isinstance(asset.get("name"), str):
            raise AssetSyncError("GitHub release has malformed asset metadata")
        name = str(asset["name"])
        if name in by_name:
            raise AssetSyncError(f"GitHub release has duplicate asset {name}")
        by_name[name] = asset
    extra = sorted(set(by_name) - set(expected))
    if extra:
        raise AssetSyncError(f"draft has unexpected assets: {extra}")
    for name, asset in by_name.items():
        size, digest = expected[name]
        if asset.get("state") != "uploaded":
            raise AssetSyncError(f"draft asset is not uploaded: {name}")
        if asset.get("size") != size or asset.get("digest") != digest:
            raise AssetSyncError(f"draft asset differs from candidate: {name}")
    return sorted(set(expected) - set(by_name))


def require_draft(release: object, tag: str) -> dict[str, object]:
    if not isinstance(release, dict) or release.get("tag_name") != tag:
        raise AssetSyncError(f"GitHub draft for {tag} is missing")
    if release.get("draft") is not True or release.get("prerelease") is not False:
        raise AssetSyncError("release must remain draft and non-prerelease")
    return release


def require_published_immutable(
    release: object, tag: str
) -> dict[str, object]:
    if not isinstance(release, dict) or release.get("tag_name") != tag:
        raise AssetSyncError(f"published GitHub release for {tag} is missing")
    if (
        release.get("draft") is not False
        or release.get("prerelease") is not False
        or release.get("immutable") is not True
    ):
        raise AssetSyncError("release is not published, non-prerelease, and immutable")
    return release


def require_release_id(release: object, tag: str) -> int:
    if not isinstance(release, dict) or release.get("tag_name") != tag:
        raise AssetSyncError(f"GitHub release for {tag} is missing")
    identifier = release.get("id")
    if type(identifier) is not int or identifier <= 0:
        raise AssetSyncError(f"GitHub release for {tag} has no valid numeric ID")
    return identifier


def write_output(path: Path, state: str, release_id: int) -> None:
    with path.open("a", encoding="utf-8") as stream:
        stream.write(f"state={state}\n")
        stream.write(f"release_id={release_id}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag", required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify-published", action="store_true")
    mode.add_argument("--verify-only", action="store_true")
    parser.add_argument("--github-output", type=Path)
    arguments = parser.parse_args()
    directory = arguments.directory.resolve(strict=True)
    try:
        expected = expected_assets(directory)
        if len(expected) != 12:
            raise AssetSyncError(f"candidate must contain exactly 12 assets, got {len(expected)}")
        release_state = lookup_release(arguments.repository, arguments.tag)
        if arguments.verify_published:
            release = require_published_immutable(
                release_state, arguments.tag
            )
            state = "published"
        elif arguments.verify_only:
            if isinstance(release_state, dict) and release_state.get("draft") is False:
                release = require_published_immutable(release_state, arguments.tag)
                state = "published"
            else:
                release = require_draft(release_state, arguments.tag)
                state = "draft"
        else:
            if isinstance(release_state, dict) and release_state.get("draft") is False:
                release = require_published_immutable(release_state, arguments.tag)
                state = "published"
            else:
                release = require_draft(release_state, arguments.tag)
                missing = compare_assets(expected, release.get("assets"))
                for name in missing:
                    result = subprocess.run(
                        [
                            "gh",
                            "release",
                            "upload",
                            arguments.tag,
                            str(directory / name),
                            "--repo",
                            arguments.repository,
                        ],
                        check=False,
                    )
                    if result.returncode:
                        raise AssetSyncError(f"failed to upload draft asset: {name}")
                release = require_draft(
                    lookup_release(arguments.repository, arguments.tag), arguments.tag
                )
                state = "draft"
        remaining = compare_assets(expected, release.get("assets"))
        if remaining:
            raise AssetSyncError(f"release assets are still missing: {remaining}")
        release_id = require_release_id(release, arguments.tag)
        if arguments.github_output is not None:
            write_output(arguments.github_output, state, release_id)
    except (GitHubReleaseError, OSError, ValueError, AssetSyncError) as error:
        parser.exit(1, f"release asset sync failed: {error}\n")
    description = "published immutable" if state == "published" else "draft"
    print(f"verified {len(expected)} {description} assets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
