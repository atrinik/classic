#!/usr/bin/env python3
"""Resolve GitHub's latest complete immutable Classic release."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess


TAG_RE = re.compile(r"v([0-9]+)\.([0-9]+)\.([0-9]+)")
DIGEST_RE = re.compile(r"sha256:[0-9a-f]{64}")
MODULES = ("client", "server", "editor", "libatrinik", "protocol")


class LatestTagError(RuntimeError):
    """Raised when GitHub's latest release is not safe to promote."""


def version(tag: str) -> tuple[int, int, int]:
    match = TAG_RE.fullmatch(tag)
    if match is None:
        raise LatestTagError(f"unexpected release tag: {tag}")
    parsed = tuple(int(part) for part in match.groups())
    if parsed < (5, 6, 0):
        raise LatestTagError("unified releases begin at v5.6.0")
    if parsed[0] != 5:
        raise LatestTagError(f"unexpected release tag: {tag}")
    return parsed  # type: ignore[return-value]


def expected_names(release_version: str) -> set[str]:
    names = {f"atrinik-classic-{release_version}.tar.gz"}
    names.update(
        f"atrinik-classic-{module}-{release_version}.tar.gz"
        for module in MODULES
    )
    names.update(
        {
            f"atrinik-classic-client-{release_version}-windows-x86_64.zip",
            f"atrinik-classic-server-{release_version}-windows-x86_64.zip",
            f"atrinik_classic_protocol-{release_version}-py3-none-any.whl",
            f"atrinik-classic-{release_version}.spdx.json",
            "release-manifest.json",
            "SHA256SUMS",
        }
    )
    return names


def validate_latest(release: object) -> tuple[str, str]:
    if not isinstance(release, dict) or not isinstance(release.get("tag_name"), str):
        raise LatestTagError("selected release has malformed metadata")
    tag = str(release["tag_name"])
    parsed = version(tag)
    release_version = ".".join(str(part) for part in parsed)
    if (
        release.get("draft") is not False
        or release.get("prerelease") is not False
        or release.get("immutable") is not True
    ):
        raise LatestTagError(
            "selected release is not published, stable, and immutable"
        )
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise LatestTagError("selected release has invalid assets")
    actual: set[str] = set()
    for asset in assets:
        if not isinstance(asset, dict) or not isinstance(asset.get("name"), str):
            raise LatestTagError("selected release has malformed asset metadata")
        name = str(asset["name"])
        if name in actual:
            raise LatestTagError(f"selected release repeats asset {name}")
        actual.add(name)
        if (
            asset.get("state") != "uploaded"
            or not isinstance(asset.get("size"), int)
            or int(asset["size"]) <= 0
            or not isinstance(asset.get("digest"), str)
            or DIGEST_RE.fullmatch(str(asset["digest"])) is None
        ):
            raise LatestTagError(f"selected release has incomplete asset {name}")
    expected = expected_names(release_version)
    if actual != expected:
        raise LatestTagError(
            "selected release asset set differs: "
            f"missing={sorted(expected - actual)}, extra={sorted(actual - expected)}"
        )
    return tag, release_version


def select_latest(releases: object) -> dict[str, object]:
    if not isinstance(releases, list):
        raise LatestTagError("GitHub releases API returned malformed metadata")
    candidates: list[tuple[tuple[int, int, int], dict[str, object]]] = []
    for release in releases:
        if not isinstance(release, dict) or not isinstance(
            release.get("tag_name"), str
        ):
            raise LatestTagError("GitHub releases API returned malformed metadata")
        match = TAG_RE.fullmatch(str(release["tag_name"]))
        if match is None:
            continue
        parsed = tuple(int(part) for part in match.groups())
        if parsed < (5, 6, 0):
            continue
        if parsed[0] != 5:
            raise LatestTagError(f"unexpected release tag: {release['tag_name']}")
        if release.get("draft") is False and release.get("prerelease") is False:
            candidates.append((parsed, release))
    if not candidates:
        raise LatestTagError("no published unified Classic release exists")
    selected = max(candidates, key=lambda candidate: candidate[0])[1]
    validate_latest(selected)
    release_id = selected.get("id")
    if not isinstance(release_id, int) or release_id <= 0:
        raise LatestTagError("selected release has no valid numeric ID")
    return selected


def write_output(path: Path, values: dict[str, str]) -> None:
    with path.open("a", encoding="utf-8") as stream:
        for key, value in values.items():
            stream.write(f"{key}={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True)
    parser.add_argument("--github-output", type=Path, required=True)
    arguments = parser.parse_args()
    result = subprocess.run(
        [
            "gh",
            "api",
            "--paginate",
            f"repos/{arguments.repository}/releases?per_page=100",
            "--jq",
            ".[]",
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        parser.exit(
            1,
            "latest-release audit failed: "
            + (result.stderr.strip() or result.stdout.strip())
            + "\n",
        )
    try:
        selected = select_latest(
            [json.loads(line) for line in result.stdout.splitlines() if line]
        )
        tag, release_version = validate_latest(selected)
        write_output(
            arguments.github_output,
            {
                "release_id": str(selected["id"]),
                "tag": tag,
                "version": release_version,
            },
        )
    except (OSError, ValueError, LatestTagError) as error:
        parser.exit(1, f"latest-release audit failed: {error}\n")
    print(f"validated highest immutable Classic release {tag}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
