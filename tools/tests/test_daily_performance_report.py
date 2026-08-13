from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("daily_report", ROOT / "ci" / "daily_performance_report.py")
assert SPEC and SPEC.loader
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)

CLIENT_TOOLS = ROOT.parent / "client" / "tools"
sys.path.insert(0, str(CLIENT_TOOLS))
FIXTURE_SPEC = importlib.util.spec_from_file_location(
    "daily_report_movement_fixtures",
    CLIENT_TOOLS / "tests" / "test_benchmark_movement_regression.py",
)
assert FIXTURE_SPEC and FIXTURE_SPEC.loader
fixtures = importlib.util.module_from_spec(FIXTURE_SPEC)
FIXTURE_SPEC.loader.exec_module(fixtures)


def evidence() -> dict[str, object]:
    return fixtures.benchmark._build_evidence(
        [],
        [fixtures.native_record(), fixtures.native_record()],
        [
            fixtures.native_record(viewport="large"),
            fixtures.native_record(viewport="large"),
        ],
        fixtures.additional_contexts(full=True),
        enforce_performance=False,
        comparison_note="event-has-no-comparison-base",
    )


class DailyReportTests(unittest.TestCase):
    def test_point_contains_all_phases_and_cohort(self) -> None:
        point = report.build_point(evidence(), commit="a" * 40, run_id="7",
                                   recorded_at="2026-08-13T00:00:00+00:00", environment={})
        self.assertEqual(set(point["phases"]), set(report.PHASES))
        self.assertEqual(len(point["cohort"]), 16)

    def test_merge_is_idempotent_and_retains_only_latest_points(self) -> None:
        point = report.build_point(evidence(), commit="a" * 40, run_id="7",
                                   recorded_at="2026-08-13T00:00:00+00:00", environment={})
        trend = None
        for index in range(report.TREND_RETENTION + 2):
            item = dict(point, id=f"p{index}", recorded_at=f"2026-08-{(index % 9) + 1:02d}T00:00:00+00:00")
            trend = report.merge_trend(trend, item)
        points = trend["cohorts"][point["cohort"]]
        self.assertEqual(len(points), report.TREND_RETENTION)
        self.assertEqual(report.merge_trend(trend, points[-1])["cohorts"][point["cohort"]], points)

    def test_failed_evidence_is_not_published(self) -> None:
        bad = evidence()
        bad["status"] = "error"
        with self.assertRaises(report.ReportError):
            report.validate_evidence(bad)

    def test_incomplete_full_matrix_is_not_published(self) -> None:
        bad = evidence()
        bad["samples"]["candidate_large"] = 0
        bad["records"]["candidate_large"] = []
        with self.assertRaises(report.ReportError):
            report.validate_evidence(bad)

    def test_mismatched_revision_is_not_published(self) -> None:
        with self.assertRaisesRegex(report.ReportError, "published commit"):
            report.build_point(
                evidence(),
                commit="b" * 40,
                run_id="7",
                recorded_at="2026-08-13T00:00:00+00:00",
                environment={},
            )

    def test_two_failed_points_open_one_alert_and_two_passes_recover_it(self) -> None:
        item = evidence()
        point = report.build_point(item, commit="a" * 40, run_id="7",
                                   recorded_at="2026-08-13T00:00:00+00:00", environment={})
        point["checks"] = {"candidate_sustained_p95": {"passed": False}}
        trend = report.merge_trend(None, point)
        for index, passed in enumerate((False, True, True), 2):
            next_point = dict(
                point,
                id=f"run-{index}",
                run_id=str(index),
                recorded_at=f"2026-08-{index:02d}T00:00:00+00:00",
                checks={"candidate_sustained_p95": {"passed": passed}},
            )
            trend = report.merge_trend(trend, next_point)
        state = next(iter(trend["alerts"].values()))
        self.assertFalse(state["active"])
        self.assertEqual(state["last_transition"], "recovered")

    def test_large_context_and_summary_links_are_retained(self) -> None:
        item = evidence()
        point = report.build_point(item, commit="a" * 40, run_id="7",
                                   recorded_at="2026-08-13T00:00:00+00:00",
                                   environment={"workflow_url": "https://example.test/run"})
        trend = report.merge_trend(None, point)
        summary = report.render_summary(point, trend)
        self.assertIn("Large viewport", summary)
        self.assertIn("Workflow run", summary)
        self.assertIn("Lighting translated/partial rebuild: `2399` / `2399`", summary)
        self.assertIn("Lighting dirty pixels: `2456576`", summary)

    def test_failed_complete_evidence_is_retained_for_alerting(self) -> None:
        item = evidence()
        for record in item["records"]["candidate_standard"]:
            record["phases"][1]["queue"]["peak_depth"] = 2
        item = fixtures.benchmark._build_evidence(
            [],
            item["records"]["candidate_standard"],
            item["records"]["candidate_large"],
            item["records"]["additional_contexts"],
            enforce_performance=False,
            comparison_note="event-has-no-comparison-base",
        )
        point = report.build_point(item, commit="a" * 40, run_id="7",
                                   recorded_at="2026-08-13T00:00:00+00:00", environment={})
        self.assertEqual(point["status"], "failed")

    def test_cross_day_rerun_replaces_the_same_observation(self) -> None:
        first = report.build_point(
            evidence(), commit="a" * 40, run_id="7",
            recorded_at="2026-08-13T23:59:00+00:00", environment={}
        )
        second = report.build_point(
            evidence(), commit="a" * 40, run_id="7",
            recorded_at="2026-08-14T00:01:00+00:00", environment={}
        )
        trend = report.merge_trend(report.merge_trend(None, first), second)
        self.assertEqual(len(trend["cohorts"][first["cohort"]]), 1)
        for state in trend["alerts"].values():
            self.assertEqual(len(state["history"]), 1)

    def test_rerun_replaces_observation_after_cohort_change(self) -> None:
        first = report.build_point(
            evidence(), commit="a" * 40, run_id="7",
            recorded_at="2026-08-13T23:59:00+00:00", environment={}
        )
        second = dict(
            first,
            cohort="new-cohort",
            recorded_at="2026-08-14T00:01:00+00:00",
        )
        trend = report.merge_trend(report.merge_trend(None, first), second)
        self.assertEqual(trend["cohorts"][first["cohort"]], [])
        self.assertEqual(len(trend["cohorts"][second["cohort"]]), 1)
        histories = [state["history"] for state in trend["alerts"].values()]
        self.assertEqual(sum(len(history) for history in histories), 2)

    def test_new_cohort_clears_old_alert_transition(self) -> None:
        first = report.build_point(
            evidence(), commit="a" * 40, run_id="7",
            recorded_at="2026-08-13T00:00:00+00:00", environment={}
        )
        first["checks"] = {"candidate_sustained_p95": {"passed": False}}
        second = dict(first, id="run-8", run_id="8", recorded_at="2026-08-14T00:00:00+00:00")
        trend = report.merge_trend(report.merge_trend(None, first), second)
        old_key = next(iter(trend["alerts"]))
        self.assertEqual(trend["alerts"][old_key]["last_transition"], "regressed")

        other = dict(first, id="run-9", run_id="9", cohort="other-cohort",
                     recorded_at="2026-08-15T00:00:00+00:00")
        other["checks"] = {"candidate_sustained_p95": {"passed": True}}
        trend = report.merge_trend(trend, other)
        self.assertEqual(trend["alerts"][old_key]["last_transition"], "none")


if __name__ == "__main__":
    unittest.main()
