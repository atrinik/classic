from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "select_release_batch", Path(__file__).resolve().parents[1] / "release/select_release_batch.py")
assert SPEC is not None and SPEC.loader is not None
batch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(batch)

OLD = "a" * 40
HEAD = "b" * 40


def environment(event="workflow_run", branch="main", revision=HEAD):
    return {"GITHUB_REPOSITORY": batch.REPOSITORY, "GITHUB_EVENT_NAME": event,
            "GITHUB_REF": "refs/heads/" + branch, "GITHUB_REF_NAME": branch,
            "GITHUB_SHA": revision,
            "GITHUB_WORKFLOW_REF": f"{batch.REPOSITORY}/{batch.WORKFLOW}@refs/heads/{branch}"}


def check_run(revision=HEAD, **updates):
    return {"id": 10, "run_attempt": 1, "head_sha": revision, "head_branch": "main",
            "path": batch.CHECK, "event": "push", "status": "completed", "conclusion": "success",
            "repository": {"full_name": batch.REPOSITORY},
            "head_repository": {"full_name": batch.REPOSITORY}, "check_suite_id": 20} | updates


def package_run(revision=OLD, **updates):
    return check_run(revision, id=30, path=batch.PACKAGE, event="workflow_dispatch",
                     head_branch="v5.80.0") | updates


def aggregate(revision=HEAD, **updates):
    return {"name": "Classic validation", "app": {"id": 15368}, "head_sha": revision,
            "check_suite": {"id": 20}, "status": "completed", "conclusion": "success"} | updates


