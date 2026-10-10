#!/usr/bin/env python3
"""Admit only the current, trusted, checked release revision to publication.

All reads happen under the shared non-cancelling publication lock. Check events
carry an exact revision; package completion is only a wake-up to inspect main.
Neither event payloads nor a namesake check grant publication authority.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

REPOSITORY = "atrinik/classic"
WORKFLOW = ".github/workflows/release.yml"
CHECK = ".github/workflows/check.yml"
PACKAGE = ".github/workflows/package-release.yml"
SHA = re.compile(r"[0-9a-f]{40}")
BRANCH = re.compile(r"main|[0-9]+\.[0-9]+\.x")


def need(condition: object, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def api(path: str) -> dict:
    result = subprocess.run(["gh", "api", f"repos/{REPOSITORY}/{path}"],
                            capture_output=True, text=True, timeout=60, check=True)
    need(len(result.stdout) < 8 * 1024 * 1024, "oversize GitHub response")
    data = json.loads(result.stdout)
    need(isinstance(data, dict), "invalid GitHub response")
    return data


def inventory(data: dict, key: str) -> list[dict]:
    values = data.get(key)
    need(isinstance(values, list) and all(isinstance(value, dict) for value in values),
         "invalid release validation inventory")
    need(type(data.get("total_count")) is int and data["total_count"] == len(values)
         and len(values) <= 100, "release validation inventory is incomplete")
    return values


def canonical(run: dict, path: str, event: str, branch: str | None = None) -> bool:
    return (run.get("path") == path and run.get("event") == event
            and run.get("repository", {}).get("full_name") == REPOSITORY
            and run.get("head_repository", {}).get("full_name") == REPOSITORY
            and (branch is None or run.get("head_branch") == branch))


def current(branch: str) -> str:
    need(BRANCH.fullmatch(branch), "unsupported release branch")
    ref = api(f"git/ref/heads/{branch}")
    revision = ref.get("object", {}).get("sha", "")
    need(ref.get("ref") == f"refs/heads/{branch}" and
         ref.get("object", {}).get("type") == "commit" and SHA.fullmatch(revision),
         "invalid release branch identity")
    return revision


def checked(branch: str, revision: str) -> dict | None:
    runs = inventory(api(f"actions/workflows/check.yml/runs?event=push&"
                         f"branch={branch}&head_sha={revision}&per_page=100"), "workflow_runs")
    runs = [run for run in runs if canonical(run, CHECK, "push", branch)
            and run.get("head_sha") == revision]
    if not runs:
        return None
    need(all(type(run.get("id")) is int and run["id"] > 0 for run in runs),
         "invalid Check run identity")
    run = max(runs, key=lambda item: item["id"])
    if run.get("status") != "completed" or run.get("conclusion") != "success":
        return None
    suite = run.get("check_suite_id")
    need(type(suite) is int and suite > 0, "invalid Check suite identity")
    checks = inventory(api(f"check-suites/{suite}/check-runs?filter=latest&per_page=100"),
                       "check_runs")
    matches = [check for check in checks if check.get("name") == "Classic validation"
               and check.get("app", {}).get("id") == 15368
               and check.get("head_sha") == revision
               and check.get("check_suite", {}).get("id") == suite]
    need(len(matches) == 1 and matches[0].get("status") == "completed"
         and matches[0].get("conclusion") == "success",
         "successful trusted Classic validation aggregate is missing")
    return run


def context(env: dict[str, str]) -> str:
    branch = env.get("GITHUB_REF_NAME", "")
    need(BRANCH.fullmatch(branch), "unsupported release branch")
    need(env.get("GITHUB_REPOSITORY") == REPOSITORY
         and env.get("GITHUB_REF") == f"refs/heads/{branch}"
         and env.get("GITHUB_WORKFLOW_REF") == f"{REPOSITORY}/{WORKFLOW}@refs/heads/{branch}"
         and SHA.fullmatch(env.get("GITHUB_SHA", "")),
         "release requires the canonical protected-branch workflow")
    return branch


def trigger(payload: dict) -> dict:
    supplied = payload.get("workflow_run", {})
    run_id = supplied.get("id")
    need(type(run_id) is int and run_id > 0, "invalid triggering run identity")
    run = api(f"actions/runs/{run_id}")
    need(all(run.get(field) == supplied.get(field) for field in
             ("id", "head_sha", "head_branch", "path", "event")),
         "triggering run changed or does not match its event")
    need(supplied.get("status") == "completed" and supplied.get("conclusion") == "success"
         and SHA.fullmatch(run.get("head_sha", "")), "triggering event is not successful")
    need(canonical(run, CHECK, "push", "main") or
         canonical(run, PACKAGE, "workflow_dispatch"), "untrusted release trigger")
    if run["path"] == PACKAGE:
        need(BRANCH.fullmatch(run.get("head_branch", "")) or
             re.fullmatch(r"v5\.[0-9]+\.[0-9]+", run.get("head_branch", "")),
             "package wake-up requires a release branch or immutable release tag")
    run = dict(run)
    run["superseded"] = (run.get("run_attempt") != supplied.get("run_attempt") or
                         run.get("status") != "completed" or run.get("conclusion") != "success")
    return run


def select(env: dict[str, str], payload: dict, wait: bool = False) -> dict[str, str]:
    branch = context(env)
    event = env.get("GITHUB_EVENT_NAME")
    need(event in ("push", "workflow_dispatch", "workflow_run"), "unsupported release event")
    run = None
    if event == "workflow_run":
        need(branch == "main", "automatic release requires main workflow definition")
        run = trigger(payload)
        if run["superseded"]:
            return {"proceed": "false", "branch": branch, "revision": run["head_sha"],
                    "reason": "superseded-trigger"}
        # A package completion is a wake-up only. It never supplies source authority.
        revision = run["head_sha"] if run["path"] == CHECK else current(branch)
        if run["path"] == PACKAGE:
            comparison = api(f"compare/{run['head_sha']}...{revision}")
            if (re.fullmatch(r"[0-9]+\.[0-9]+\.x", run["head_branch"]) and
                    comparison.get("status") in ("behind", "diverged") and
                    SHA.fullmatch(comparison.get("merge_base_commit", {}).get("sha", ""))):
                return {"proceed": "false", "branch": branch, "revision": revision,
                        "reason": "maintenance-package-outside-main"}
            need(comparison.get("status") in ("ahead", "identical") and
                 comparison.get("merge_base_commit", {}).get("sha") == run["head_sha"],
                 "package source is not an ancestor of current main")
    else:
        need(event != "push" or branch != "main", "main releases follow Check completion")
        revision = env["GITHUB_SHA"]
    result = {"proceed": "false", "branch": branch, "revision": revision}
    for attempt in range(30 if wait and event != "workflow_run" else 1):
        if current(branch) != revision:
            return result | {"reason": "superseded"}
        validation = checked(branch, revision)
        if validation is not None:
            if run is not None and run["path"] == CHECK:
                if any(validation.get(key) != run.get(key) for key in ("id", "run_attempt")):
                    return result | {"reason": "superseded-check"}
            # Fence head movement while reading run/suite evidence.
            if current(branch) != revision:
                return result | {"reason": "superseded"}
            return result | {"proceed": "true", "reason": "checked-current-head"}
        if wait and event != "workflow_run" and attempt < 29:
            time.sleep(30)
    need(event == "workflow_run",
         f"Classic validation did not complete successfully for {branch} at {revision}")
    return result | {"reason": "awaiting-successful-check"}


def recheck(branch: str, revision: str) -> bool:
    need(BRANCH.fullmatch(branch) and SHA.fullmatch(revision), "invalid release coordinate")
    checkout = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True, timeout=30).stdout.strip()
    need(checkout == revision, "release checkout does not match the checked revision")
    return current(branch) == revision and checked(branch, revision) is not None and current(branch) == revision


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--github-output", required=True, type=Path)
    parser.add_argument("--recheck", action="store_true")
    parser.add_argument("--require-current", action="store_true")
    parser.add_argument("--branch")
    parser.add_argument("--revision")
    args = parser.parse_args()
    need(not args.require_current or args.recheck, "--require-current requires --recheck")
    exit_code = 0
    if args.recheck:
        branch = context(dict(os.environ))
        need(args.branch == branch, "recheck branch differs from the workflow branch")
        need(args.branch and args.revision, "recheck requires a selected branch and revision")
        proceed = recheck(args.branch, args.revision)
        # Reserved status 3 means a normal superseded batch, never an API,
        # provenance, context, checkout, or argument error. The workflow latches
        # this disposition before skipping the remaining mutation steps.
        exit_code = 3 if args.require_current and not proceed else 0
        values = {"proceed": str(proceed).lower()}
    else:
        payload = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
        values = select(dict(os.environ), payload, wait=True)
    with args.github_output.open("a", encoding="utf-8") as stream:
        for key, value in values.items():
            need("\n" not in value and "\r" not in value, "invalid release output")
            stream.write(f"{key}={value}\n")
    print(json.dumps(values, sort_keys=True))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
