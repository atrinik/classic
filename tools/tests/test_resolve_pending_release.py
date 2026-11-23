from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit


RELEASE_TOOLS = Path(__file__).resolve().parents[1] / "release"
sys.path.insert(0, str(RELEASE_TOOLS))
MODULE_PATH = RELEASE_TOOLS / "resolve_pending_release.py"
SPEC = importlib.util.spec_from_file_location("resolve_pending_release", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
resolve_pending_release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(resolve_pending_release)


TAG = "v5.8.1"
COMMIT = "76d9ece39f9fc141d485a4f68ba23134f8a3a2e8"
RELEASE_ID = 367395490
RUN_IDS = [31298735525, 31341539056]


def draft(**updates: object) -> dict[str, object]:
    value: dict[str, object] = {
        "id": RELEASE_ID,
        "tag_name": TAG,
        "draft": True,
        "prerelease": False,
        "assets": [],
    }
    value.update(updates)
    return value


def policy() -> dict[str, object]:
    return {
        TAG: {
            "commit": COMMIT,
            "disposition": "delete-empty-draft",
            "empty_draft_id": RELEASE_ID,
            "failed_package_run_ids": RUN_IDS,
            "windows_server_conclusion": "failure",
            "server_image_conclusion": "failure",
        }
    }


def retained_candidate(**updates: str) -> dict[str, str]:
    value = {
        "tag": "v5.37.0",
        "candidate_run_id": "32048332566",
        "run_commit": "1" * 40,
        "artifact_digest": "sha256:" + "2" * 64,
        "failure_class": "release-publication",
    }
    value.update(updates)
    return value


def retained_publication_steps(failed_name: str) -> list[dict[str, str]]:
    steps = [
        {
            "name": name,
            "conclusion": "failure" if name == failed_name else "success",
        }
        for name in resolve_pending_release.RETAINED_PROOF_STEPS
    ]
    if failed_name not in resolve_pending_release.RETAINED_PROOF_STEPS:
        steps.append({"name": failed_name, "conclusion": "failure"})
    return steps


WORKFLOWS = Path(__file__).resolve().parents[2] / ".github" / "workflows"


def workflow_jobs(name: str) -> dict[str, tuple[str, str]]:
    """Map each job ID to its display name and text block, without YAML."""
    text = (WORKFLOWS / name).read_text(encoding="utf-8")
    jobs = text[text.index("\njobs:\n") :]
    starts = list(re.finditer(r"(?m)^  ([A-Za-z0-9_-]+):\n    name: (.+)$", jobs))
    return {
        match[1]: (match[2], jobs[match.start() : starts[index + 1].start()
                                  if index + 1 < len(starts) else len(jobs)])
        for index, match in enumerate(starts)
    }


def step_names(job: str) -> list[str]:
    return [first or second for first, second in
            re.findall(r"(?m)^      - name: (.+)$|^        name: (.+)$", job)]


class WorkflowIdentityTests(unittest.TestCase):
    def test_every_inspected_job_and_step_name_exists(self) -> None:
        # A renamed job or step silently turns its evidence check into a
        # permanent mismatch, so the resolver must name the live workflow.
        package = workflow_jobs("package-release.yml")
        candidate_caller, candidate_job = package["candidate"]
        self.assertIn("uses: ./.github/workflows/build-release-candidate.yml", candidate_job)
        reusable = {
            f"{candidate_caller} / {name}"
            for name, _ in workflow_jobs("build-release-candidate.yml").values()
        }
        for job in (
            resolve_pending_release.IMAGE_JOB,
            resolve_pending_release.WINDOWS_JOB,
            resolve_pending_release.CANDIDATE_FINALIZER_JOB,
        ):
            self.assertIn(job, reusable)
        publish_name, publish_job = package["publish"]
        self.assertEqual(resolve_pending_release.PUBLISH_JOB, publish_name)
        steps = step_names(publish_job)
        for step in (
            resolve_pending_release.PUBLISH_STEP,
            resolve_pending_release.IMAGE_INSPECTION_STEP,
            *resolve_pending_release.RETAINED_PROOF_STEPS,
        ):
            self.assertEqual(steps.count(step), 1, step)
        workflow = (WORKFLOWS / "package-release.yml").read_text(encoding="utf-8")
        for job in (resolve_pending_release.CANDIDATE_FINALIZER_JOB, publish_name):
            self.assertIn(f'select(.name == "{job}" and .conclusion == ', workflow)


class ResolvePendingReleaseTests(unittest.TestCase):
    def test_validate_branch_accepts_main_and_maintenance_lines(self) -> None:
        self.assertEqual(resolve_pending_release.validate_branch("main"), "main")
        self.assertEqual(
            resolve_pending_release.validate_branch("8.3.x"), "8.3.x"
        )
        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError, "release branch must be"
        ):
            resolve_pending_release.validate_branch("feature/release")

    def test_resolve_repository_checks_the_maintenance_remote(self) -> None:
        with (
            patch.object(
                resolve_pending_release,
                "command",
                side_effect=["b" * 40, "b" * 40],
            ) as command,
            patch.object(resolve_pending_release, "list_drafts", return_value=[]),
            patch.object(
                resolve_pending_release,
                "resolve",
                return_value={"action": "none", "tag": "", "release_id": ""},
            ),
        ):
            self.assertEqual(
                resolve_pending_release.resolve_repository(
                    "atrinik/classic", "8.3.x"
                ),
                {"action": "none", "tag": "", "release_id": ""},
            )

        self.assertEqual(
            command.call_args_list[1].args,
            ("git", "rev-parse", "refs/remotes/origin/8.3.x"),
        )

    def test_resolve_repository_rejects_a_stale_maintenance_head(self) -> None:
        with patch.object(
            resolve_pending_release,
            "command",
            side_effect=["a" * 40, "b" * 40],
        ):
            with self.assertRaisesRegex(
                resolve_pending_release.PendingReleaseError,
                "current origin/8.3.x",
            ):
                resolve_pending_release.resolve_repository(
                    "atrinik/classic", "8.3.x"
                )

    def test_resolve_repository_checks_retained_tag_on_the_selected_line(self) -> None:
        with (
            patch.object(
                resolve_pending_release,
                "command",
                side_effect=["a" * 40, "a" * 40],
            ),
            patch.object(
                resolve_pending_release, "list_drafts", return_value=[]
            ),
            patch.object(
                resolve_pending_release,
                "resolve",
                return_value={
                    "action": "resume",
                    "tag": "v8.3.1",
                    "release_id": "10",
                },
            ),
            patch.object(
                resolve_pending_release, "is_ancestor", return_value=False
            ),
        ):
            with self.assertRaisesRegex(
                resolve_pending_release.PendingReleaseError,
                "not an ancestor of current 8.3.x",
            ):
                resolve_pending_release.resolve_repository(
                    "atrinik/classic", "8.3.x"
                )

    def test_main_passes_the_selected_branch_to_the_resolver(self) -> None:
        with (
            patch.object(
                sys,
                "argv",
                [
                    "resolve_pending_release.py",
                    "--repository",
                    "atrinik/classic",
                    "--branch",
                    "8.3.x",
                ],
            ),
            patch.object(
                resolve_pending_release,
                "resolve_repository",
                return_value={"action": "none", "tag": "", "release_id": ""},
            ) as resolver,
        ):
            self.assertEqual(resolve_pending_release.main(), 0)
        resolver.assert_called_once_with("atrinik/classic", "8.3.x")

    def test_guarded_deletion_main_passes_the_selected_branch(self) -> None:
        with (
            patch.object(
                sys,
                "argv",
                [
                    "resolve_pending_release.py",
                    "--repository",
                    "atrinik/classic",
                    "--branch",
                    "8.3.x",
                    "--delete-policy-listed-empty-draft",
                    "--expected-tag",
                    "v8.3.1",
                    "--expected-release-id",
                    "10",
                ],
            ),
            patch.object(
                resolve_pending_release,
                "delete_policy_listed_empty_draft",
            ) as delete,
        ):
            self.assertEqual(resolve_pending_release.main(), 0)
        self.assertEqual(delete.call_args.args[:3], ("atrinik/classic", "v8.3.1", 10))

    def test_no_draft_allows_semantic_release(self) -> None:
        self.assertEqual(
            resolve_pending_release.resolve(
                [], policy(), lambda tag: COMMIT, lambda run, windows, image: None
            ),
            {"action": "none", "tag": "", "release_id": ""},
        )

    def test_ordinary_empty_draft_is_resumed(self) -> None:
        release = draft(id=10, tag_name="v5.8.0")
        self.assertEqual(
            resolve_pending_release.resolve(
                [release],
                policy(),
                lambda tag: "0" * 40,
                lambda run, windows, image: None,
            ),
            {"action": "resume", "tag": "v5.8.0", "release_id": "10"},
        )

    def test_recorded_empty_failed_draft_is_deleted(self) -> None:
        validated: list[tuple[int, str, str]] = []
        self.assertEqual(
            resolve_pending_release.resolve(
                [draft()],
                policy(),
                lambda tag: COMMIT,
                lambda run, windows, image: validated.append((run, windows, image)),
            ),
            {
                "action": "delete-empty-draft",
                "tag": TAG,
                "release_id": str(RELEASE_ID),
            },
        )
        self.assertEqual(
            validated, [(run, "failure", "failure") for run in RUN_IDS]
        )

    def test_failed_draft_must_remain_exact_and_empty(self) -> None:
        cases = (
            (draft(id=RELEASE_ID + 1), policy(), COMMIT),
            (draft(assets=[{"name": "partial.zip"}]), policy(), COMMIT),
            (draft(), policy(), "0" * 40),
        )
        for release, failed_policy, commit in cases:
            with self.subTest(release=release, commit=commit):
                with self.assertRaisesRegex(
                    resolve_pending_release.PendingReleaseError,
                    "no longer matches policy",
                ):
                    resolve_pending_release.resolve(
                        [release],
                        failed_policy,
                        lambda tag: commit,
                        lambda run, windows, image: None,
                    )

    def test_non_draft_release_cannot_be_deleted(self) -> None:
        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "metadata is invalid",
        ):
            resolve_pending_release.resolve(
                [draft(draft=False)],
                policy(),
                lambda tag: COMMIT,
                lambda run, windows, image: None,
            )

    def test_partial_ordinary_draft_requires_retained_candidate(self) -> None:
        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "retained-candidate recovery is required",
        ):
            resolve_pending_release.resolve(
                [draft(id=10, tag_name="v5.8.0", assets=[{"name": "partial.zip"}])],
                policy(),
                lambda tag: "0" * 40,
                lambda run, windows, image: None,
            )

    def test_complete_ordinary_draft_dispatches_exact_retained_candidate(self) -> None:
        result = resolve_pending_release.resolve(
            [
                draft(
                    id=371791046,
                    tag_name="v5.37.0",
                    assets=[{"name": "atrinik-classic-v5.37.0.tar.gz"}],
                )
            ],
            policy(),
            lambda tag: "0" * 40,
            lambda run, windows, image: None,
            lambda tag: [retained_candidate()],
        )
        self.assertEqual(
            result,
            {
                "action": "resume-retained-candidate",
                "tag": "v5.37.0",
                "release_id": "371791046",
                "candidate_run_id": "32048332566",
                "failure_class": "release-publication",
            },
        )

    def test_failed_retained_continuations_stop_at_the_attempt_cap(self) -> None:
        def resolved(classes: list[str]) -> dict[str, str]:
            return resolve_pending_release.resolve(
                [draft(id=371791046, tag_name="v5.37.0",
                       assets=[{"name": "atrinik-classic-v5.37.0.tar.gz"}])],
                policy(),
                lambda tag: "0" * 40,
                lambda run, windows, image: None,
                lambda tag: [retained_candidate()],
                lambda tag: classes,
            )

        self.assertEqual(
            resolved(["release-publication", "release-publication"])["action"],
            "resume-retained-candidate",
        )
        self.assertEqual(
            resolved(["release-publication"] * 3),
            {
                "action": "blocked",
                "tag": "v5.37.0",
                "release_id": "371791046",
                "failure_class": "release-publication",
                "retry_count": "3",
            },
        )

    def test_retained_candidate_and_continuations_share_the_real_lineage_budget(self) -> None:
        tag = "v5.120.0"
        repository = "atrinik/classic"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            environment = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
                           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
                           "GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1",
                           "PATH": os.environ.get("PATH", "/usr/bin:/bin")}

            def git(*arguments: str) -> str:
                return subprocess.run(["git", "-C", str(root), *arguments], check=True,
                                      capture_output=True, text=True, env=environment).stdout.strip()

            git("init", "-q", "-b", "main")
            commits = []
            for message in ("before-tag", "tag", "main-1", "main-2", "main-3"):
                git("commit", "-q", "--allow-empty", "-m", message)
                commits.append(git("rev-parse", "HEAD"))
            before_tag, tag_commit, main_1, main_2, head = commits

            def run(run_id: int, branch: str, sha: str) -> dict[str, object]:
                return {"id": run_id, "name": "Package Release",
                        "path": ".github/workflows/package-release.yml",
                        "event": "workflow_dispatch", "conclusion": "failure",
                        "status": "completed", "head_branch": branch, "head_sha": sha,
                        "head_repository": {"full_name": repository}}

            publisher_steps = retained_publication_steps(resolve_pending_release.PUBLISH_STEP)
            jobs = {
                100: [{"name": resolve_pending_release.CANDIDATE_FINALIZER_JOB, "conclusion": "success"},
                      {"name": resolve_pending_release.PUBLISH_JOB, "conclusion": "failure",
                       "steps": publisher_steps}],
                201: [{"name": resolve_pending_release.PUBLISH_JOB, "conclusion": "failure",
                       "steps": [{"name": resolve_pending_release.PUBLISH_STEP, "conclusion": "failure"}]}],
                202: [{"name": "Bind workflow definition to release tag", "conclusion": "failure"}],
                50: [],
            }
            # A tag-ref candidate, two main-ref continuations at later main
            # heads, and an older main-ref failure off the tag's lineage.
            runs = {100: run(100, tag, tag_commit), 201: run(201, "main", main_1),
                    202: run(202, "main", main_2), 50: run(50, "main", before_tag)}

            def resolved(listed_ids: list[int]) -> dict[str, str]:
                listed = [runs[run_id] for run_id in listed_ids]

                def request(path: str) -> object:
                    if "/actions/workflows/package-release.yml/runs?" in path:
                        return {"total_count": len(listed), "workflow_runs": listed}
                    parts = path.split("?")[0].split("/")
                    run_id = int(parts[5])
                    if len(parts) == 6:
                        return runs[run_id]
                    if parts[6] == "jobs":
                        return {"total_count": len(jobs[run_id]), "jobs": jobs[run_id]}
                    artifacts = ([{"name": f"complete-release-candidate-{tag}", "expired": False,
                                   "size_in_bytes": 10, "digest": "sha256:" + "d" * 64}]
                                 if run_id == 100 else [])
                    return {"total_count": len(artifacts), "artifacts": artifacts}

                return resolve_pending_release.resolve(
                    [draft(id=7, tag_name=tag, assets=[{"name": "SHA256SUMS"}])],
                    {},
                    lambda _: tag_commit,
                    lambda *_: None,
                    lambda name: resolve_pending_release.find_retained_candidates(
                        repository, name, tag_commit, head, request),
                    lambda name: resolve_pending_release.recent_failure_classes(
                        repository, name, tag_commit, head, request),
                )

            # The resolver's Git calls inherit the process environment; drop any
            # GIT_* repository overrides so they read the temporary lineage.
            clean = {key: value for key, value in os.environ.items()
                     if not key.startswith("GIT_")}
            clean |= {"GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1"}
            with (
                patch.object(resolve_pending_release, "ROOT", root),
                patch.dict(os.environ, clean, clear=True),
            ):
                first = resolved([100, 50])
                self.assertEqual(first["action"], "resume-retained-candidate")
                self.assertEqual(first["retry_count"], "1")
                second = resolved([201, 100, 50])
                self.assertEqual(second["action"], "resume-retained-candidate")
                self.assertEqual(second["candidate_run_id"], "100")
                self.assertEqual(second["retry_count"], "2")
                self.assertEqual(
                    resolved([202, 201, 100, 50]),
                    {"action": "blocked", "tag": tag, "release_id": "7",
                     "failure_class": "unknown", "retry_count": "3"},
                )

    def test_off_lineage_failures_do_not_use_the_attempt_budget(self) -> None:
        tag_commit = "1" * 40
        listed = [
            {
                "id": run_id,
                "name": "Package Release",
                "path": ".github/workflows/package-release.yml",
                "event": "workflow_dispatch",
                "conclusion": "failure",
                "head_branch": "main",
                "head_sha": sha,
            }
            for run_id, sha in ((301, "9" * 40), (302, "8" * 40), (303, tag_commit),
                                (304, tag_commit), (305, tag_commit))
        ]
        requested: list[str] = []

        def request(path: str) -> object:
            requested.append(path)
            run_id = int(path.rsplit("/", 1)[1])
            return dict(listed[run_id - 301], head_repository={"full_name": "atrinik/classic"})

        with (
            patch.object(
                resolve_pending_release, "list_workflow_runs", return_value=listed
            ),
            patch.object(
                resolve_pending_release,
                "is_ancestor",
                side_effect=lambda older, newer: tag_commit in (older, newer)
                and "9" * 40 not in (older, newer) and "8" * 40 not in (older, newer),
            ),
            patch.object(resolve_pending_release, "_collect_jobs", return_value=[]),
        ):
            self.assertEqual(
                resolve_pending_release.recent_failure_classes(
                    "atrinik/classic", "v5.8.0", tag_commit, "2" * 40, request
                ),
                ["unknown"] * 3,
            )
        self.assertEqual(
            requested,
            [f"repos/atrinik/classic/actions/runs/{run_id}" for run_id in (303, 304, 305)],
        )

    def test_repeated_server_image_failures_stop_automatic_retries(self) -> None:
        result = resolve_pending_release.resolve(
            [draft(id=10, tag_name="v5.8.0")],
            policy(),
            lambda tag: "0" * 40,
            lambda run, windows, image: None,
            failure_classes=lambda tag: [
                "server-image-build",
                "server-image-build",
                "server-image-build",
            ],
        )
        self.assertEqual(
            result,
            {
                "action": "blocked",
                "tag": "v5.8.0",
                "release_id": "10",
                "failure_class": "server-image-build",
                "retry_count": "3",
            },
        )

    def test_first_server_image_failure_remains_bounded_and_resumable(self) -> None:
        result = resolve_pending_release.resolve(
            [draft(id=10, tag_name="v5.8.0")],
            policy(),
            lambda tag: "0" * 40,
            lambda run, windows, image: None,
            failure_classes=lambda tag: ["server-image-build"],
        )
        self.assertEqual(result["action"], "resume")
        self.assertEqual(result["failure_class"], "server-image-build")
        self.assertEqual(result["retry_count"], "1")

    def test_third_consecutive_failure_of_any_class_stops_automatic_retries(self) -> None:
        def resolved(classes: list[str]) -> dict[str, str]:
            return resolve_pending_release.resolve(
                [draft(id=10, tag_name="v5.8.0")],
                policy(),
                lambda tag: "0" * 40,
                lambda run, windows, image: None,
                failure_classes=lambda tag: classes,
            )

        self.assertEqual(resolve_pending_release.MAX_AUTOMATIC_PACKAGE_ATTEMPTS, 3)
        self.assertEqual(
            resolved(["unknown", "windows-server-build"]),
            {
                "action": "resume",
                "tag": "v5.8.0",
                "release_id": "10",
                "failure_class": "unknown",
                "retry_count": "2",
            },
        )
        self.assertEqual(
            resolved(["unknown", "candidate-validation", "server-image-build"]),
            {
                "action": "blocked",
                "tag": "v5.8.0",
                "release_id": "10",
                "failure_class": "unknown",
                "retry_count": "3",
            },
        )

    def test_failure_inspection_stops_at_the_attempt_cap(self) -> None:
        listed = [
            {
                "id": run_id,
                "name": "Package Release",
                "path": ".github/workflows/package-release.yml",
                "event": "workflow_dispatch",
                "conclusion": "failure",
                "head_branch": "v5.8.0",
                "head_sha": "1" * 40,
            }
            for run_id in range(201, 206)
        ]
        requested: list[str] = []

        def request(path: str) -> object:
            requested.append(path)
            run_id = int(path.rsplit("/", 1)[1])
            return dict(listed[run_id - 201], head_repository={"full_name": "atrinik/classic"})

        with (
            patch.object(
                resolve_pending_release, "list_workflow_runs", return_value=listed
            ),
            patch.object(resolve_pending_release, "is_ancestor", return_value=True),
            patch.object(resolve_pending_release, "_collect_jobs", return_value=[]),
        ):
            self.assertEqual(
                resolve_pending_release.recent_failure_classes(
                    "atrinik/classic", "v5.8.0", "1" * 40, "2" * 40, request
                ),
                ["unknown"] * 3,
            )
        self.assertEqual(
            requested,
            [f"repos/atrinik/classic/actions/runs/{run_id}" for run_id in (201, 202, 203)],
        )

    def test_multiple_drafts_fail_closed(self) -> None:
        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "multiple draft releases",
        ):
            resolve_pending_release.resolve(
                [draft(), draft(id=RELEASE_ID + 1)],
                policy(),
                lambda tag: COMMIT,
                lambda run, windows, image: None,
            )

    def test_failed_runs_must_prove_no_candidate_or_publication(self) -> None:
        run = {
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_repository": {"full_name": "atrinik/classic"},
        }
        jobs = {
            "total_count": 4,
            "jobs": [
                {
                    "name": "Build and validate immutable candidate / Build Windows server package",
                    "conclusion": "failure",
                },
                {
                    "name": "Build and validate immutable candidate / "
                    "Build classic server image without publishing",
                    "conclusion": "failure",
                },
                {
                    "name": "Build and validate immutable candidate / "
                    "Validate complete release candidate",
                    "conclusion": "skipped",
                },
                {"name": "Publish unified release", "conclusion": "skipped"},
            ]
        }

        def request(path: str) -> object:
            return jobs if "/jobs?" in path else run

        resolve_pending_release.validate_failed_run(
            "atrinik/classic", RUN_IDS[0], "failure", "failure", request
        )
        jobs["jobs"][-1]["conclusion"] = "success"
        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "failed-candidate evidence",
        ):
            resolve_pending_release.validate_failed_run(
                "atrinik/classic", RUN_IDS[0], "failure", "failure", request
            )

    def test_retained_candidate_requires_publication_boundary_proof(self) -> None:
        run = {
            "id": 32048332566,
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_sha": "1" * 40,
            "head_branch": "v5.37.0",
            "head_repository": {"full_name": "atrinik/classic"},
        }
        jobs = {
            "total_count": 2,
            "jobs": [
                {
                    "name": "Build and validate immutable candidate / "
                    "Validate complete release candidate",
                    "conclusion": "success",
                },
                {
                    "name": "Publish unified release",
                    "conclusion": "failure",
                    "steps": retained_publication_steps(
                        resolve_pending_release.PUBLISH_STEP
                    ),
                },
            ],
        }
        artifacts = {
            "total_count": 1,
            "artifacts": [
                {
                    "name": "complete-release-candidate-v5.37.0",
                    "expired": False,
                    "size_in_bytes": 100,
                    "digest": "sha256:" + "2" * 64,
                }
            ],
        }

        def request(path: str) -> object:
            if "/jobs?" in path:
                return jobs
            if "/artifacts?" in path:
                return artifacts
            return run

        self.assertEqual(
            resolve_pending_release.validate_retained_candidate(
                "atrinik/classic", "v5.37.0", 32048332566, request
            ),
            retained_candidate(),
        )

        for head_branch in ("main", "8.3.x", None):
            with self.subTest(head_branch=head_branch):
                branch_run = dict(run, head_branch=head_branch)
                with self.assertRaisesRegex(
                    resolve_pending_release.CandidateNotSafe,
                    "did not run on release tag v5.37.0",
                ):
                    resolve_pending_release.validate_retained_candidate(
                        "atrinik/classic",
                        "v5.37.0",
                        32048332566,
                        lambda path: branch_run
                        if path.endswith("/runs/32048332566")
                        else request(path),
                    )

        jobs["jobs"][1]["steps"] = retained_publication_steps(
            resolve_pending_release.IMAGE_INSPECTION_STEP
        )
        with self.assertRaisesRegex(
            resolve_pending_release.CandidateNotSafe,
            "publication boundary",
        ):
            resolve_pending_release.validate_retained_candidate(
                "atrinik/classic", "v5.37.0", 32048332566, request
            )

    def test_retained_candidate_search_requires_the_exact_tag_commit(self) -> None:
        tag_commit = "1" * 40
        listed = [
            {
                "id": run_id,
                "name": "Package Release",
                "path": ".github/workflows/package-release.yml",
                "event": "workflow_dispatch",
                "conclusion": "failure",
            }
            for run_id in (101, 102)
        ]
        commits = {101: "3" * 40, 102: tag_commit}

        def validate(
            repository: str, tag: str, run_id: int, request: object
        ) -> dict[str, str]:
            return retained_candidate(
                tag=tag,
                candidate_run_id=str(run_id),
                run_commit=commits[run_id],
            )

        with (
            patch.object(
                resolve_pending_release, "list_workflow_runs", return_value=listed
            ),
            patch.object(
                resolve_pending_release,
                "validate_retained_candidate",
                side_effect=validate,
            ),
            patch.object(resolve_pending_release, "is_ancestor", return_value=True),
        ):
            # Run 101 descends from the tag but was built from a later commit.
            self.assertEqual(
                resolve_pending_release.find_retained_candidates(
                    "atrinik/classic", "v5.37.0", tag_commit, "4" * 40, object()
                ),
                [
                    retained_candidate(
                        candidate_run_id="102", run_commit=tag_commit
                    )
                ],
            )
            commits[102] = "3" * 40
            self.assertEqual(
                resolve_pending_release.find_retained_candidates(
                    "atrinik/classic", "v5.37.0", tag_commit, "4" * 40, object()
                ),
                [],
            )

    def test_retained_candidate_rejects_duplicate_boundary_jobs(self) -> None:
        run = {
            "id": 32048332566,
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_sha": "1" * 40,
            "head_branch": "v5.37.0",
            "head_repository": {"full_name": "atrinik/classic"},
        }
        jobs = {
            "total_count": 2,
            "jobs": [
                {
                    "name": resolve_pending_release.CANDIDATE_FINALIZER_JOB,
                    "conclusion": "success",
                },
                {
                    "name": resolve_pending_release.PUBLISH_JOB,
                    "conclusion": "failure",
                    "steps": retained_publication_steps(
                        resolve_pending_release.PUBLISH_STEP
                    ),
                },
            ],
        }
        artifacts = {
            "total_count": 1,
            "artifacts": [
                {
                    "name": "complete-release-candidate-v5.37.0",
                    "expired": False,
                    "size_in_bytes": 100,
                    "digest": "sha256:" + "2" * 64,
                }
            ],
        }

        def request(path: str) -> object:
            if "/jobs?" in path:
                return jobs
            if "/artifacts?" in path:
                return artifacts
            return run

        for duplicate_index in (0, 1):
            with self.subTest(duplicate_index=duplicate_index):
                jobs["jobs"].append(dict(jobs["jobs"][duplicate_index]))
                jobs["total_count"] = 3
                with self.assertRaisesRegex(
                    resolve_pending_release.CandidateNotSafe,
                    "one complete candidate and one failed publisher",
                ):
                    resolve_pending_release.validate_retained_candidate(
                        "atrinik/classic", "v5.37.0", 32048332566, request
                    )
                jobs["jobs"].pop()
                jobs["total_count"] = 2

    def test_retained_candidate_rejects_non_success_publisher_steps(self) -> None:
        run = {
            "id": 32048332566,
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_sha": "1" * 40,
            "head_branch": "v5.37.0",
            "head_repository": {"full_name": "atrinik/classic"},
        }
        steps = retained_publication_steps(resolve_pending_release.PUBLISH_STEP)
        steps[1]["conclusion"] = "cancelled"
        jobs = {
            "total_count": 2,
            "jobs": [
                {
                    "name": resolve_pending_release.CANDIDATE_FINALIZER_JOB,
                    "conclusion": "success",
                },
                {
                    "name": resolve_pending_release.PUBLISH_JOB,
                    "conclusion": "failure",
                    "steps": steps,
                },
            ],
        }
        artifacts = {
            "total_count": 1,
            "artifacts": [
                {
                    "name": "complete-release-candidate-v5.37.0",
                    "expired": False,
                    "size_in_bytes": 100,
                    "digest": "sha256:" + "2" * 64,
                }
            ],
        }

        def request(path: str) -> object:
            if "/jobs?" in path:
                return jobs
            if "/artifacts?" in path:
                return artifacts
            return run

        with self.assertRaisesRegex(
            resolve_pending_release.CandidateNotSafe,
            "publication boundary",
        ):
            resolve_pending_release.validate_retained_candidate(
                "atrinik/classic", "v5.37.0", 32048332566, request
            )

    def test_failed_run_classification_exposes_image_boundary(self) -> None:
        self.assertEqual(
            resolve_pending_release.classify_failed_run(
                [
                    {"name": resolve_pending_release.IMAGE_JOB, "conclusion": "failure"}
                ]
            ),
            "server-image-build",
        )
        self.assertEqual(
            resolve_pending_release.classify_failed_run(
                [
                    {
                        "name": resolve_pending_release.PUBLISH_JOB,
                        "conclusion": "failure",
                        "steps": [
                            {
                                "name": resolve_pending_release.IMAGE_INSPECTION_STEP,
                                "conclusion": "failure",
                            }
                        ],
                    }
                ]
            ),
            "server-image-inspection",
        )

    def test_recent_tag_runs_are_classified_from_the_failed_job(self) -> None:
        runs = {
            "total_count": 3,
            "workflow_runs": [
                {
                    "id": run_id,
                    "name": "Package Release",
                    "path": ".github/workflows/package-release.yml",
                    "event": "workflow_dispatch",
                    "conclusion": "failure",
                    "head_branch": "v5.8.0",
                    "head_sha": "1" * 40,
                }
                for run_id in (3, 2, 1)
            ],
        }
        run = {
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_sha": "1" * 40,
            "head_repository": {"full_name": "atrinik/classic"},
        }
        jobs = {
            "total_count": 1,
            "jobs": [{"name": resolve_pending_release.IMAGE_JOB, "conclusion": "failure"}],
        }

        def request(path: str) -> object:
            if "/actions/workflows/" in path:
                return runs
            if "/jobs?" in path:
                return jobs
            return run

        with patch.object(resolve_pending_release, "is_ancestor", return_value=True):
            self.assertEqual(
                resolve_pending_release.recent_failure_classes(
                    "atrinik/classic", "v5.8.0", "1" * 40, "1" * 40, request
                ),
                ["server-image-build", "server-image-build", "server-image-build"],
            )

    def test_recent_main_recovery_runs_are_classified_on_tag_lineage(self) -> None:
        runs = {
            "total_count": 1,
            "workflow_runs": [
                {
                    "id": 2,
                    "name": "Package Release",
                    "path": ".github/workflows/package-release.yml",
                    "event": "workflow_dispatch",
                    "conclusion": "failure",
                    "head_branch": "main",
                    "head_sha": "1" * 40,
                }
            ],
        }
        run = {
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_sha": "1" * 40,
            "head_repository": {"full_name": "atrinik/classic"},
        }
        jobs = {
            "total_count": 1,
            "jobs": [{"name": resolve_pending_release.IMAGE_JOB, "conclusion": "failure"}],
        }

        def request(path: str) -> object:
            if "/actions/workflows/" in path:
                return runs
            if "/jobs?" in path:
                return jobs
            return run

        with patch.object(resolve_pending_release, "is_ancestor", return_value=True):
            self.assertEqual(
                resolve_pending_release.recent_failure_classes(
                    "atrinik/classic", "v5.8.0", "0" * 40, "1" * 40, request
                ),
                ["server-image-build"],
            )

    def test_duplicate_failed_run_jobs_are_ambiguous(self) -> None:
        run = {
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_repository": {"full_name": "atrinik/classic"},
        }
        names = (
            "Build and validate immutable candidate / Build Windows server package",
            "Build and validate immutable candidate / "
            "Build classic server image without publishing",
            "Build and validate immutable candidate / Validate complete release candidate",
            "Publish unified release",
        )
        conclusions = ("failure", "failure", "skipped", "skipped")
        job_list = [
            {"name": name, "conclusion": conclusion}
            for name, conclusion in zip(names, conclusions, strict=True)
        ]
        job_list.append(dict(job_list[0]))

        def request(path: str) -> object:
            return (
                {"total_count": len(job_list), "jobs": job_list}
                if "/jobs?" in path
                else run
            )

        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "failed-candidate evidence",
        ):
            resolve_pending_release.validate_failed_run(
                "atrinik/classic", RUN_IDS[0], "failure", "failure", request
            )

    def test_failed_run_accepts_policy_listed_successful_server_image(self) -> None:
        run = {
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_repository": {"full_name": "atrinik/classic"},
        }
        jobs = {
            "total_count": 4,
            "jobs": [
                {
                    "name": "Build and validate immutable candidate / Build Windows server package",
                    "conclusion": "failure",
                },
                {
                    "name": "Build and validate immutable candidate / "
                    "Build classic server image without publishing",
                    "conclusion": "success",
                },
                {
                    "name": "Build and validate immutable candidate / "
                    "Validate complete release candidate",
                    "conclusion": "skipped",
                },
                {"name": "Publish unified release", "conclusion": "skipped"},
            ]
        }

        def request(path: str) -> object:
            return jobs if "/jobs?" in path else run

        resolve_pending_release.validate_failed_run(
            "atrinik/classic", 31429488922, "failure", "success", request
        )

    def test_failed_run_accepts_policy_listed_successful_windows_server(self) -> None:
        run = {
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_repository": {"full_name": "atrinik/classic"},
        }
        jobs = {
            "total_count": 4,
            "jobs": [
                {
                    "name": "Build and validate immutable candidate / Build Windows server package",
                    "conclusion": "success",
                },
                {
                    "name": "Build and validate immutable candidate / "
                    "Build classic server image without publishing",
                    "conclusion": "failure",
                },
                {
                    "name": "Build and validate immutable candidate / "
                    "Validate complete release candidate",
                    "conclusion": "skipped",
                },
                {"name": "Publish unified release", "conclusion": "skipped"},
            ]
        }

        def request(path: str) -> object:
            return jobs if "/jobs?" in path else run

        resolve_pending_release.validate_failed_run(
            "atrinik/classic", 31935172521, "success", "failure", request
        )

    def test_failed_run_inspects_every_job_page(self) -> None:
        run = {
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_repository": {"full_name": "atrinik/classic"},
        }
        required = [
            {
                "name": "Build and validate immutable candidate / "
                "Build Windows server package",
                "conclusion": "failure",
            },
            {
                "name": "Build and validate immutable candidate / "
                "Build classic server image without publishing",
                "conclusion": "failure",
            },
            {
                "name": "Build and validate immutable candidate / "
                "Validate complete release candidate",
                "conclusion": "skipped",
            },
            {"name": "Publish unified release", "conclusion": "skipped"},
        ]
        first_page = required + [
            {"name": f"Unrelated job {index}", "conclusion": "success"}
            for index in range(96)
        ]
        second_page = [{"name": "Final unrelated job", "conclusion": "success"}]

        def request(path: str) -> object:
            if "/jobs?" not in path:
                return run
            return {
                "total_count": 101,
                "jobs": second_page if path.endswith("page=2") else first_page,
            }

        resolve_pending_release.validate_failed_run(
            "atrinik/classic", RUN_IDS[0], "failure", "failure", request
        )
        second_page[0] = dict(required[0])
        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "failed-candidate evidence",
        ):
            resolve_pending_release.validate_failed_run(
                "atrinik/classic", RUN_IDS[0], "failure", "failure", request
            )

    def test_guarded_deletion_rechecks_exact_release(self) -> None:
        expected = {
            "action": "delete-empty-draft",
            "tag": TAG,
            "release_id": str(RELEASE_ID),
        }
        deleted: list[int] = []
        release = draft()

        resolve_pending_release.delete_policy_listed_empty_draft(
            "atrinik/classic",
            TAG,
            RELEASE_ID,
            lambda: expected,
            lambda path: release,
            deleted.append,
        )
        self.assertEqual(deleted, [RELEASE_ID])

        deleted.clear()
        release["assets"] = [{"name": "concurrent.zip"}]
        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "changed before deletion",
        ):
            resolve_pending_release.delete_policy_listed_empty_draft(
                "atrinik/classic",
                TAG,
                RELEASE_ID,
                lambda: expected,
                lambda path: release,
                deleted.append,
            )
        self.assertEqual(deleted, [])

        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "changed before deletion",
        ):
            resolve_pending_release.delete_policy_listed_empty_draft(
                "atrinik/classic",
                TAG,
                RELEASE_ID,
                lambda: {"action": "none", "tag": "", "release_id": ""},
                lambda path: draft(),
                deleted.append,
            )
        self.assertEqual(deleted, [])


