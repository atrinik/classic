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

from github_release import GitHubReleaseError, invoke, list_releases, parse_json


ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "docs" / "history" / "release-tags.json"
TAG_RE = re.compile(r"v5\.[0-9]+\.[0-9]+")
SHA_RE = re.compile(r"[0-9a-f]{40}")
DIGEST_RE = re.compile(r"sha256:[0-9a-f]{64}")

PACKAGE_WORKFLOW_PATH = ".github/workflows/package-release.yml"
PACKAGE_RUN_NAME = "Package Release"
IMAGE_JOB = (
    "Build and validate immutable candidate / "
    "Build classic server image without publishing"
)
WINDOWS_JOB = (
    "Build and validate immutable candidate / Build Windows server package"
)
CANDIDATE_FINALIZER_JOB = (
    "Build and validate immutable candidate / Validate complete release candidate"
)
PUBLISH_JOB = "Publish unified release"
PUBLISH_STEP = "Publish the complete draft release"
IMAGE_INSPECTION_STEP = "Inspect the versioned server image"

# The resolver deliberately keeps GitHub API inspection finite. A release draft
# is old state, not permission to scan an unbounded Actions history on every
# successful main validation.
MAX_API_PAGES = 20
MAX_WORKFLOW_RUNS = 50
MAX_CANDIDATE_INSPECTIONS = 25
MAX_AUTOMATIC_SERVER_IMAGE_RETRIES = 3

RETAINED_PROOF_STEPS = (
    IMAGE_INSPECTION_STEP,
    "Recheck candidate hashes before publication",
    "Attest downloadable artifact provenance",
    "Attest downloadable artifact SBOM",
    "Resume draft assets or verify the immutable release",
    "Verify the exact image, labels, provenance, and SBOM",
    "Attest the exact published server image",
    "Verify the GitHub server-image attestation",
    "Recheck the exact release assets immediately before publication",
)


class PendingReleaseError(RuntimeError):
    """Raised when a pending release cannot be resolved safely."""


class CandidateNotSafe(PendingReleaseError):
    """Raised when one failed run is not a publishable retained candidate."""


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
    return [release for release in list_releases(repository) if release.get("draft") is True]


def _collect_jobs(
    repository: str,
    run_id: int,
    request: Callable[[str], object],
) -> list[dict[str, object]]:
    jobs: list[dict[str, object]] = []
    total_count: int | None = None
    for page in range(1, MAX_API_PAGES + 1):
        if total_count is not None and len(jobs) >= total_count:
            return jobs
        response = request(
            f"repos/{repository}/actions/runs/{run_id}/jobs"
            f"?filter=latest&per_page=100&page={page}"
        )
        if (
            not isinstance(response, dict)
            or not isinstance(response.get("total_count"), int)
            or response["total_count"] < 0
            or not isinstance(response.get("jobs"), list)
            or len(response["jobs"]) > 100
        ):
            raise PendingReleaseError(f"run {run_id} returned invalid jobs")
        if total_count is None:
            total_count = response["total_count"]
        elif response["total_count"] != total_count:
            raise PendingReleaseError(f"run {run_id} jobs changed during inspection")
        page_jobs = response["jobs"]
        if not page_jobs and len(jobs) < total_count:
            raise PendingReleaseError(f"run {run_id} returned incomplete jobs")
        for job in page_jobs:
            if not isinstance(job, dict):
                raise PendingReleaseError(f"run {run_id} returned invalid jobs")
            jobs.append(job)
        if len(jobs) > total_count:
            raise PendingReleaseError(f"run {run_id} returned invalid jobs")
    raise PendingReleaseError(f"run {run_id} has too many job pages")


