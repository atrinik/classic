from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("daily_report", ROOT / "ci" / "daily_performance_report.py")
assert SPEC and SPEC.loader
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


def evidence() -> dict[str, object]:
    phase = {
        "p50_ns": 1_000_000, "p95_ns": 2_000_000, "p99_ns": 3_000_000,
        "max_ns": 4_000_000, "first_window_p95_ns": 2_000_000,
        "last_window_p95_ns": 2_100_000, "map": {
            "map_draws": 1, "primary_map_draws": 1, "auxiliary_map_draws": 0,
            "presents": 1, "changed_map_packets": 1, "noop_map_packets": 0,
        }, "full_draw_reasons": {"reset_packet": 1},
        "queue": {"peak_depth": 1, "peak_bytes": 2, "oldest_age_us": 3,
                  "budget_yields": 0, "recoveries": 0},
        "lighting": {"counters": {"field_rebuilds": 1, "field_reuses": 2,
                    "lit_sprite_lookups": 3, "lit_sprite_hits": 2,
                    "lit_sprite_misses": 1, "lit_sprite_evictions": 0}},
        "sprite_cache": {"counters": {"lookups": 1, "hits": 1, "misses": 0, "gc_removals": 0}},
    }
    record = {"identity": {"instrumentation": {"schema_version": 4},
                            "implementation": {"compiler_version": "15"},
                            "run": {"mode": "smooth", "viewport": {"name": "standard"}}},
              "fixture": {"manifest_sha256": "a", "snapshot_sha256": "b"},
              "phases": {name: copy.deepcopy(phase) for name in report.PHASES}}
    return {"schema_version": 4, "status": "passed", "failed": False,
            "records": {"candidate_standard": [record], "additional_contexts": {}},
            "checks": {}, "resources": {"candidate_standard": {}}}


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

    def test_two_failed_points_open_one_alert_and_two_passes_recover_it(self) -> None:
        item = evidence()
        item["checks"] = {"candidate_sustained_p95": {"passed": False}}
        point = report.build_point(item, commit="a" * 40, run_id="7",
                                   recorded_at="2026-08-13T00:00:00+00:00", environment={})
        trend = report.merge_trend(None, point)
        for index, passed in enumerate((False, True, True), 2):
            item["checks"]["candidate_sustained_p95"]["passed"] = passed
            next_point = report.build_point(item, commit=(str(index) * 40)[:40], run_id=str(index),
                                            recorded_at=f"2026-08-{index:02d}T00:00:00+00:00", environment={})
            trend = report.merge_trend(trend, next_point)
        state = next(iter(trend["alerts"].values()))
        self.assertFalse(state["active"])
        self.assertEqual(state["last_transition"], "recovered")


if __name__ == "__main__":
    unittest.main()
