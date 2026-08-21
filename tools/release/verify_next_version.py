#!/usr/bin/env python3
"""Reject a semantic-release result outside the immutable classic 5.x line."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "docs" / "history" / "release-tags.json"
VERSION_RE = re.compile(r"([0-9]+)\.([0-9]+)\.([0-9]+)")
MAINTENANCE_BRANCH_RE = re.compile(r"^([0-9]+)\.([0-9]+)\.x$")


class VersionPolicyError(RuntimeError):
    """Raised when semantic-release proposes a forbidden classic version."""


def semantic_version(value: str) -> tuple[int, int, int]:
    match = VERSION_RE.fullmatch(value)
    if match is None:
        raise VersionPolicyError("next version must be MAJOR.MINOR.PATCH")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def release_branch(value: str | None = None) -> tuple[str, tuple[int, int] | None]:
    branch = value or os.environ.get("ATRINIK_RELEASE_BRANCH") or "main"
    if branch == "main":
        return branch, None
    match = MAINTENANCE_BRANCH_RE.fullmatch(branch)
    if match is None:
        raise VersionPolicyError("release branch must be main or MAJOR.MINOR.x")
    return branch, (int(match.group(1)), int(match.group(2)))


def local_tags() -> set[str]:
    result = subprocess.run(
        ["git", "-C", str(ROOT), "tag", "--list"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        raise VersionPolicyError(
            "cannot inspect existing tags: "
            + (result.stderr.strip() or result.stdout.strip())
        )
    return set(result.stdout.splitlines())


def verify_next_version(
    value: str,
    policy_path: Path = POLICY,
    active_tags: set[str] | None = None,
    branch: str | None = None,
) -> None:
    proposed = semantic_version(value)
    policy = json.loads(policy_path.read_text(encoding="utf-8"))
    future = policy.get("future_tags", {})
    first_value = future.get("first_version")
    minimum_value = future.get("minimum_version")
    maximum_major = future.get("maximum_major")
    historical = policy.get("tags")
    if (
        not isinstance(first_value, str)
        or not first_value.startswith("v")
        or not isinstance(minimum_value, str)
        or not minimum_value.startswith("v")
    ):
        raise VersionPolicyError("release-tag policy has no first/minimum version")
    if not isinstance(maximum_major, int):
        raise VersionPolicyError("release-tag policy has no maximum major")
    if not isinstance(historical, dict) or not all(
        isinstance(tag, str) for tag in historical
    ):
        raise VersionPolicyError("release-tag policy has no historical tag set")
    first = semantic_version(first_value.removeprefix("v"))
    minimum = semantic_version(minimum_value.removeprefix("v"))
    if proposed < minimum:
        raise VersionPolicyError(
            f"unified classic releases begin at {minimum_value}"
        )
    if proposed[0] != maximum_major:
        raise VersionPolicyError(
            f"classic releases must remain on major version {maximum_major}"
        )

    present = local_tags() if active_tags is None else active_tags
    missing_historical = set(historical) - present
    if missing_historical:
        raise VersionPolicyError(
            "historical release tags are missing: "
            + ", ".join(sorted(missing_historical))
        )
    future_tags = present - set(historical)
    try:
        future_versions = sorted(
            (semantic_version(tag.removeprefix("v")) for tag in future_tags)
        )
    except VersionPolicyError as error:
        raise VersionPolicyError("an existing future tag is not semantic") from error
    _, maintenance = release_branch(branch)
    if maintenance is not None:
        if proposed[:2] != maintenance:
            raise VersionPolicyError(
                "maintenance release must remain in its MAJOR.MINOR.x range"
            )
        if proposed[2] == 0:
            raise VersionPolicyError("maintenance releases must use a non-zero patch")
        baseline = f"v{maintenance[0]}.{maintenance[1]}.0"
        if baseline not in present:
            raise VersionPolicyError(f"maintenance baseline tag is missing: {baseline}")
        line_versions = [
            maintenance + (0,),
            *[version for version in future_versions if version[:2] == maintenance],
        ]
        if proposed <= max(line_versions):
            raise VersionPolicyError(
                "next maintenance version must exceed every existing patch"
            )
        return
    if proposed[2] != 0:
        raise VersionPolicyError(
            "mainline releases must begin a new minor line with patch zero"
        )
    if future_versions:
        if future_versions[0] != first:
            raise VersionPolicyError(f"the first unified tag must be {first_value}")
        if proposed <= future_versions[-1]:
            raise VersionPolicyError("next version must exceed every existing unified tag")
    elif proposed != first:
        raise VersionPolicyError(f"the first unified tag must be {first_value}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    parser.add_argument("--branch")
    arguments = parser.parse_args()
    try:
        verify_next_version(arguments.version, branch=arguments.branch)
    except (OSError, ValueError, VersionPolicyError) as error:
        parser.exit(1, f"next-version verification failed: {error}\n")
    print(f"verified classic release version {arguments.version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