def _collect_artifacts(
    repository: str,
    run_id: int,
    request: Callable[[str], object],
) -> list[dict[str, object]]:
    artifacts: list[dict[str, object]] = []
    total_count: int | None = None
    for page in range(1, MAX_API_PAGES + 1):
        if total_count is not None and len(artifacts) >= total_count:
            return artifacts
        response = request(
            f"repos/{repository}/actions/runs/{run_id}/artifacts"
            f"?per_page=100&page={page}"
        )
        if (
            not isinstance(response, dict)
            or not isinstance(response.get("total_count"), int)
            or response["total_count"] < 0
            or not isinstance(response.get("artifacts"), list)
            or len(response["artifacts"]) > 100
        ):
            raise PendingReleaseError(f"run {run_id} returned invalid artifacts")
        if total_count is None:
            total_count = response["total_count"]
        elif response["total_count"] != total_count:
            raise PendingReleaseError(
                f"run {run_id} artifacts changed during inspection"
            )
        page_artifacts = response["artifacts"]
        if not page_artifacts and len(artifacts) < total_count:
            raise PendingReleaseError(f"run {run_id} returned incomplete artifacts")
        for artifact in page_artifacts:
            if not isinstance(artifact, dict):
                raise PendingReleaseError(f"run {run_id} returned invalid artifacts")
            artifacts.append(artifact)
        if len(artifacts) > total_count:
            raise PendingReleaseError(f"run {run_id} returned invalid artifacts")
    raise PendingReleaseError(f"run {run_id} has too many artifact pages")


def _validate_package_run(
    repository: str, run_id: int, run: object
) -> dict[str, object]:
    if not isinstance(run, dict) or any(
        run.get(key) != value
        for key, value in {
            "name": PACKAGE_RUN_NAME,
            "path": PACKAGE_WORKFLOW_PATH,
            "event": "workflow_dispatch",
            "conclusion": "failure",
        }.items()
    ):
        raise PendingReleaseError(f"run {run_id} is not a failed package run")
    if "id" in run and run.get("id") != run_id:
        raise PendingReleaseError(f"run {run_id} returned the wrong identity")
    head_repository = run.get("head_repository")
    if not isinstance(head_repository, dict) or head_repository.get("full_name") != repository:
        raise PendingReleaseError(f"run {run_id} belongs to another repository")
    return run


def validate_failed_run(
    repository: str,
    run_id: int,
    windows_server_conclusion: str,
    server_image_conclusion: str,
    request: Callable[[str], object],
) -> None:
    run = request(f"repos/{repository}/actions/runs/{run_id}")
    _validate_package_run(repository, run_id, run)
    jobs = _collect_jobs(repository, run_id, request)
    required = (
        (WINDOWS_JOB, windows_server_conclusion),
        (IMAGE_JOB, server_image_conclusion),
        (CANDIDATE_FINALIZER_JOB, "skipped"),
        (PUBLISH_JOB, "skipped"),
    )
    for name, conclusion in required:
        matches = [job for job in jobs if job.get("name") == name]
        if len(matches) != 1 or matches[0].get("conclusion") != conclusion:
            raise PendingReleaseError(
                f"run {run_id} no longer matches the failed-candidate evidence"
            )


def _non_success_step_names(job: dict[str, object], run_id: int) -> list[str]:
    steps = job.get("steps")
    if not isinstance(steps, list) or not steps:
        raise CandidateNotSafe(f"run {run_id} has no inspectable publication steps")
    non_success: list[str] = []
    for step in steps:
        if not isinstance(step, dict):
            raise CandidateNotSafe(f"run {run_id} has invalid publication steps")
        name = step.get("name")
        conclusion = step.get("conclusion")
        if not isinstance(name, str) or not isinstance(conclusion, str):
            raise CandidateNotSafe(f"run {run_id} has an incomplete publication step")
        if conclusion not in {"success", "skipped"}:
            non_success.append(name)
    return non_success


