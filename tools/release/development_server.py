#!/usr/bin/env python3
"""Publish only checked main source identities to the Classic development channel.

GitHub releases and stable image coordinates are deliberately outside this tool.
Registry reads fail closed; only the final promote command changes an alias.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

import check_registry_version as registry
import locked_inputs

REPOSITORY = "atrinik/classic"
IMAGE = "ghcr.io/atrinik/classic-server"
WORKFLOW = ".github/workflows/publish-development-server.yml"
SOURCE_REF = "refs/heads/main"
CHANNEL_LABEL = "org.atrinik.release-channel"
REVISION = re.compile(r"[0-9a-f]{40}")
DIGEST = re.compile(r"sha256:[0-9a-f]{64}")


def need(condition: object, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def command(*args: str) -> str:
    result = subprocess.run(args, check=False, capture_output=True, text=True, timeout=300)
    need(result.returncode == 0, f"development publication command failed: {args[0]}")
    need(len(result.stdout) <= 8 * 1024 * 1024, "oversize publication response")
    return result.stdout.strip()


def api(path: str) -> object:
    return json.loads(command("gh", "api", f"repos/{REPOSITORY}/{path}"))


def context(revision: str) -> None:
    need(REVISION.fullmatch(revision), "invalid source revision")
    expected = {"GITHUB_REPOSITORY": REPOSITORY, "GITHUB_REF": SOURCE_REF,
                "GITHUB_SHA": revision,
                "GITHUB_WORKFLOW_REF": f"{REPOSITORY}/{WORKFLOW}@{SOURCE_REF}"}
    need(all(os.environ.get(key) == value for key, value in expected.items()),
         "development publication requires the canonical main workflow")
    need(os.environ.get("GITHUB_EVENT_NAME") in ("push", "workflow_dispatch"),
         "unsupported development publication event")
    need(command("git", "rev-parse", "HEAD") == revision, "checkout revision mismatch")
    need(not command("git", "status", "--porcelain", "--untracked-files=normal"),
         "tracked publication source is dirty")


def current_main(revision: str) -> None:
    ref = api("git/ref/heads/main")
    need(isinstance(ref, dict) and ref.get("ref") == SOURCE_REF and
         ref.get("object", {}).get("type") == "commit" and
         ref["object"].get("sha") == revision, "source is no longer current main")


def checked(revision: str) -> bool:
    """Bind the aggregate to a main-push Check run, not a namesake PR check."""
    data = api(f"actions/workflows/check.yml/runs?event=push&head_sha={revision}&per_page=100")
    need(isinstance(data, dict) and isinstance(data.get("workflow_runs"), list),
         "invalid Check workflow inventory")
    runs = [run for run in data["workflow_runs"] if
            run.get("head_sha") == revision and run.get("head_branch") == "main" and
            run.get("event") == "push" and run.get("path") == ".github/workflows/check.yml" and
            run.get("repository", {}).get("full_name") == REPOSITORY and
            run.get("head_repository", {}).get("full_name") == REPOSITORY]
    if not runs:
        return False
    need(all(type(run.get(field)) is int and run[field] > 0
             for run in runs for field in ("id", "check_suite_id")),
         "invalid Check run or suite identity")
    run = max(runs, key=lambda item: item["id"])
    if run.get("status") != "completed":
        return False
    need(run.get("conclusion") == "success", "main Check workflow failed")
    checks = api(f"commits/{revision}/check-runs?filter=latest&per_page=100")
    need(isinstance(checks, dict) and isinstance(checks.get("check_runs"), list),
         "invalid aggregate checks")
    matches = [check for check in checks["check_runs"] if
               check.get("name") == "Classic validation" and
               check.get("app", {}).get("id") == 15368 and
               check.get("head_sha") == revision and
               type(check.get("check_suite", {}).get("id")) is int and
               check["check_suite"]["id"] == run["check_suite_id"]]
    need(len(matches) == 1 and matches[0].get("status") == "completed" and
         matches[0].get("conclusion") == "success", "successful trusted aggregate is missing")
    return True


def preflight(revision: str, wait: bool = False) -> None:
    context(revision)
    for attempt in range(150 if wait else 1):
        current_main(revision)
        if checked(revision):
            return
        need(wait and attempt < 149, "main validation is not complete")
        time.sleep(30)


def find_tag(tag: str) -> tuple[bool, str | None]:
    need(tag == "development" or re.fullmatch(r"source-[0-9a-f]{40}", tag),
         "invalid development image tag")
    # A transport/authentication failure never proves that a tag is absent.
    for page in range(1, 101):
        code, data, _ = registry.request(
            "orgs/atrinik/packages/container/classic-server/versions"
            f"?per_page=100&page={page}")
        if code:
            if page == 1 and isinstance(data, dict) and str(data.get("status")) == "404" and data.get("message") == "Package not found.":
                return False, None
            raise RuntimeError("cannot audit development package")
        digest = registry.find_version(data, tag)
        if digest is not None:
            return True, digest
        if len(data) < 100:
            return True, None
    raise RuntimeError("development package inventory exceeded its bounded window")


def audit(revision: str) -> tuple[bool, str | None]:
    preflight(revision)
    exists, digest = find_tag("source-" + revision)
    need(exists, "server package missing; exact-digest recovery required")
    return exists, digest


def labels(digest: str) -> dict[str, str]:
    need(DIGEST.fullmatch(digest), "invalid index digest")
    ref = IMAGE + "@" + digest
    command("docker", "pull", "--platform", "linux/amd64", ref)
    objects = json.loads(command("docker", "image", "inspect", ref))
    need(isinstance(objects, list) and len(objects) == 1, "invalid development image")
    obj = objects[0]
    need(obj.get("Os") == "linux" and obj.get("Architecture") == "amd64" and
         ref in obj.get("RepoDigests", []), "development platform or digest mismatch")
    result = obj.get("Config", {}).get("Labels")
    need(isinstance(result, dict), "missing development labels")
    need(result.get("org.opencontainers.image.source") == "https://github.com/" + REPOSITORY and
         result.get("org.opencontainers.image.version") == "0.0.0" and
         result.get(CHANNEL_LABEL) == "development", "development source/channel/version mismatch")
    need(REVISION.fullmatch(result.get("org.opencontainers.image.revision", "")),
         "invalid development image source revision")
    return result


def attest(digest: str, revision: str) -> None:
    command("gh", "attestation", "verify", "oci://" + IMAGE + "@" + digest,
            "--repo", REPOSITORY, "--signer-workflow", REPOSITORY + "/" + WORKFLOW,
            "--source-ref", SOURCE_REF, "--source-digest", revision)


def verify(revision: str, digest: str, require_attestation: bool = True) -> None:
    need(REVISION.fullmatch(revision) and DIGEST.fullmatch(digest), "invalid immutable identity")
    _, actual = find_tag("source-" + revision)
    need(actual == digest, "immutable source tag does not match the built digest")
    actual_labels = labels(digest)
    need(actual_labels["org.opencontainers.image.revision"] == revision,
         "development source revision mismatch")
    inputs = locked_inputs.load_locked_inputs("0.0.0", Path.cwd())
    locked_inputs.verify_image(IMAGE, inputs, "server", "0.0.0", revision, digest)
    if require_attestation:
        attest(digest, revision)


def promotion_allowed(previous_revision: str, revision: str) -> None:
    need(REVISION.fullmatch(previous_revision) and REVISION.fullmatch(revision),
         "invalid promotion ancestry")
    if previous_revision == revision:
        return
    comparison = api(f"compare/{previous_revision}...{revision}")
    need(isinstance(comparison, dict) and comparison.get("status") == "ahead" and
         comparison.get("merge_base_commit", {}).get("sha") == previous_revision,
         "development alias cannot move backwards or to a divergent source")


def promote(revision: str, digest: str) -> None:
    preflight(revision)
    verify(revision, digest)
    package_exists, previous_digest = find_tag("development")
    need(package_exists, "development package disappeared")
    if previous_digest is not None:
        previous_revision = labels(previous_digest)["org.opencontainers.image.revision"]
        attest(previous_digest, previous_revision)
        _, immutable = find_tag("source-" + previous_revision)
        need(immutable == previous_digest, "previous development source tag changed")
        if previous_revision == revision:
            need(previous_digest == digest, "same source was rebuilt at another digest")
        promotion_allowed(previous_revision, revision)
    # All writers use one non-cancelling workflow concurrency group. The branch
    # check is repeated at the point of alias mutation; a later main push queues
    # its own publication and cannot cause this run to overwrite a newer alias.
    current_main(revision)
    if previous_digest != digest:
        command("docker", "buildx", "imagetools", "create", "--tag", IMAGE + ":development", IMAGE + "@" + digest)
    for attempt in range(6):
        _, actual = find_tag("development")
        if actual == digest:
            return
        need(attempt < 5, "development alias postcondition failed")
        time.sleep(10)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("preflight", "audit", "verify", "promote"))
    parser.add_argument("--revision", required=True)
    parser.add_argument("--digest")
    parser.add_argument("--wait", action="store_true")
    parser.add_argument("--before-attestation", action="store_true")
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    if args.action == "preflight":
        preflight(args.revision, args.wait)
    elif args.action == "audit":
        exists, digest = audit(args.revision)
        need(args.github_output is not None, "audit requires output path")
        registry.write_output(args.github_output, exists, digest is not None, digest or "")
    else:
        need(args.digest is not None, "image digest required")
        if args.action == "verify":
            verify(args.revision, args.digest, not args.before_attestation)
        else:
            need(not args.before_attestation, "promotion always requires attestation")
            promote(args.revision, args.digest)


if __name__ == "__main__":
    main()
