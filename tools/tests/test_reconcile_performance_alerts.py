from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "reconcile_performance_alerts", ROOT / "ci" / "reconcile_performance_alerts.py"
)
assert SPEC and SPEC.loader
reconcile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(reconcile)


KEY = "0123456789abcdef:standard:sustained_p95"
URL = "https://github.com/atrinik/classic/actions/runs/7"


def desired(active: bool, transition: str) -> dict[str, object]:
    return {
        "schema_version": 1,
        "observation": {"run_id": "7", "run_attempt": "2"},
        "alerts": {
            KEY: {
                "active": active,
                "history": [],
                "last_transition": transition,
            }
        },
    }


class FakeGitHub:
    def __init__(self, state: str | None = None) -> None:
        self.state = state
        self.body = reconcile._marker(KEY)
        self.comments: list[str] = []
        self.mutations: list[str] = []

    def __call__(self, arguments: list[str], *, check: bool = True) -> str:
        del check
        operation = arguments[:2]
        if operation == ["issue", "list"]:
            if self.state is None:
                return "[]"
            return json.dumps(
                [{
                    "number": 19,
                    "title": f"[perf-alert] {KEY}",
                    "state": self.state,
                    "body": self.body,
                }]
            )
        if operation == ["issue", "view"]:
            return json.dumps({"comments": [{"body": body} for body in self.comments]})
        if operation == ["issue", "create"]:
            self.state = "OPEN"
            self.body = arguments[arguments.index("--body") + 1]
            self.mutations.append("create")
            return "https://example.test/19\n"
        if operation == ["issue", "comment"]:
            body = arguments[arguments.index("--body") + 1]
            self.comments.append(body)
            self.mutations.append("comment")
            return "https://example.test/19#comment\n"
        if operation == ["issue", "reopen"]:
            self.state = "OPEN"
            self.mutations.append("reopen")
            return ""
        if operation == ["issue", "close"]:
            self.state = "CLOSED"
            self.mutations.append("close")
            return ""
        raise AssertionError(arguments)


class ReconcilePerformanceAlertTests(unittest.TestCase):
    def test_regression_transition_comment_is_idempotent(self) -> None:
        github = FakeGitHub("OPEN")
        with mock.patch.object(reconcile, "_run_gh", side_effect=github):
            reconcile.reconcile(desired(True, "regressed"), URL)
            reconcile.reconcile(desired(True, "regressed"), URL)
        self.assertEqual(github.mutations, ["comment"])

    def test_active_state_creates_or_reopens_owned_issue(self) -> None:
        absent = FakeGitHub()
        with mock.patch.object(reconcile, "_run_gh", side_effect=absent):
            reconcile.reconcile(desired(True, "regressed"), URL)
            reconcile.reconcile(desired(True, "regressed"), URL)
        self.assertEqual(absent.mutations, ["create"])

        closed = FakeGitHub("CLOSED")
        with mock.patch.object(reconcile, "_run_gh", side_effect=closed):
            reconcile.reconcile(desired(True, "none"), URL)
        self.assertEqual(closed.mutations, ["reopen", "comment"])

    def test_recovery_closes_once(self) -> None:
        github = FakeGitHub("OPEN")
        with mock.patch.object(reconcile, "_run_gh", side_effect=github):
            reconcile.reconcile(desired(False, "recovered"), URL)
            reconcile.reconcile(desired(False, "recovered"), URL)
        self.assertEqual(github.mutations, ["comment", "close"])

    def test_ambiguous_owned_issues_fail_closed(self) -> None:
        duplicate = {
            "number": 20,
            "title": f"[perf-alert] {KEY}",
            "state": "OPEN",
            "body": reconcile._marker(KEY),
        }

        def fake(arguments: list[str], *, check: bool = True) -> str:
            del arguments, check
            return json.dumps([duplicate, duplicate])

        with mock.patch.object(reconcile, "_run_gh", side_effect=fake):
            with self.assertRaisesRegex(reconcile.ReconcileError, "multiple"):
                reconcile.reconcile(desired(True, "none"), URL)


if __name__ == "__main__":
    unittest.main()