def classify_failed_run(jobs: list[dict[str, object]]) -> str:
    """Return a stable failure class derived from the failed job boundary."""

    image_jobs = [job for job in jobs if job.get("name") == IMAGE_JOB]
    if any(job.get("conclusion") == "failure" for job in image_jobs):
        return "server-image-build"

    windows_jobs = [job for job in jobs if job.get("name") == WINDOWS_JOB]
    if any(job.get("conclusion") == "failure" for job in windows_jobs):
        return "windows-server-build"

    publish_jobs = [job for job in jobs if job.get("name") == PUBLISH_JOB]
    if any(job.get("conclusion") == "failure" for job in publish_jobs):
        for job in publish_jobs:
            if job.get("conclusion") != "failure":
                continue
            steps = job.get("steps")
            if isinstance(steps, list):
                failed_names = {
                    step.get("name")
                    for step in steps
                    if isinstance(step, dict) and step.get("conclusion") == "failure"
                }
                if IMAGE_INSPECTION_STEP in failed_names:
                    return "server-image-inspection"
                if PUBLISH_STEP in failed_names:
                    return "release-publication"
            return "release-publication"

    if any(
        job.get("name") == CANDIDATE_FINALIZER_JOB
        and job.get("conclusion") == "failure"
        for job in jobs
    ):
        return "candidate-validation"
    return "unknown"


def validate_retained_candidate(
    repository: str,
    tag: str,
    run_id: int,
    request: Callable[[str], object],
) -> dict[str, str]:
    """Prove that a failed run stopped only at the release publication boundary."""

    run = _validate_package_run(
        repository, run_id, request(f"repos/{repository}/actions/runs/{run_id}")
    )
    run_commit = run.get("head_sha")
    if not isinstance(run_commit, str) or SHA_RE.fullmatch(run_commit) is None:
        raise CandidateNotSafe(f"run {run_id} has no immutable source commit")

    jobs = _collect_jobs(repository, run_id, request)
    finalizer_jobs = [
        job for job in jobs if job.get("name") == CANDIDATE_FINALIZER_JOB
    ]
    publisher_jobs = [job for job in jobs if job.get("name") == PUBLISH_JOB]
    if (
        len(finalizer_jobs) != 1
        or finalizer_jobs[0].get("conclusion") != "success"
        or len(publisher_jobs) != 1
        or publisher_jobs[0].get("conclusion") != "failure"
    ):
        raise CandidateNotSafe(
            f"run {run_id} does not have one complete candidate and one failed publisher"
        )

    publisher = publisher_jobs[0]
    failed_steps = _non_success_step_names(publisher, run_id)
    if failed_steps != [PUBLISH_STEP]:
        raise CandidateNotSafe(
            f"run {run_id} failed outside the complete-release publication boundary"
        )
    steps = publisher["steps"]
    assert isinstance(steps, list)
    for required_name in RETAINED_PROOF_STEPS:
        matches = [
            step
            for step in steps
            if isinstance(step, dict) and step.get("name") == required_name
        ]
        if len(matches) != 1 or matches[0].get("conclusion") != "success":
            raise CandidateNotSafe(
                f"run {run_id} lacks successful retained-candidate proof: {required_name}"
            )

    expected_artifact = f"complete-release-candidate-{tag}"
    artifacts = _collect_artifacts(repository, run_id, request)
    matches = [artifact for artifact in artifacts if artifact.get("name") == expected_artifact]
    if len(matches) != 1:
        raise CandidateNotSafe(
            f"run {run_id} does not have exactly one complete candidate artifact"
        )
    artifact = matches[0]
    digest = artifact.get("digest")
    size = artifact.get("size_in_bytes")
    if (
        artifact.get("expired") is not False
        or isinstance(size, bool)
        or not isinstance(size, int)
        or size <= 0
        or not isinstance(digest, str)
        or DIGEST_RE.fullmatch(digest) is None
    ):
        raise CandidateNotSafe(f"run {run_id} has an invalid complete candidate artifact")

    return {
        "tag": tag,
        "candidate_run_id": str(run_id),
        "run_commit": run_commit,
        "artifact_digest": digest,
        "failure_class": "release-publication",
    }


