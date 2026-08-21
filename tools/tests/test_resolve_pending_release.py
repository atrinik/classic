from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


RELEASE_TOOLS = Path(__file__).resolve().parents[1] / "release"
sys.path.insert(0, str(RELEASE_TOOLS))
MODULE_PATH = RELEASE_TOOLS / "resolve_pending_release.py"
SPEC = importlib.util.spec_from_file_location("resolve_pending_release", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
resolve_pending_release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(resolve_pending_release)


TAG = "v5.8.1"
COMMIT = "4653cb0a5f8bb11f5f3b522008bdd28c39d8c14c"
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


class ResolvePendingReleaseTests(unittest.TestCase):
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

    def test_retained_candidate_rejects_duplicate_boundary_jobs(self) -> None:
        run = {
            "id": 32048332566,
            "name": "Package Release",
            "path": ".github/workflows/package-release.yml",
            "event": "workflow_dispatch",
            "conclusion": "failure",
            "head_sha": "1" * 40,
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


if __name__ == "__main__":
    unittest.main()
