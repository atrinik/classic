from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


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
            "server_image_conclusion": "failure",
        }
    }


class ResolvePendingReleaseTests(unittest.TestCase):
    def test_no_draft_allows_semantic_release(self) -> None:
        self.assertEqual(
            resolve_pending_release.resolve(
                [], policy(), lambda tag: COMMIT, lambda run, image: None
            ),
            {"action": "none", "tag": "", "release_id": ""},
        )

    def test_ordinary_empty_draft_is_resumed(self) -> None:
        release = draft(id=10, tag_name="v5.8.0")
        self.assertEqual(
            resolve_pending_release.resolve(
                [release], policy(), lambda tag: "0" * 40, lambda run, image: None
            ),
            {"action": "resume", "tag": "v5.8.0", "release_id": "10"},
        )

    def test_recorded_empty_failed_draft_is_deleted(self) -> None:
        validated: list[tuple[int, str]] = []
        self.assertEqual(
            resolve_pending_release.resolve(
                [draft()],
                policy(),
                lambda tag: COMMIT,
                lambda run, image: validated.append((run, image)),
            ),
            {
                "action": "delete-empty-draft",
                "tag": TAG,
                "release_id": str(RELEASE_ID),
            },
        )
        self.assertEqual(validated, [(run, "failure") for run in RUN_IDS])

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
                        lambda run, image: None,
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
                lambda run, image: None,
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
                lambda run, image: None,
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
                lambda run, image: None,
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
            "atrinik/classic", RUN_IDS[0], "failure", request
        )
        jobs["jobs"][-1]["conclusion"] = "success"
        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "failed-candidate evidence",
        ):
            resolve_pending_release.validate_failed_run(
                "atrinik/classic", RUN_IDS[0], "failure", request
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
                "atrinik/classic", RUN_IDS[0], "failure", request
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
            "atrinik/classic", 31429488922, "success", request
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
            "atrinik/classic", RUN_IDS[0], "failure", request
        )
        second_page[0] = dict(required[0])
        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "failed-candidate evidence",
        ):
            resolve_pending_release.validate_failed_run(
                "atrinik/classic", RUN_IDS[0], "failure", request
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