class BatchTests(unittest.TestCase):
    def setUp(self):
        self.head = HEAD
        self.trigger = check_run()
        self.runs = [check_run()]
        self.checks = [aggregate()]
        self.requests = []
        self.patch = patch.object(batch, "api", side_effect=self.api)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def api(self, path):
        self.requests.append(path)
        if path.startswith("git/ref/heads/"):
            return {"ref": "refs/heads/" + path.rsplit("/", 1)[-1],
                    "object": {"type": "commit", "sha": self.head}}
        if path.startswith("actions/runs/"):
            return self.trigger
        if path.startswith("actions/workflows/check.yml/runs?"):
            return {"total_count": len(self.runs), "workflow_runs": self.runs}
        if path.startswith("check-suites/"):
            return {"total_count": len(self.checks), "check_runs": self.checks}
        if path.startswith("compare/"):
            return {"status": "ahead", "merge_base_commit": {"sha": self.trigger["head_sha"]}}
        raise AssertionError(path)

    def select(self, supplied=None, env=None):
        return batch.select(env or environment(), {"workflow_run": supplied or self.trigger})

    def test_exact_checked_current_main_is_selected(self):
        self.assertEqual(self.select(), {"branch": "main", "revision": HEAD,
                                        "proceed": "true", "reason": "checked-current-head"})
        self.assertIn("check-suites/20/check-runs?filter=latest&per_page=100", self.requests)

    def test_new_then_old_out_of_order_events_cannot_roll_back_batch(self):
        self.assertEqual(self.select()["revision"], HEAD)
        self.trigger = check_run(OLD)
        self.assertEqual(self.select()["reason"], "superseded")
        self.assertEqual(self.select()["proceed"], "false")

    def test_rapid_merge_completions_coalesce_to_current_checked_head(self):
        selected = []
        for revision in (OLD, HEAD, OLD):
            self.trigger = check_run(revision)
            decision = self.select()
            if decision["proceed"] == "true":
                selected.append(decision["revision"])
        self.assertEqual(selected, [HEAD])

    def test_untrusted_trigger_provenance_rejected_before_check_lookup(self):
        for changes in ({"event": "pull_request"}, {"event": "workflow_dispatch"},
                        {"head_branch": "feature"}, {"path": ".github/workflows/fake.yml"},
                        {"repository": {"full_name": "foreign/classic"}},
                        {"head_repository": {"full_name": "fork/classic"}}):
            with self.subTest(changes=changes):
                self.trigger = check_run(**changes)
                with self.assertRaisesRegex(RuntimeError, "untrusted"):
                    self.select()

    def test_payload_cannot_substitute_a_live_run(self):
        payload = check_run(OLD)
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            self.select(payload)

    def test_rerun_after_queued_success_is_a_stale_noop(self):
        payload = check_run()
        for status, conclusion in (("in_progress", None), ("completed", "failure"),
                                   ("completed", "success")):
            self.trigger = check_run(run_attempt=2, status=status, conclusion=conclusion)
            with self.subTest(status=status, conclusion=conclusion):
                self.assertEqual(self.select(payload)["reason"], "superseded-trigger")

    def test_newer_check_run_overrides_older_success_even_with_reordered_inventory(self):
        for newer in (check_run(id=11), check_run(id=11, conclusion="failure"),
                      check_run(id=11, status="in_progress", conclusion=None)):
            for ordered in ([newer, check_run()], [check_run(), newer]):
                self.runs = ordered
                self.assertEqual(self.select()["proceed"], "false")

    def test_wrong_suite_application_revision_or_failed_aggregate_rejected(self):
        for changes in ({"check_suite": {"id": 21}}, {"app": {"id": 1}},
                        {"head_sha": OLD}, {"conclusion": "failure"}, {"status": "in_progress"}):
            with self.subTest(changes=changes):
                self.checks = [aggregate(**changes)]
                with self.assertRaisesRegex(RuntimeError, "aggregate is missing"):
                    self.select()

    def test_foreign_or_pr_validation_does_not_authorize_manual_main(self):
        for changes in ({"event": "pull_request"}, {"head_repository": {"full_name": "fork/classic"}},
                        {"path": batch.PACKAGE}, {"head_branch": "feature"}):
            self.runs = [check_run(**changes)]
            with self.assertRaisesRegex(RuntimeError, "Classic validation did not complete"):
                self.select(env=environment("workflow_dispatch"))

    def test_branch_changes_during_validation_cannot_authorize_old_head(self):
        with patch.object(batch, "current", side_effect=[HEAD, OLD]):
            self.assertEqual(self.select()["reason"], "superseded")

    def test_package_completion_recovers_new_batch_after_old_draft_consumed_check(self):
        # A Check wake-up can first resume an old draft instead of versioning HEAD.
        self.assertEqual(self.select()["revision"], HEAD)
        # Finishing that old package must select HEAD even without another Check event.
        self.trigger = package_run()
        decision = self.select(env=environment(revision=OLD))
        self.assertEqual((decision["proceed"], decision["revision"]), ("true", HEAD))
        self.assertIn(f"compare/{OLD}...{HEAD}", self.requests)

    def test_package_completion_does_not_release_unchecked_new_main(self):
        self.trigger = package_run()
        self.runs = [check_run(status="in_progress", conclusion=None)]
        self.assertEqual(self.select()["reason"], "awaiting-successful-check")

    def test_recovery_branch_package_wakes_current_head(self):
        self.trigger = package_run(head_branch="main")
        self.assertEqual(self.select()["proceed"], "true")

    def test_package_from_feature_branch_or_foreign_source_is_rejected(self):
        for changes in ({"head_branch": "feature"}, {"event": "push"},
                        {"head_repository": {"full_name": "fork/classic"}}):
            self.trigger = package_run(**changes)
            with self.assertRaises(RuntimeError):
                self.select()
        self.trigger = package_run()
        with patch.object(batch, "api", side_effect=[self.trigger,
                          {"ref": "refs/heads/main", "object": {"type": "commit", "sha": HEAD}},
                          {"status": "diverged", "merge_base_commit": {"sha": OLD}}]):
            with self.assertRaisesRegex(RuntimeError, "not an ancestor"):
                self.select()

    def test_manual_main_and_maintenance_are_checked_exactly(self):
        self.assertEqual(self.select(env=environment("workflow_dispatch"))["proceed"], "true")
        self.runs = [check_run(head_branch="5.80.x")]
        for event in ("push", "workflow_dispatch"):
            with self.subTest(event=event):
                self.assertEqual(self.select(env=environment(event, "5.80.x"))["proceed"], "true")

    def test_manual_wait_is_bounded_and_never_changes_selected_revision(self):
        self.runs = []
        for event, branch in (("workflow_dispatch", "main"), ("push", "5.80.x"),
                              ("workflow_dispatch", "5.80.x")):
            with self.subTest(event=event, branch=branch), patch.object(batch.time, "sleep") as sleep:
                with self.assertRaisesRegex(RuntimeError, f"did not complete successfully for {branch} at {HEAD}"):
                    batch.select(environment(event, branch), {}, wait=True)
                self.assertEqual(sleep.call_count, 29)
                self.assertEqual(sleep.call_args.args, (30,))

    def test_divergent_authenticated_maintenance_package_is_a_noop(self):
        self.trigger = package_run(head_branch="5.80.x")
        for status in ("behind", "diverged"):
            with self.subTest(status=status), patch.object(batch, "api", side_effect=[self.trigger,
                    {"ref": "refs/heads/main", "object": {"type": "commit", "sha": HEAD}},
                    {"status": status, "merge_base_commit": {"sha": "c" * 40}}]):
                result = self.select()
                self.assertEqual(result["proceed"], "false")
                self.assertEqual(result["reason"], "maintenance-package-outside-main")

    def test_maintenance_package_does_not_hide_malformed_or_failed_api(self):
        self.trigger = package_run(head_branch="5.80.x")
        for response in ({"status": "diverged"}, RuntimeError("API unavailable")):
            with self.subTest(response=response), patch.object(batch, "api", side_effect=[self.trigger,
                    {"ref": "refs/heads/main", "object": {"type": "commit", "sha": HEAD}}, response]):
                with self.assertRaises(RuntimeError):
                    self.select()

    def test_cli_reserves_status_three_for_authenticated_supersession(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "output"
            argv = ["select_release_batch.py", "--recheck", "--require-current", "--branch", "main",
                    "--revision", HEAD, "--github-output", str(output)]
            for proceed, expected_status in ((False, 3), (True, 0)):
                with self.subTest(proceed=proceed), patch("sys.argv", argv), patch.dict(
                        os.environ, environment(), clear=True), patch.object(batch, "recheck", return_value=proceed):
                    output.write_text("")
                    self.assertEqual(batch.main(), expected_status)
                    self.assertEqual(output.read_text(), f"proceed={str(proceed).lower()}\n")
            with patch("sys.argv", argv), patch.dict(os.environ, environment(), clear=True), patch.object(
                    batch, "recheck", side_effect=RuntimeError("API unavailable")):
                output.write_text("")
                with self.assertRaisesRegex(RuntimeError, "API unavailable"):
                    batch.main()
                self.assertEqual(output.read_text(), "")
            for changes in ({"GITHUB_REPOSITORY": "fork/classic"}, {"GITHUB_REF_NAME": "feature"}):
                with self.subTest(changes=changes), patch("sys.argv", argv), patch.dict(
                        os.environ, environment() | changes, clear=True), patch.object(batch, "recheck") as recheck:
                    with self.assertRaises(RuntimeError):
                        batch.main()
                    recheck.assert_not_called()

    def test_cli_cannot_recheck_another_branch_or_require_without_recheck(self):
        with tempfile.TemporaryDirectory() as temporary:
            argv = ["select_release_batch.py", "--require-current", "--branch", "5.80.x",
                    "--revision", HEAD, "--github-output", str(Path(temporary) / "output")]
            for args in (argv, argv + ["--recheck"]):
                with self.subTest(args=args), patch("sys.argv", args), patch.dict(
                        os.environ, environment(), clear=True), patch.object(batch, "recheck") as recheck:
                    with self.assertRaises(RuntimeError):
                        batch.main()
                    recheck.assert_not_called()

    def test_main_push_and_unprotected_contexts_are_rejected(self):
        for env in (environment("push"), environment("pull_request"), environment(branch="feature"),
                    environment() | {"GITHUB_REF": "refs/tags/v5.80.0"},
                    environment() | {"GITHUB_REPOSITORY": "fork/classic"},
                    environment() | {"GITHUB_WORKFLOW_REF": "foreign/path@refs/heads/main"}):
            with self.subTest(env=env), self.assertRaises(RuntimeError):
                self.select(env=env)

    def test_recheck_fences_checkout_head_checks_and_api_failure(self):
        completed = subprocess.CompletedProcess([], 0, stdout=HEAD + "\n")
        with patch.object(batch.subprocess, "run", return_value=completed):
            self.assertTrue(batch.recheck("main", HEAD))
            self.head = OLD
            self.assertFalse(batch.recheck("main", HEAD))
            self.head = HEAD
            self.runs = [check_run(conclusion="failure")]
            self.assertFalse(batch.recheck("main", HEAD))
            with patch.object(batch, "api", side_effect=RuntimeError("API unavailable")):
                with self.assertRaisesRegex(RuntimeError, "API unavailable"):
                    batch.recheck("main", HEAD)
        with patch.object(batch.subprocess, "run", return_value=completed):
            with self.assertRaisesRegex(RuntimeError, "checkout"):
                batch.recheck("main", OLD)

    def test_incomplete_validation_inventory_fails_closed(self):
        for inventory in ({"total_count": 101, "workflow_runs": [check_run()]},
                          {"workflow_runs": [check_run()]}, {"total_count": 1, "workflow_runs": [None]}):
            with self.subTest(inventory=inventory), self.assertRaises(RuntimeError):
                batch.inventory(inventory, "workflow_runs")


if __name__ == "__main__":
    unittest.main()
