#!/usr/bin/env python3
"""Validate the exact tag-exists/release-missing recovery state."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess

from github_release import GitHubReleaseError, lookup_release


ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "docs" / "history" / "release-tags.json"
TAG_RE = re.compile(r"v([0-9]+)\.([0-9]+)\.([0-9]+)")


class RecoveryError(RuntimeError):
    """Raised when an existing tag is not safe to recover."""


def run(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(arguments),
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def command(*arguments: str) -> str:
    result = run(*arguments)
    if result.returncode:
        raise RecoveryError(
            f"{' '.join(arguments)}: {result.stderr.strip() or result.stdout.strip()}"
        )
    return result.stdout.strip()


def semantic_version(tag: str) -> tuple[int, int, int]:
    match = TAG_RE.fullmatch(tag)
    if match is None:
        raise RecoveryError("recovery tag must be unprefixed vMAJOR.MINOR.PATCH")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def select_previous_release_tag(current_tag: str, tags: list[str]) -> str:
    current = semantic_version(current_tag)
    candidates = []
    for tag in tags:
        try:
            candidate = semantic_version(tag)
        except RecoveryError:
            continue
        if candidate[0] == current[0] and candidate < current:
            candidates.append((candidate, tag))
    if not candidates:
        raise RecoveryError("recovery tag has no earlier reachable unified release")
    return max(candidates)[1]


def previous_release_tag(current_tag: str, tag_commit: str) -> str:
    tags = command(
        "git", "tag", "--merged", tag_commit, "--list", "v[0-9]*.[0-9]*.[0-9]*"
    ).splitlines()
    return select_previous_release_tag(current_tag, tags)


def write_github_output(path: Path, previous_tag: str, tag_commit: str) -> None:
    with path.open("a", encoding="utf-8") as output:
        output.write(f"previous_tag={previous_tag}\n")
        output.write(f"tag_commit={tag_commit}\n")


def verify_version_policy(tag: str, policy_path: Path = POLICY) -> None:
    proposed = semantic_version(tag)
    policy = json.loads(policy_path.read_text(encoding="utf-8"))
    future = policy.get("future_tags", {})
    minimum = future.get("minimum_version")
    maximum_major = future.get("maximum_major")
    if not isinstance(minimum, str) or not isinstance(maximum_major, int):
        raise RecoveryError("release-tag policy has no future-tag contract")
    if proposed < semantic_version(minimum):
        raise RecoveryError(f"unified releases begin at {minimum}")
    if proposed[0] != maximum_major:
        raise RecoveryError(
            f"classic releases must remain on major version {maximum_major}"
        )


def has_successful_classic_check(payload: object) -> bool:
    if not isinstance(payload, dict) or not isinstance(payload.get("check_runs"), list):
        return False
    return any(
        isinstance(item, dict)
        and item.get("name") == "Classic validation"
        and item.get("conclusion") == "success"
        and isinstance(item.get("app"), dict)
        and item["app"].get("id") == 15368
        for item in payload["check_runs"]
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--github-output", type=Path)
    arguments = parser.parse_args()

    try:
        verify_version_policy(arguments.tag)
        tag_commit = command("git", "rev-parse", f"{arguments.tag}^{{commit}}")
        main_commit = command("git", "rev-parse", "refs/remotes/origin/main")
        if command("git", "rev-parse", "HEAD") != main_commit:
            raise RecoveryError("recovery must run from the current main commit")
        first_parent = set(
            command("git", "rev-list", "--first-parent", main_commit).splitlines()
        )
        if tag_commit not in first_parent:
            raise RecoveryError("release tag is not on main's first-parent line")
        subprocess.run(
            ["python3", "tools/verify_import_history.py"], cwd=ROOT, check=True
        )

        if lookup_release(arguments.repository, arguments.tag) is not None:
            raise RecoveryError(
                "GitHub release already exists; use Package Release recovery"
            )
        checks = json.loads(
            command(
                "gh",
                "api",
                "-H",
                "Accept: application/vnd.github+json",
                f"repos/{arguments.repository}/commits/{tag_commit}/check-runs?per_page=100",
            )
        )
        if not has_successful_classic_check(checks):
            raise RecoveryError("tag commit has no successful Classic validation")
        previous_tag = previous_release_tag(arguments.tag, tag_commit)
        if arguments.github_output is not None:
            write_github_output(arguments.github_output, previous_tag, tag_commit)
    except (
        GitHubReleaseError,
        OSError,
        ValueError,
        RecoveryError,
        subprocess.CalledProcessError,
    ) as error:
        parser.exit(1, f"release recovery validation failed: {error}\n")

    print(
        f"validated missing GitHub release recovery for {arguments.tag} "
        f"after {previous_tag}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
