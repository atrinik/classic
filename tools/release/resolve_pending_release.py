#!/usr/bin/env python3
"""Resolve one pending draft without obscuring an explicitly failed release."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from typing import Callable

from github_release import GitHubReleaseError, invoke, parse_json


ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "docs" / "history" / "release-tags.json"
TAG_RE = re.compile(r"v5\.[0-9]+\.[0-9]+")


class PendingReleaseError(RuntimeError):
    """Raised when a pending release cannot be resolved safely."""


def command(*arguments: str) -> str:
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
        raise PendingReleaseError(f"{' '.join(arguments)}: {detail}")
    return result.stdout.strip()


def is_ancestor(older: str, newer: str) -> bool:
    return (
        subprocess.run(
            ["git", "merge-base", "--is-ancestor", older, newer],
            cwd=ROOT,
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0
    )


def list_drafts(repository: str) -> list[dict[str, object]]:
    result = invoke(
        [
            "gh",
            "api",
            "--paginate",
            f"repos/{repository}/releases?per_page=100",
            "--jq",
            ".[] | @json",
        ]
    )
    if result.returncode:
        raise PendingReleaseError(
            "cannot inspect GitHub releases: "
            + (result.stderr.strip() or result.stdout.strip())
        )
    try:
        releases = [json.loads(line) for line in result.stdout.splitlines() if line]
    except json.JSONDecodeError as error:
        raise PendingReleaseError("GitHub releases API returned invalid JSON") from error
    if not all(isinstance(release, dict) for release in releases):
        raise PendingReleaseError("GitHub releases API returned invalid releases")
    return [release for release in releases if release.get("draft") is True]


def validate_failed_run(
    repository: str,
    run_id: int,
    request: Callable[[str], object],
) -> None:
    run = request(f"repos/{repository}/actions/runs/{run_id}")
    if not isinstance(run, dict) or any(
        run.get(key) != value
        for key, value in {
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
        }.items()
    ):
        raise PendingReleaseError(f"run {run_id} is not the recorded failed package run")
    head_repository = run.get("head_repository")
    if not isinstance(head_repository, dict) or head_repository.get("full_name") != repository:
        raise PendingReleaseError(f"run {run_id} belongs to another repository")

    jobs = request(f"repos/{repository}/actions/runs/{run_id}/jobs?filter=latest&per_page=100")
    if not isinstance(jobs, dict) or not isinstance(jobs.get("jobs"), list):
        raise PendingReleaseError(f"run {run_id} returned invalid jobs")
    conclusions = {
        job.get("name"): job.get("conclusion")
        for job in jobs["jobs"]
        if isinstance(job, dict)
    }
    required = {
        "Build and validate immutable candidate / Build Windows server package": "failure",
        "Build and validate immutable candidate / Build classic server image without publishing": "failure",
        "Build and validate immutable candidate / Validate complete release candidate": "skipped",
        "Publish unified release": "skipped",
    }
    if any(conclusions.get(name) != conclusion for name, conclusion in required.items()):
        raise PendingReleaseError(f"run {run_id} no longer matches the failed-candidate evidence")


def resolve(
    drafts: list[dict[str, object]],
    failed_releases: object,
    tag_commit: Callable[[str], str],
    validate_run: Callable[[int], None],
) -> dict[str, str]:
    if len(drafts) > 1:
        raise PendingReleaseError("multiple draft releases require manual investigation")
    if not drafts:
        return {"action": "none", "tag": "", "release_id": ""}

    release = drafts[0]
    tag = release.get("tag_name")
    release_id = release.get("id")
    assets = release.get("assets")
    if (
        not isinstance(tag, str)
        or TAG_RE.fullmatch(tag) is None
        or not isinstance(release_id, int)
        or release_id <= 0
        or release.get("prerelease") is not False
        or not isinstance(assets, list)
    ):
        raise PendingReleaseError("draft release metadata is invalid")

    if not isinstance(failed_releases, dict):
        raise PendingReleaseError("failed-release policy is invalid")
    disposition = failed_releases.get(tag)
    if disposition is None:
        if assets:
            raise PendingReleaseError(
                f"draft {tag} has assets; retained-candidate recovery is required"
            )
        return {"action": "resume", "tag": tag, "release_id": str(release_id)}

    if not isinstance(disposition, dict):
        raise PendingReleaseError(f"{tag}: failed-release disposition is invalid")
    commit = disposition.get("commit")
    expected_id = disposition.get("empty_draft_id")
    run_ids = disposition.get("failed_package_run_ids")
    if (
        disposition.get("disposition") != "delete-empty-draft"
        or not isinstance(commit, str)
        or re.fullmatch(r"[0-9a-f]{40}", commit) is None
        or not isinstance(expected_id, int)
        or not isinstance(run_ids, list)
        or not run_ids
        or not all(isinstance(run_id, int) and run_id > 0 for run_id in run_ids)
    ):
        raise PendingReleaseError(f"{tag}: failed-release disposition is invalid")
    if release_id != expected_id or assets or tag_commit(tag) != commit:
        raise PendingReleaseError(f"{tag}: empty failed draft no longer matches policy")
    for run_id in run_ids:
        validate_run(run_id)
    return {"action": "delete-empty-draft", "tag": tag, "release_id": str(release_id)}


def write_output(path: Path, values: dict[str, str]) -> None:
    with path.open("a", encoding="utf-8") as stream:
        for key, value in values.items():
            if "\n" in value or "\r" in value:
                raise PendingReleaseError(f"invalid multiline output: {key}")
            stream.write(f"{key}={value}\n")


def api(path: str) -> object:
    return parse_json(invoke(["gh", "api", path]), f"cannot inspect {path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True)
    parser.add_argument("--github-output", type=Path)
    arguments = parser.parse_args()

    if command("git", "rev-parse", "HEAD") != command(
        "git", "rev-parse", "refs/remotes/origin/main"
    ):
        raise PendingReleaseError("pending-release resolution is not current origin/main")
    policy = json.loads(POLICY.read_text(encoding="utf-8"))
    values = resolve(
        list_drafts(arguments.repository),
        policy.get("failed_releases"),
        lambda tag: command("git", "rev-parse", f"refs/tags/{tag}^{{commit}}"),
        lambda run_id: validate_failed_run(arguments.repository, run_id, api),
    )
    if values["tag"] and not is_ancestor(f"refs/tags/{values['tag']}^{{commit}}", "HEAD"):
        raise PendingReleaseError(f"{values['tag']}: tag is not an ancestor of current main")

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
    except (GitHubReleaseError, OSError, PendingReleaseError, ValueError) as error:
        raise SystemExit(f"pending release resolution: {error}") from error
