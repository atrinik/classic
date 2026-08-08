"""Read GitHub release state without hiding authenticated draft releases."""

from __future__ import annotations

import json
import subprocess


class GitHubReleaseError(RuntimeError):
    """Raised when release state cannot be established conclusively."""


def invoke(arguments: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def parse_json(result: subprocess.CompletedProcess[str], context: str) -> object:
    if result.returncode:
        raise GitHubReleaseError(
            f"{context}: {result.stderr.strip() or result.stdout.strip()}"
        )
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise GitHubReleaseError(f"{context}: invalid JSON response") from error


def find_unique_release(releases: list[object], tag: str) -> dict[str, object] | None:
    matches = [
        release
        for release in releases
        if isinstance(release, dict) and release.get("tag_name") == tag
    ]
    if len(matches) > 1:
        raise GitHubReleaseError(f"multiple GitHub releases use tag {tag}")
    return matches[0] if matches else None


def lookup_release(repository: str, tag: str) -> dict[str, object] | None:
    view = invoke(
        [
            "gh",
            "release",
            "view",
            tag,
            "--repo",
            repository,
            "--json",
            "apiUrl",
        ]
    )
    if view.returncode == 0:
        value = parse_json(view, "cannot view GitHub release")
        if not isinstance(value, dict) or not isinstance(value.get("apiUrl"), str):
            raise GitHubReleaseError("GitHub release view has no API URL")
        release = parse_json(
            invoke(["gh", "api", value["apiUrl"]]),
            "cannot inspect GitHub release",
        )
        if not isinstance(release, dict) or release.get("tag_name") != tag:
            raise GitHubReleaseError("GitHub release API returned the wrong tag")
        return release

    detail = f"{view.stdout}\n{view.stderr}".lower()
    if "release not found" not in detail:
        raise GitHubReleaseError(
            "cannot determine GitHub release state: "
            + (view.stderr.strip() or view.stdout.strip())
        )
    pages = parse_json(
        invoke(
            [
                "gh",
                "api",
                "--paginate",
                "--slurp",
                f"repos/{repository}/releases?per_page=100",
            ]
        ),
        "cannot prove GitHub release absence",
    )
    if not isinstance(pages, list) or not all(isinstance(page, list) for page in pages):
        raise GitHubReleaseError("GitHub releases API returned invalid pages")
    return find_unique_release(
        [release for page in pages for release in page], tag
    )