def list_workflow_runs(
    repository: str,
    request: Callable[[str], object],
) -> list[dict[str, object]]:
    """Read a bounded, newest-first window of completed Package Release runs."""

    runs: list[dict[str, object]] = []
    total_count: int | None = None
    for page in range(1, MAX_API_PAGES + 1):
        if len(runs) >= MAX_WORKFLOW_RUNS:
            return runs[:MAX_WORKFLOW_RUNS]
        response = request(
            f"repos/{repository}/actions/workflows/package-release.yml/runs"
            f"?event=workflow_dispatch&status=completed&sort=created"
            f"&direction=desc&per_page=100&page={page}"
        )
        if (
            not isinstance(response, dict)
            or not isinstance(response.get("total_count"), int)
            or response["total_count"] < 0
            or not isinstance(response.get("workflow_runs"), list)
            or len(response["workflow_runs"]) > 100
        ):
            raise PendingReleaseError("Package Release returned invalid workflow runs")
        if total_count is None:
            total_count = response["total_count"]
        elif response["total_count"] != total_count:
            raise PendingReleaseError("Package Release runs changed during inspection")
        page_runs = response["workflow_runs"]
        if not page_runs and len(runs) < total_count:
            raise PendingReleaseError("Package Release returned incomplete workflow runs")
        for run in page_runs:
            if not isinstance(run, dict):
                raise PendingReleaseError("Package Release returned invalid workflow runs")
            runs.append(run)
        if len(runs) > total_count:
            raise PendingReleaseError("Package Release returned invalid workflow runs")
        if not page_runs:
            return runs
        if total_count == len(runs):
            return runs
    raise PendingReleaseError("Package Release workflow history exceeds inspection bound")


def find_retained_candidates(
    repository: str,
    tag: str,
    tag_commit: str,
    current_head: str,
    request: Callable[[str], object],
) -> list[dict[str, str]]:
    """Find exactly one safe retained candidate without rebuilding or guessing."""

    candidates: list[dict[str, str]] = []
    inspected = 0
    for listed_run in list_workflow_runs(repository, request):
        if (
            listed_run.get("name") != PACKAGE_RUN_NAME
            or listed_run.get("path") != PACKAGE_WORKFLOW_PATH
            or listed_run.get("event") != "workflow_dispatch"
            or listed_run.get("conclusion") != "failure"
        ):
            continue
        run_id = listed_run.get("id")
        if isinstance(run_id, bool) or not isinstance(run_id, int) or run_id <= 0:
            continue
        inspected += 1
        if inspected > MAX_CANDIDATE_INSPECTIONS:
            break
        try:
            candidate = validate_retained_candidate(repository, tag, run_id, request)
        except CandidateNotSafe:
            continue
        if not is_ancestor(tag_commit, candidate["run_commit"]):
            continue
        if not is_ancestor(candidate["run_commit"], current_head):
            continue
        candidates.append(candidate)
        if len(candidates) > 1:
            raise PendingReleaseError(
                f"draft {tag} has multiple verified retained candidates"
            )
    return candidates


def recent_failure_classes(
    repository: str,
    tag: str,
    tag_commit: str,
    current_head: str,
    request: Callable[[str], object],
) -> list[str]:
    """Classify the newest failed tag-bound runs for bounded retry policy."""

    classes: list[str] = []
    inspected = 0
    for listed_run in list_workflow_runs(repository, request):
        if (
            listed_run.get("name") != PACKAGE_RUN_NAME
            or listed_run.get("path") != PACKAGE_WORKFLOW_PATH
            or listed_run.get("event") != "workflow_dispatch"
            or listed_run.get("conclusion") != "failure"
        ):
            continue
        head_branch = listed_run.get("head_branch")
        listed_sha = listed_run.get("head_sha")
        if head_branch != tag and listed_sha != tag_commit:
            continue
        run_id = listed_run.get("id")
        if isinstance(run_id, bool) or not isinstance(run_id, int) or run_id <= 0:
            continue
        inspected += 1
        if inspected > MAX_AUTOMATIC_SERVER_IMAGE_RETRIES:
            break
        run = _validate_package_run(
            repository, run_id, request(f"repos/{repository}/actions/runs/{run_id}")
        )
        run_sha = run.get("head_sha")
        if (
            not isinstance(run_sha, str)
            or SHA_RE.fullmatch(run_sha) is None
            or not is_ancestor(tag_commit, run_sha)
            or not is_ancestor(run_sha, current_head)
        ):
            continue
        classes.append(classify_failed_run(_collect_jobs(repository, run_id, request)))
    return classes