class ExactRetirementGuardTests(unittest.TestCase):
    tag = "v5.80.0"
    commit = "24bee6c8a830aa7372a30be6b842210b07639f73"
    current = "c" * 40
    release_id = 409250858
    run_id = 38092568098

    def setUp(self) -> None:
        self.policy = json.loads(resolve_pending_release.POLICY.read_text())["failed_releases"]
        self.disposition = self.policy[self.tag]
        self.run = {
            "id": self.run_id, "name": "Package Release",
            "path": ".github/workflows/package-release.yml", "event": "workflow_dispatch",
            "status": "completed", "conclusion": "failure", "run_attempt": 1,
            "head_sha": self.commit, "head_branch": self.tag,
            "repository": {"full_name": "atrinik/classic"},
            "head_repository": {"full_name": "atrinik/classic"},
            "created_at": "2026-10-10T22:44:52Z",
        }
        self.runs = [copy.deepcopy(self.run)]
        self.jobs = [
            {"name": resolve_pending_release.WINDOWS_JOB, "conclusion": "success"},
            {"name": resolve_pending_release.IMAGE_JOB, "conclusion": "failure"},
            {"name": resolve_pending_release.CANDIDATE_FINALIZER_JOB, "conclusion": "skipped"},
            {"name": resolve_pending_release.PUBLISH_JOB, "conclusion": "skipped"},
        ]
        self.artifacts = [{"name": "release-client-linux-v5.80.0"}]
        self.versions = []
        self.requests = []
        self.inventory_override = None
        self.release = draft(id=self.release_id, tag_name=self.tag)
        self.ref = {"ref": "refs/heads/main", "object": {"type": "commit", "sha": self.current}}

    def request(self, path):
        self.requests.append(path)
        if "/jobs?" in path:
            return {"total_count": len(self.jobs), "jobs": self.jobs}
        if "/artifacts?" in path:
            return {"total_count": len(self.artifacts), "artifacts": self.artifacts}
        if "/actions/workflows/" in path:
            self.assertNotIn("status=", path)
            filters = parse_qs(urlsplit(path).query)
            selected = self.runs
            if "branch" in filters:
                self.assertNotIn("created", filters)
                selected = [run for run in self.runs if run.get("head_branch") == filters["branch"][0]]
            elif "head_sha" in filters:
                self.assertNotIn("created", filters)
                selected = [run for run in self.runs if run.get("head_sha") == filters["head_sha"][0]]
            else:
                self.assertEqual(filters.get("created"), [">=2026-10-10T22:44:52Z"])
                selected = [run for run in self.runs if run.get("created_at", "") >= "2026-10-10T22:44:52Z"]
            return self.inventory_override or {"total_count": len(selected), "workflow_runs": selected}
        if "/actions/runs/" in path:
            self.assertTrue(path.endswith(str(self.run_id)))
            return self.run
        if "/packages/" in path:
            return self.versions
        if path.endswith("git/ref/heads/main"):
            return self.ref
        if path.endswith(f"releases/{self.release_id}"):
            return self.release
        raise AssertionError(path)

    def guard(self):
        with patch.object(resolve_pending_release, "is_ancestor", return_value=True):
            resolve_pending_release.validate_retirement_guard(
                "atrinik/classic", self.tag, self.current, self.disposition, self.request)

    def test_incident_policy_selects_only_the_exact_unpublished_failure(self):
        checked = []
        decision = resolve_pending_release.resolve(
            [self.release], self.policy, lambda tag: self.commit,
            lambda run, windows, image: checked.append((run, windows, image)))
        self.assertEqual(decision, {"action": "delete-empty-draft", "tag": self.tag,
                                    "release_id": str(self.release_id)})
        self.assertEqual(checked, [(self.run_id, "success", "failure")])
        self.guard()
        self.assertTrue(any("/packages/" in path for path in self.requests))
        self.assertTrue(self.requests[-1].endswith("git/ref/heads/main"))

    def test_incident_rejects_changed_draft_source_assets_and_multiple_drafts(self):
        cases = (([draft(id=self.release_id + 1, tag_name=self.tag)], self.commit),
                 ([draft(id=self.release_id, tag_name=self.tag, assets=[{"name": "partial.deb"}])], self.commit),
                 ([draft(id=self.release_id, tag_name=self.tag, draft=False)], self.commit),
                 ([self.release], "b" * 40), ([self.release, draft()], self.commit))
        for releases, source in cases:
            with self.subTest(releases=releases, source=source), self.assertRaises(
                    resolve_pending_release.PendingReleaseError):
                resolve_pending_release.resolve(releases, self.policy, lambda tag: source,
                                                lambda run, windows, image: None)

    def test_any_additional_queued_failed_successful_or_main_lineage_run_stops_retirement(self):
        for branch, status, conclusion in ((self.tag, "queued", None),
                (self.tag, "completed", "failure"), (self.tag, "completed", "success"),
                ("main", "in_progress", None), ("v5.81.0", "completed", "success")):
            self.runs = [copy.deepcopy(self.run), self.run | {
                "id": self.run_id + 1, "head_branch": branch, "head_sha": self.current,
                "status": status, "conclusion": conclusion}]
            with self.subTest(branch=branch, status=status), self.assertRaisesRegex(
                    resolve_pending_release.PendingReleaseError, "additional or changed"):
                self.guard()

    def test_earlier_tag_or_source_runs_cannot_hide_outside_lineage_time_window(self):
        for branch, source, status, conclusion in (
                (self.tag, self.current, "completed", "success"),
                (self.tag, self.commit, "in_progress", None),
                ("main", self.commit, "completed", "failure")):
            self.runs = [copy.deepcopy(self.run), self.run | {
                "id": self.run_id - 1, "head_branch": branch, "head_sha": source,
                "status": status, "conclusion": conclusion, "created_at": "2026-10-09T12:00:00Z"}]
            with self.subTest(branch=branch, status=status), self.assertRaisesRegex(
                    resolve_pending_release.PendingReleaseError, "additional or changed"):
                self.guard()

    def test_each_exact_query_requires_its_expected_runs_and_correct_filter(self):
        original = self.request
        for selector in ("branch=", "head_sha="):
            for response in ({"total_count": 0, "workflow_runs": []},
                    {"total_count": 1, "workflow_runs": [self.run | {
                        "head_branch": "feature", "head_sha": self.current}]}):
                def request(path):
                    return response if selector in path else original(path)
                with self.subTest(selector=selector, response=response), patch.object(
                        resolve_pending_release, "is_ancestor", return_value=True), self.assertRaises(
                        resolve_pending_release.PendingReleaseError):
                    resolve_pending_release.validate_retirement_guard(
                        "atrinik/classic", self.tag, self.current, self.disposition, request)

    def test_run_attempt_source_status_or_repository_drift_fails(self):
        original = copy.deepcopy(self.run)
        for changes in ({"run_attempt": 2}, {"run_attempt": True}, {"head_sha": self.current},
                {"status": "queued"}, {"conclusion": "success"}, {"head_branch": "feature"},
                {"repository": {"full_name": "fork/classic"}},
                {"head_repository": {"full_name": "fork/classic"}}, {"created_at": "invalid"}):
            self.run = original | changes
            with self.subTest(changes=changes), self.assertRaises(resolve_pending_release.PendingReleaseError):
                self.guard()

    def test_successful_candidate_publication_or_changed_build_jobs_fail(self):
        original = copy.deepcopy(self.jobs)
        for index, conclusion in ((0, "failure"), (1, "success"), (2, "success"), (3, "success")):
            self.jobs = copy.deepcopy(original)
            self.jobs[index]["conclusion"] = conclusion
            with self.subTest(index=index), self.assertRaisesRegex(
                    resolve_pending_release.PendingReleaseError, "failed-candidate evidence"):
                self.guard()

    def test_even_expired_complete_candidate_artifacts_block_retirement(self):
        for expired in (True, False):
            self.artifacts = [{"name": "complete-release-candidate-v5.80.0", "expired": expired}]
            with self.subTest(expired=expired), self.assertRaisesRegex(
                    resolve_pending_release.PendingReleaseError, "complete release candidate"):
                self.guard()

    def test_incomplete_oversize_missing_duplicate_or_untrusted_run_inventory_fails(self):
        for response in ({"total_count": 101, "workflow_runs": [self.run]},
                {"total_count": 2, "workflow_runs": [self.run]},
                {"total_count": 0, "workflow_runs": []},
                {"total_count": 2, "workflow_runs": [self.run, self.run]},
                {"total_count": 1, "workflow_runs": [self.run | {"event": "pull_request"}]},
                {"total_count": 1, "workflow_runs": [self.run | {"status": "queued"}]}):
            self.inventory_override = response
            with self.subTest(response=response), self.assertRaises(resolve_pending_release.PendingReleaseError):
                self.guard()

    def test_registry_existing_image_malformed_or_exhausted_inventory_fails(self):
        version = {"name": "sha256:" + "a" * 64,
                   "metadata": {"container": {"tags": ["5.80.0"]}}}
        for versions in ([version], {"message": "Forbidden"}, [{"name": "invalid"}],
                         [version | {"metadata": {"container": {"tags": []}}}] * 100):
            self.versions = versions
            with self.subTest(versions_type=type(versions), length=len(versions)), self.assertRaises(
                    resolve_pending_release.PendingReleaseError):
                self.guard()
        pages = [path for path in self.requests if "/packages/" in path]
        self.assertLessEqual(len(pages), resolve_pending_release.MAX_API_PAGES + 3)

    def test_registry_permissions_failure_is_not_absence(self):
        request = self.request
        def unavailable(path):
            if "/packages/" in path:
                raise resolve_pending_release.PendingReleaseError("HTTP 403 missing packages permission")
            return request(path)
        with patch.object(resolve_pending_release, "is_ancestor", return_value=True), self.assertRaisesRegex(
                resolve_pending_release.PendingReleaseError, "HTTP 403"):
            resolve_pending_release.validate_retirement_guard(
                "atrinik/classic", self.tag, self.current, self.disposition, unavailable)

    def test_changed_main_or_guard_policy_fails(self):
        self.ref["object"]["sha"] = "b" * 40
        with self.assertRaisesRegex(resolve_pending_release.PendingReleaseError, "main changed"):
            self.guard()
        self.ref["object"]["sha"] = self.current
        self.disposition = self.disposition | {"retirement_guard": {"package_run_inventory": "unknown"}}
        with self.assertRaisesRegex(resolve_pending_release.PendingReleaseError, "invalid exact"):
            self.guard()

    def test_repository_resolution_runs_guard_and_requires_main(self):
        with patch.object(resolve_pending_release, "command", return_value=self.current), patch.object(
                resolve_pending_release, "list_drafts", return_value=[self.release]), patch.object(
                resolve_pending_release, "resolve", return_value={"action": "delete-empty-draft",
                    "tag": self.tag, "release_id": str(self.release_id)}), patch.object(
                resolve_pending_release, "api", side_effect=self.request), patch.object(
                resolve_pending_release, "is_ancestor", return_value=True):
            self.assertEqual(resolve_pending_release.resolve_repository("atrinik/classic")["action"],
                             "delete-empty-draft")
            with self.assertRaisesRegex(resolve_pending_release.PendingReleaseError, "requires current main"):
                resolve_pending_release.resolve_repository("atrinik/classic", "5.80.x")

    def test_guarded_delete_repeats_inventory_and_never_deletes_after_new_evidence(self):
        decision = {"action": "delete-empty-draft", "tag": self.tag, "release_id": str(self.release_id)}
        self.guard()  # Initial disposition was safe, but it is not cached authority.
        self.runs.append(self.run | {"id": self.run_id + 1, "status": "queued", "conclusion": None})
        deleted = []
        def resolve_now():
            self.guard()
            return decision
        with self.assertRaises(resolve_pending_release.PendingReleaseError):
            resolve_pending_release.delete_policy_listed_empty_draft(
                "atrinik/classic", self.tag, self.release_id, resolve_now, self.request, deleted.append)
        self.assertEqual(deleted, [])


if __name__ == "__main__":
    unittest.main()
