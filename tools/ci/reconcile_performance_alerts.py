#!/usr/bin/env python3
"""Idempotently reconcile Classic performance alert issues with deployed state."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


REPOSITORY = "atrinik/classic"


class ReconcileError(ValueError):
    """Raised when desired alert state or live issue identity is ambiguous."""


def _run_gh(arguments: list[str], *, check: bool = True) -> str:
    result = subprocess.run(
        ["gh", *arguments], capture_output=True, text=True, check=False
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() or f"exit {result.returncode}"
        raise ReconcileError(f"GitHub operation failed: {detail}")
    return result.stdout


def _marker(key: str) -> str:
    digest = hashlib.sha256(key.encode()).hexdigest()[:24]
    return f"<!-- atrinik-performance-alert:{digest} -->"


def _transition_marker(key: str, run_id: str, run_attempt: str, active: bool) -> str:
    digest = hashlib.sha256(key.encode()).hexdigest()[:24]
    desired = "active" if active else "recovered"
    return (
        f"<!-- atrinik-performance-transition:{digest}:"
        f"{run_id}:{run_attempt}:{desired} -->"
    )


def _issue_body(
    key: str, run_id: str, run_attempt: str, active: bool, workflow_url: str
) -> str:
    state = "regressed" if active else "recovered"
    return "\n".join(
        (
            _marker(key),
            _transition_marker(key, run_id, run_attempt, active),
            f"The deployed Classic performance state is **{state}** for `{key}`.",
            "",
            f"Observation: [run {run_id}, attempt {run_attempt}]({workflow_url})",
            "",
            "This comment is generated from the complete digest-validated deployed state.",
        )
    )


def _json_output(arguments: list[str]) -> Any:
    try:
        return json.loads(_run_gh(arguments))
    except json.JSONDecodeError as error:
        raise ReconcileError("GitHub returned malformed JSON") from error


def _issue_text(number: str, body: str) -> str:
    comments = _json_output(
        ["issue", "view", number, "--repo", REPOSITORY, "--json", "comments"]
    ).get("comments", [])
    if not isinstance(comments, list):
        raise ReconcileError("GitHub issue comments are malformed")
    return "\n".join(
        [body] + [str(comment.get("body", "")) for comment in comments]
    )


def reconcile(desired: Any, workflow_url: str) -> None:
    if not isinstance(desired, dict) or desired.get("schema_version") != 1:
        raise ReconcileError("unsupported desired alert schema")
    observation = desired.get("observation")
    alerts = desired.get("alerts")
    if not isinstance(observation, dict) or not isinstance(alerts, dict):
        raise ReconcileError("desired alert state is malformed")
    run_id = observation.get("run_id")
    run_attempt = observation.get("run_attempt")
    if not isinstance(run_id, str) or not run_id.isdigit():
        raise ReconcileError("desired alert run ID is invalid")
    if not isinstance(run_attempt, str) or not run_attempt.isdigit():
        raise ReconcileError("desired alert run attempt is invalid")
    if not workflow_url.startswith(f"https://github.com/{REPOSITORY}/actions/runs/{run_id}"):
        raise ReconcileError("workflow URL does not match the desired observation")
    for key, state in sorted(alerts.items()):
        if (
            not isinstance(key, str)
            or re.fullmatch(
                r"[0-9a-f]{16}:(?:standard|large):sustained_p95", key
            ) is None
            or not isinstance(state, dict)
            or type(state.get("active")) is not bool
        ):
            raise ReconcileError("desired alert entry is malformed")
        title = f"[perf-alert] {key}"
        marker = _marker(key)
        candidates = _json_output(
            [
                "issue", "list", "--repo", REPOSITORY, "--state", "all",
                "--search", f'\"{title}\" in:title', "--limit", "100",
                "--json", "number,title,state,body",
            ]
        )
        matches = [
            issue for issue in candidates
            if issue.get("title") == title and marker in str(issue.get("body", ""))
        ]
        if len(matches) > 1:
            raise ReconcileError(f"multiple owned alert issues match {key}")
        issue = matches[0] if matches else None
        active = state["active"]
        transition = _transition_marker(key, run_id, run_attempt, active)
        body = _issue_body(key, run_id, run_attempt, active, workflow_url)
        if issue is None:
            if active:
                _run_gh(
                    [
                        "issue", "create", "--repo", REPOSITORY, "--title", title,
                        "--body", body, "--label", "component: client",
                    ]
                )
            continue
        number = str(issue["number"])
        issue_state = issue.get("state")
        if active and issue_state == "CLOSED":
            _run_gh(["issue", "reopen", number, "--repo", REPOSITORY])
            _run_gh(["issue", "comment", number, "--repo", REPOSITORY, "--body", body])
        elif active and state.get("last_transition") == "regressed":
            transition_text = _issue_text(number, str(issue.get("body", "")))
            if transition not in transition_text:
                _run_gh(
                    ["issue", "comment", number, "--repo", REPOSITORY, "--body", body]
                )
        elif not active and issue_state == "OPEN":
            if transition not in _issue_text(number, str(issue.get("body", ""))):
                _run_gh(
                    ["issue", "comment", number, "--repo", REPOSITORY, "--body", body]
                )
            _run_gh(["issue", "close", number, "--repo", REPOSITORY])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--alerts", type=Path, required=True)
    parser.add_argument("--workflow-url", required=True)
    args = parser.parse_args()
    try:
        reconcile(json.loads(args.alerts.read_text()), args.workflow_url)
    except (OSError, json.JSONDecodeError, ReconcileError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