def _leading_failure_class_count(classes: list[str], failure_class: str) -> int:
    count = 0
    for value in classes:
        if value != failure_class:
            break
        count += 1
    return count


def resolve(
    drafts: list[dict[str, object]],
    failed_releases: object,
    tag_commit: Callable[[str], str],
    validate_run: Callable[[int, str, str], None],
    retained_candidates: Callable[[str], list[dict[str, str]]] | None = None,
    failure_classes: Callable[[str], list[str]] | None = None,
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
        or release.get("draft") is not True
        or release.get("prerelease") is not False
        or not isinstance(assets, list)
    ):
        raise PendingReleaseError("draft release metadata is invalid")

    if not isinstance(failed_releases, dict):
        raise PendingReleaseError("failed-release policy is invalid")
    disposition = failed_releases.get(tag)
    if disposition is None:
        if assets:
            candidates = retained_candidates(tag) if retained_candidates is not None else []
            if len(candidates) > 1:
                raise PendingReleaseError(
                    f"draft {tag} has multiple verified retained candidates"
                )
            if len(candidates) == 1:
                candidate = candidates[0]
                if not isinstance(candidate, dict):
                    raise PendingReleaseError(
                        f"draft {tag}: retained candidate evidence is invalid"
                    )
                candidate_run_id = candidate.get("candidate_run_id")
                candidate_tag = candidate.get("tag")
                failure_class = candidate.get("failure_class", "release-publication")
                if (
                    candidate_tag not in (None, tag)
                    or isinstance(candidate_run_id, bool)
                    or not isinstance(candidate_run_id, str)
                    or re.fullmatch(r"[1-9][0-9]*", candidate_run_id) is None
                    or not isinstance(failure_class, str)
                    or re.fullmatch(r"[a-z0-9-]+", failure_class) is None
                ):
                    raise PendingReleaseError(
                        f"draft {tag}: retained candidate identity is invalid"
                    )
                return {
                    "action": "resume-retained-candidate",
                    "tag": tag,
                    "release_id": str(release_id),
                    "candidate_run_id": candidate_run_id,
                    "failure_class": failure_class,
                }
            raise PendingReleaseError(
                f"draft {tag} has assets; retained-candidate recovery is required "
                "but no safe retained-candidate run was found"
            )
        classes = failure_classes(tag) if failure_classes is not None else []
        if classes:
            failure_class = classes[0]
            retry_count = _leading_failure_class_count(classes, failure_class)
            if (
                failure_class == "server-image-build"
                and retry_count >= MAX_AUTOMATIC_SERVER_IMAGE_RETRIES
            ):
                return {
                    "action": "blocked",
                    "tag": tag,
                    "release_id": str(release_id),
                    "failure_class": failure_class,
                    "retry_count": str(retry_count),
                }
            return {
                "action": "resume",
                "tag": tag,
                "release_id": str(release_id),
                "failure_class": failure_class,
                "retry_count": str(retry_count),
            }
        return {"action": "resume", "tag": tag, "release_id": str(release_id)}

    if not isinstance(disposition, dict):
        raise PendingReleaseError(f"{tag}: failed-release disposition is invalid")
    commit = disposition.get("commit")
    expected_id = disposition.get("empty_draft_id")
    run_ids = disposition.get("failed_package_run_ids")
    windows_server_conclusion = disposition.get("windows_server_conclusion")
    server_image_conclusion = disposition.get("server_image_conclusion")
    if (
        disposition.get("disposition") != "delete-empty-draft"
        or not isinstance(commit, str)
        or SHA_RE.fullmatch(commit) is None
        or not isinstance(expected_id, int)
        or not isinstance(run_ids, list)
        or not run_ids
        or not all(isinstance(run_id, int) and run_id > 0 for run_id in run_ids)
        or windows_server_conclusion not in {"success", "failure"}
        or server_image_conclusion not in {"success", "failure"}
    ):
        raise PendingReleaseError(f"{tag}: failed-release disposition is invalid")
    if release_id != expected_id or assets or tag_commit(tag) != commit:
        raise PendingReleaseError(f"{tag}: empty failed draft no longer matches policy")
    for run_id in run_ids:
        validate_run(run_id, windows_server_conclusion, server_image_conclusion)
    return {"action": "delete-empty-draft", "tag": tag, "release_id": str(release_id)}


