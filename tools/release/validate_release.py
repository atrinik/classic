#!/usr/bin/env python3
"""Validate a semantic-release tag before any classic artifact is published."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "docs" / "history" / "release-tags.json"
TAG_RE = re.compile(r"v([0-9]+)\.([0-9]+)\.([0-9]+)")


class ValidationError(RuntimeError):
    """Raised when a release is not safe to publish."""


def command(*arguments: str, json_output: bool = False) -> str | object:
    result = subprocess.run(
        list(arguments),
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ValidationError(f"{' '.join(arguments)}: {detail}")
    output = result.stdout.strip()
    return json.loads(output) if json_output else output


def git(*arguments: str) -> str:
    return str(command("git", *arguments))


def version(tag: str) -> tuple[int, int, int]:
    match = TAG_RE.fullmatch(tag)
    if not match:
        raise ValidationError("release tag must be unprefixed vMAJOR.MINOR.PATCH")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def is_ancestor(older: str, newer: str) -> bool:
    result = subprocess.run(
        ["git", "merge-base", "--is-ancestor", older, newer],
        cwd=ROOT,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def write_output(path: Path, values: dict[str, str]) -> None:
    with path.open("a", encoding="utf-8") as stream:
        for key, value in values.items():
            if "\n" in value or "\r" in value:
                raise ValidationError(f"invalid multiline output: {key}")
            stream.write(f"{key}={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--github-output", type=Path)
    arguments = parser.parse_args()

    release_version = version(arguments.tag)
    policy = json.loads(POLICY.read_text(encoding="utf-8"))
    future = policy.get("future_tags", {})
    minimum = future.get("minimum_version")
    floor = future.get("ancestry_floor")
    if not isinstance(minimum, str) or not isinstance(floor, str):
        raise ValidationError("release-tag policy has no future-tag contract")
    if release_version < version(minimum):
        raise ValidationError(f"new unified releases begin at {minimum}")

    commit = git("rev-parse", f"{arguments.tag}^{{commit}}")
    if git("rev-parse", "HEAD") != commit:
        raise ValidationError("checked-out tag does not resolve to HEAD")
    if not is_ancestor(floor, commit):
        raise ValidationError("release tag predates the unified release floor")
    if not is_ancestor(commit, "refs/remotes/origin/main"):
        raise ValidationError("release commit is not an ancestor of origin/main")

    subprocess.run(
        ["python3", "tools/verify_import_history.py"], cwd=ROOT, check=True
    )
    release = command(
        "gh",
        "api",
        f"repos/{arguments.repository}/releases/tags/{arguments.tag}",
        json_output=True,
    )
    assert isinstance(release, dict)
    if release.get("draft") or release.get("prerelease"):
        raise ValidationError("release must be published and non-prerelease")
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise ValidationError("GitHub release returned invalid asset metadata")
    if assets:
        raise ValidationError("release already has assets; refusing duplicate publication")

    checks = command(
        "gh",
        "api",
        "-H",
        "Accept: application/vnd.github+json",
        f"repos/{arguments.repository}/commits/{commit}/check-runs?per_page=100",
        json_output=True,
    )
    assert isinstance(checks, dict)
    runs = checks.get("check_runs")
    if not isinstance(runs, list) or not any(
        run.get("name") == "Classic validation" and run.get("conclusion") == "success"
        for run in runs
        if isinstance(run, dict)
    ):
        raise ValidationError("release commit has no successful Classic validation check")

    active_tags = [line for line in git("tag", "--list", "v[0-9]*").splitlines() if line]
    latest = max(active_tags, key=version)
    values = {
        "tag": arguments.tag,
        "version": ".".join(str(part) for part in release_version),
        "commit": commit,
        "source_epoch": git("show", "-s", "--format=%ct", commit),
        "latest": str(arguments.tag == latest).lower(),
    }
    output = arguments.github_output
    if output is None and os.environ.get("GITHUB_OUTPUT"):
        output = Path(os.environ["GITHUB_OUTPUT"])
    if output is not None:
        write_output(output, values)
    else:
        print(json.dumps(values, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValidationError as error:
        raise SystemExit(f"release validation: {error}") from error
