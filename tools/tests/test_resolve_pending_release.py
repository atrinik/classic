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
        }
    }


class ResolvePendingReleaseTests(unittest.TestCase):
    def test_no_draft_allows_semantic_release(self) -> None:
        self.assertEqual(
            resolve_pending_release.resolve([], policy(), lambda tag: COMMIT, lambda run: None),
            {"action": "none", "tag": "", "release_id": ""},
        )

    def test_ordinary_empty_draft_is_resumed(self) -> None:
        release = draft(id=10, tag_name="v5.8.0")
        self.assertEqual(
            resolve_pending_release.resolve(
                [release], policy(), lambda tag: "0" * 40, lambda run: None
            ),
            {"action": "resume", "tag": "v5.8.0", "release_id": "10"},
        )

    def test_recorded_empty_failed_draft_is_deleted(self) -> None:
        validated: list[int] = []
        self.assertEqual(
            resolve_pending_release.resolve(
                [draft()], policy(), lambda tag: COMMIT, validated.append
            ),
            {
                "action": "delete-empty-draft",
                "tag": TAG,
                "release_id": str(RELEASE_ID),
            },
        )
        self.assertEqual(validated, RUN_IDS)

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
                        [release], failed_policy, lambda tag: commit, lambda run: None
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
                lambda run: None,
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
                lambda run: None,
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
            "jobs": [
                {
                    "name": "Build and validate immutable candidate / Build Windows server package",
                    "conclusion": "failure",
                },
                {
                    "name": "Build and validate immutable candidate / Build classic server image without publishing",
                    "conclusion": "failure",
                },
                {
                    "name": "Build and validate immutable candidate / Validate complete release candidate",
                    "conclusion": "skipped",
                },
                {"name": "Publish unified release", "conclusion": "skipped"},
            ]
        }

        def request(path: str) -> object:
            return jobs if "/jobs?" in path else run

        resolve_pending_release.validate_failed_run(
            "atrinik/classic", RUN_IDS[0], request
        )
        jobs["jobs"][-1]["conclusion"] = "success"
        with self.assertRaisesRegex(
            resolve_pending_release.PendingReleaseError,
            "failed-candidate evidence",
        ):
            resolve_pending_release.validate_failed_run(
                "atrinik/classic", RUN_IDS[0], request
            )


if __name__ == "__main__":
    unittest.main()
