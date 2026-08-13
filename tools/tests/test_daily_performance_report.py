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
        sustained_map = point["phases"]["sustained"]["map"]
        self.assertEqual(
            sustained_map["map_draws"],
            sustained_map["primary_map_draws"] + sustained_map["auxiliary_map_draws"],
        )

    def test_merge_is_idempotent_and_retains_only_latest_points(self) -> None:
        point = report.build_point(evidence(), commit="a" * 40, run_id="7",
                                   recorded_at="2026-08-13T00:00:00+00:00", environment={})
        trend = None
        for index in range(report.TREND_RETENTION + 2):
            item = dict(
                point,
                id=f"run-{index + 1}",
                run_id=str(index + 1),
                recorded_at=f"2026-08-{(index % 9) + 1:02d}T00:00:00+00:00",
            )
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

    def test_incompatible_context_identity_is_not_published(self) -> None:
        item = evidence()
        for record in item["records"]["candidate_large"]:
            record["identity"]["implementation"]["compiler_version"] = "16.0.0"
        item = fixtures.benchmark._build_evidence(
            [],
            item["records"]["candidate_standard"],
            item["records"]["candidate_large"],
            item["records"]["additional_contexts"],
            enforce_performance=False,
            comparison_note="event-has-no-comparison-base",
        )
        with self.assertRaisesRegex(report.ReportError, "incompatible implementation"):
            report.build_point(
                item, commit="a" * 40, run_id="7",
                recorded_at="2026-08-13T00:00:00+00:00", environment={}
            )

    def test_every_context_identity_contributes_to_the_cohort(self) -> None:
        original = report.build_point(
            evidence(), commit="a" * 40, run_id="7",
            recorded_at="2026-08-13T00:00:00+00:00", environment={}
        )
        changed_fixture = evidence()
        for record in changed_fixture["records"]["additional_contexts"]["standard_discrete"]:
            record["fixture"]["manifest_sha256"] = "f" * 64
        changed_fixture = fixtures.benchmark._build_evidence(
            [], changed_fixture["records"]["candidate_standard"],
            changed_fixture["records"]["candidate_large"],
            changed_fixture["records"]["additional_contexts"],
            enforce_performance=False,
            comparison_note="event-has-no-comparison-base",
        )
        fixture_point = report.build_point(
            changed_fixture, commit="a" * 40, run_id="8",
            recorded_at="2026-08-14T00:00:00+00:00", environment={}
        )
        changed_runner = evidence()
        for record in changed_runner["records"]["candidate_large"]:
            record["identity"]["run"]["runner_image_version"] = "different"
        changed_runner = fixtures.benchmark._build_evidence(
            [], changed_runner["records"]["candidate_standard"],
            changed_runner["records"]["candidate_large"],
            changed_runner["records"]["additional_contexts"],
            enforce_performance=False,
            comparison_note="event-has-no-comparison-base",
        )
        runner_point = report.build_point(
            changed_runner, commit="a" * 40, run_id="9",
            recorded_at="2026-08-15T00:00:00+00:00", environment={}
        )
        self.assertNotEqual(original["cohort"], fixture_point["cohort"])
        self.assertNotEqual(original["cohort"], runner_point["cohort"])

    def test_two_failed_points_open_one_alert_and_two_passes_recover_it(self) -> None:
        item = evidence()
        point = report.build_point(item, commit="a" * 40, run_id="1",
                                   recorded_at="2026-08-01T00:00:00+00:00", environment={})
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

    def test_result_changing_rerun_recomputes_active_alert(self) -> None:
        first = report.build_point(
            evidence(), commit="a" * 40, run_id="7",
            recorded_at="2026-08-13T00:00:00+00:00", environment={}
        )
        first["checks"] = {"candidate_sustained_p95": {"passed": False}}
        second = dict(first, id="run-8", run_id="8", recorded_at="2026-08-14T00:00:00+00:00")
        trend = report.merge_trend(report.merge_trend(None, first), second)
        key = next(iter(trend["alerts"]))
        self.assertTrue(trend["alerts"][key]["active"])

        replacement = dict(
            second,
            recorded_at="2026-08-14T01:00:00+00:00",
            checks={"candidate_sustained_p95": {"passed": True}},
        )
        trend = report.merge_trend(trend, replacement)
        self.assertFalse(trend["alerts"][key]["active"])
        self.assertEqual(trend["alerts"][key]["last_transition"], "recovered")

    def test_historical_rerun_keeps_chronological_alert_order(self) -> None:
        first = report.build_point(
            evidence(), commit="a" * 40, run_id="7",
            recorded_at="2026-08-13T00:00:00+00:00", environment={}
        )
        first["checks"] = {"candidate_sustained_p95": {"passed": False}}
        second = dict(
            first, id="run-8", run_id="8",
            recorded_at="2026-08-14T00:00:00+00:00",
            checks={"candidate_sustained_p95": {"passed": True}},
        )
        third = dict(second, id="run-9", run_id="9", recorded_at="2026-08-15T00:00:00+00:00")
        trend = report.merge_trend(report.merge_trend(report.merge_trend(None, first), second), third)
        replacement = dict(first, recorded_at="2026-08-16T00:00:00+00:00")
        trend = report.merge_trend(trend, replacement)
        key = next(iter(trend["alerts"]))
        self.assertEqual(
            [item["id"] for item in trend["alerts"][key]["history"]],
            ["run-7", "run-8", "run-9"],
        )
        self.assertFalse(trend["alerts"][key]["active"])

    def test_rerun_outside_retained_history_is_rejected(self) -> None:
        template = report.build_point(
            evidence(), commit="a" * 40, run_id="1",
            recorded_at="2026-08-01T00:00:00+00:00", environment={}
        )
        trend = None
        for run_id in range(1, report.TREND_RETENTION + 2):
            item = dict(
                template,
                id=f"run-{run_id}",
                run_id=str(run_id),
                recorded_at=f"2026-08-{(run_id % 28) + 1:02d}T00:00:00+00:00",
                checks={"candidate_sustained_p95": {"passed": run_id % 2 == 0}},
            )
            trend = report.merge_trend(trend, item)
        with self.assertRaisesRegex(report.ReportError, "outside retained history"):
            report.merge_trend(trend, template)

    def test_other_cohort_cannot_hide_outside_retention_rerun(self) -> None:
        template = report.build_point(
            evidence(), commit="a" * 40, run_id="2",
            recorded_at="2026-08-01T00:00:00+00:00", environment={}
        )
        other = dict(template, id="run-1", run_id="1", cohort="other-cohort")
        trend = report.merge_trend(None, other)
        for run_id in range(2, report.TREND_RETENTION + 4):
            item = dict(
                template,
                id=f"run-{run_id}",
                run_id=str(run_id),
                checks={"candidate_sustained_p95": {"passed": run_id % 2 == 0}},
            )
            trend = report.merge_trend(trend, item)
        with self.assertRaisesRegex(report.ReportError, "outside retained history"):
            report.merge_trend(trend, template)

    def test_missing_recent_run_can_be_backfilled_without_discarded_history(self) -> None:
        template = report.build_point(
            evidence(), commit="a" * 40, run_id="10",
            recorded_at="2026-08-10T00:00:00+00:00", environment={}
        )
        later = dict(template, id="run-12", run_id="12")
        trend = report.merge_trend(report.merge_trend(None, template), later)
        missing = dict(template, id="run-11", run_id="11")
        trend = report.merge_trend(trend, missing)
        self.assertEqual(
            [point["run_id"] for point in trend["cohorts"][template["cohort"]]],
            ["10", "11", "12"],
        )

    def test_preminimum_move_is_allowed_without_discarded_history(self) -> None:
        template = report.build_point(
            evidence(), commit="a" * 40, run_id="1",
            recorded_at="2026-08-01T00:00:00+00:00", environment={}
        )
        source = dict(template, cohort="source-cohort")
        target_ten = dict(template, id="run-10", run_id="10", cohort="target-cohort")
        target_eleven = dict(template, id="run-11", run_id="11", cohort="target-cohort")
        trend = report.merge_trend(None, source)
        trend = report.merge_trend(trend, target_ten)
        trend = report.merge_trend(trend, target_eleven)
        moved = dict(source, cohort="target-cohort")
        trend = report.merge_trend(trend, moved)
        self.assertEqual(
            [point["run_id"] for point in trend["cohorts"]["target-cohort"]],
            ["1", "10", "11"],
        )

    def test_active_alert_recovers_when_rerun_moves_cohort(self) -> None:
        first = report.build_point(
            evidence(), commit="a" * 40, run_id="7",
            recorded_at="2026-08-13T00:00:00+00:00", environment={}
        )
        first["checks"] = {"candidate_sustained_p95": {"passed": False}}
        second = dict(first, id="run-8", run_id="8", recorded_at="2026-08-14T00:00:00+00:00")
        trend = report.merge_trend(report.merge_trend(None, first), second)
        old_key = next(iter(trend["alerts"]))
        moved = dict(
            second,
            cohort="new-cohort",
            checks={"candidate_sustained_p95": {"passed": True}},
        )
        trend = report.merge_trend(trend, moved)
        self.assertFalse(trend["alerts"][old_key]["active"])
        self.assertEqual(trend["alerts"][old_key]["last_transition"], "recovered")

    def test_cohort_move_before_destination_retention_is_rejected(self) -> None:
        template = report.build_point(
            evidence(), commit="a" * 40, run_id="1",
            recorded_at="2026-08-01T00:00:00+00:00", environment={}
        )
        source = dict(
            template,
            cohort="source-cohort",
            checks={"candidate_sustained_p95": {"passed": False}},
        )
        trend = report.merge_trend(None, source)
        for run_id in range(10, 101):
            item = dict(
                template,
                id=f"run-{run_id}",
                run_id=str(run_id),
                cohort="destination-cohort",
                checks={"candidate_sustained_p95": {"passed": run_id % 2 == 0}},
            )
            trend = report.merge_trend(trend, item)
        moved = dict(source, cohort="destination-cohort")
        with self.assertRaisesRegex(report.ReportError, "before retained cohort"):
            report.merge_trend(trend, moved)

        trend["cohorts"]["destination-cohort"] = []
        with self.assertRaisesRegex(report.ReportError, "before retained cohort"):
            report.merge_trend(trend, moved)


if __name__ == "__main__":
    unittest.main()