def write_output(path: Path, values: dict[str, str]) -> None:
    with path.open("a", encoding="utf-8") as stream:
        for key, value in values.items():
            if "\n" in value or "\r" in value:
                raise PendingReleaseError(f"invalid multiline output: {key}")
            stream.write(f"{key}={value}\n")


def api(path: str) -> object:
    return parse_json(invoke(["gh", "api", path]), f"cannot inspect {path}")


def delete_policy_listed_empty_draft(
    repository: str,
    expected_tag: str,
    expected_release_id: int,
    resolve_now: Callable[[], dict[str, str]],
    request: Callable[[str], object],
    delete: Callable[[int], None],
) -> None:
    expected = {
        "action": "delete-empty-draft",
        "tag": expected_tag,
        "release_id": str(expected_release_id),
    }
    if resolve_now() != expected:
        raise PendingReleaseError("failed draft changed before deletion")

    release = request(f"repos/{repository}/releases/{expected_release_id}")
    if (
        not isinstance(release, dict)
        or release.get("id") != expected_release_id
        or release.get("tag_name") != expected_tag
        or release.get("draft") is not True
        or release.get("prerelease") is not False
        or release.get("assets") != []
    ):
        raise PendingReleaseError("failed draft changed before deletion")
    delete(expected_release_id)


def resolve_repository(repository: str) -> dict[str, str]:
    current_head = command("git", "rev-parse", "HEAD")
    if current_head != command("git", "rev-parse", "refs/remotes/origin/main"):
        raise PendingReleaseError("pending-release resolution is not current origin/main")
    policy = json.loads(POLICY.read_text(encoding="utf-8"))

    def tag_commit(tag: str) -> str:
        return command("git", "rev-parse", f"refs/tags/{tag}^{{commit}}")

    values = resolve(
        list_drafts(repository),
        policy.get("failed_releases"),
        tag_commit,
        lambda run_id, windows_conclusion, image_conclusion: validate_failed_run(
            repository, run_id, windows_conclusion, image_conclusion, api
        ),
        lambda tag: find_retained_candidates(
            repository, tag, tag_commit(tag), current_head, api
        ),
        lambda tag: recent_failure_classes(
            repository, tag, tag_commit(tag), current_head, api
        ),
    )
    if values["tag"] and not is_ancestor(
        f"refs/tags/{values['tag']}^{{commit}}", current_head
    ):
        raise PendingReleaseError(f"{values['tag']}: tag is not an ancestor of current main")
    return values


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True)
    parser.add_argument("--github-output", type=Path)
    parser.add_argument("--delete-policy-listed-empty-draft", action="store_true")
    parser.add_argument("--expected-tag")
    parser.add_argument("--expected-release-id", type=int)
    arguments = parser.parse_args()

    if arguments.delete_policy_listed_empty_draft:
        if (
            arguments.github_output is not None
            or arguments.expected_tag is None
            or TAG_RE.fullmatch(arguments.expected_tag) is None
            or arguments.expected_release_id is None
            or arguments.expected_release_id <= 0
        ):
            parser.error("guarded deletion requires an exact tag and positive release ID")
        delete_policy_listed_empty_draft(
            arguments.repository,
            arguments.expected_tag,
            arguments.expected_release_id,
            lambda: resolve_repository(arguments.repository),
            api,
            lambda release_id: command(
                "gh",
                "api",
                "--method",
                "DELETE",
                f"repos/{arguments.repository}/releases/{release_id}",
            ),
        )
        return 0
    if arguments.expected_tag is not None or arguments.expected_release_id is not None:
        parser.error("expected deletion coordinates require guarded deletion")

    values = resolve_repository(arguments.repository)

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
