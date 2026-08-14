from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SITE_SPEC = importlib.util.spec_from_file_location(
    "daily_performance_site", ROOT / "ci" / "daily_performance_site.py"
)
assert SITE_SPEC and SITE_SPEC.loader
site = importlib.util.module_from_spec(SITE_SPEC)
SITE_SPEC.loader.exec_module(site)
report = site.report

CLIENT_TOOLS = ROOT.parent / "client" / "tools"
sys.path.insert(0, str(CLIENT_TOOLS))
FIXTURE_SPEC = importlib.util.spec_from_file_location(
    "daily_site_movement_fixtures",
    CLIENT_TOOLS / "tests" / "test_benchmark_movement_regression.py",
)
assert FIXTURE_SPEC and FIXTURE_SPEC.loader
fixtures = importlib.util.module_from_spec(FIXTURE_SPEC)
FIXTURE_SPEC.loader.exec_module(fixtures)


def evidence() -> dict[str, object]:
    return fixtures.benchmark._build_evidence(
        [],
        [fixtures.native_record(), fixtures.native_record()],
        [fixtures.native_record(viewport="large"), fixtures.native_record(viewport="large")],
        fixtures.additional_contexts(full=True),
        enforce_performance=False,
        comparison_note="event-has-no-comparison-base",
    )


def environment(run_id: str) -> dict[str, str]:
    return {
        "artifact_url": f"https://github.com/atrinik/classic/actions/runs/{run_id}#artifacts",
        "repository": "atrinik/classic",
        "runner_image": "ubuntu24",
        "workflow_url": f"https://github.com/atrinik/classic/actions/runs/{run_id}",
    }


class DailyPerformanceSiteTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.evidence = self.root / "evidence.json"
        self.evidence.write_text(json.dumps(evidence()))
        self.legacy = self.root / "legacy"
        (self.legacy / "points").mkdir(parents=True)
        point = report.build_point(
            evidence(), commit="a" * 40, run_id="1", run_attempt="1",
            recorded_at="2026-08-13T00:00:00Z", environment=environment("1"),
        )
        point["schema_version"] = 2
        del point["environment"]["artifact_url"]
        for context in point["contexts"].values():
            context.pop("lighting_work_ms", None)
        (self.legacy / "points" / "1.json").write_text(json.dumps(point))
        (self.legacy / "trend.json").write_text(json.dumps(report.merge_trend(None, point)))

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def build(self, output: Path, *, run_id: str = "2", attempt: str = "1",
              checkpoint: Path | None = None) -> dict[str, object]:
        arguments = {
            "evidence_path": self.evidence,
            "output": output,
            "commit": "a" * 40,
            "run_id": run_id,
            "run_attempt": attempt,
            "recorded_at": "2026-08-14T00:00:00Z",
            "environment": environment(run_id),
        }
        if checkpoint is None:
            arguments.update(
                legacy_checkpoint=self.legacy,
                legacy_final_ref="b" * 40,
            )
        else:
            arguments["checkpoint"] = checkpoint
        return site.build_site(**arguments)

    def test_bootstrap_builds_a_valid_bounded_split_site(self) -> None:
        output = self.root / "site"
        manifest = self.build(output)
        self.assertEqual(site.validate_site(output), manifest)
        state = json.loads((output / site.STATE_PATH).read_text())
        points = site._retained_points(state["trend"])
        self.assertEqual([point["run_id"] for point in points], ["1", "2"])
        self.assertNotIn("contexts", points[-1])
        self.assertTrue((output / "points/run-2.json").is_file())
        self.assertTrue((output / "reports/run-2/index.html").is_file())
        self.assertEqual((output / "trend.json").read_bytes(),
                         (output / "v1/trend.json").read_bytes())

    def test_same_inputs_and_checkpoint_produce_byte_stable_site(self) -> None:
        first = self.root / "first"
        second = self.root / "second"
        self.build(first)
        self.build(second)
        first_files = {
            path.relative_to(first): path.read_bytes()
            for path in first.rglob("*") if path.is_file()
        }
        second_files = {
            path.relative_to(second): path.read_bytes()
            for path in second.rglob("*") if path.is_file()
        }
        self.assertEqual(first_files, second_files)

    def test_higher_attempt_replaces_one_logical_point(self) -> None:
        first = self.root / "first"
        second = self.root / "second"
        self.build(first)
        self.build(second, run_id="2", attempt="2", checkpoint=first)
        points = site._retained_points(
            json.loads((second / site.STATE_PATH).read_text())["trend"]
        )
        self.assertEqual([point["run_id"] for point in points], ["1", "2"])
        self.assertEqual(points[-1]["run_attempt"], "2")

    def test_older_attempt_is_rejected(self) -> None:
        first = self.root / "first"
        second = self.root / "second"
        self.build(first, attempt="2")
        with self.assertRaisesRegex(report.ReportError, "older attempt"):
            self.build(second, attempt="1", checkpoint=first)

    def test_global_cohort_bound_prunes_files_and_records_watermark(self) -> None:
        template = report.build_point(
            evidence(), commit="a" * 40, run_id="1", recorded_at="2026-08-01T00:00:00Z",
            environment=environment("1"),
        )
        trend = None
        for run_id in range(1, site.MAX_COHORTS + 3):
            point = dict(template, id=f"run-{run_id}", run_id=str(run_id),
                         cohort=f"cohort-{run_id}")
            trend = report.merge_trend(trend, point)
        site._prune_global_history(
            trend, f"cohort-{site.MAX_COHORTS + 2}", str(site.MAX_COHORTS + 2)
        )
        self.assertEqual(len(trend["cohorts"]), site.MAX_COHORTS)
        self.assertGreater(trend["global_retention_watermark"], 0)

    def test_global_cohort_bound_always_retains_a_backfilled_current_cohort(self) -> None:
        template = report.build_point(
            evidence(), commit="a" * 40, run_id="1", recorded_at="2026-08-01T00:00:00Z",
            environment=environment("1"),
        )
        trend = None
        current = "backfilled-current"
        for run_id in range(10, 10 + site.MAX_COHORTS):
            point = dict(template, id=f"run-{run_id}", run_id=str(run_id),
                         cohort=f"cohort-{run_id}")
            trend = report.merge_trend(trend, point)
        backfill = dict(template, cohort=current)
        trend = report.merge_trend(trend, backfill)
        site._prune_global_history(trend, current, "1")
        self.assertEqual(len(trend["cohorts"]), site.MAX_COHORTS)
        self.assertIn(current, trend["cohorts"])

    def test_manifest_tampering_fails_closed(self) -> None:
        output = self.root / "site"
        self.build(output)
        (output / "index.html").write_text("tampered")
        with self.assertRaisesRegex(site.SiteError, "digests"):
            site.validate_site(output)

    def test_digest_bound_but_stale_history_path_fails_closed(self) -> None:
        output = self.root / "site"
        self.build(output)
        stale = output / "points" / "run-999.json"
        stale.write_text("{}\n")
        manifest = json.loads((output / site.MANIFEST_PATH).read_text())
        manifest["files"] = site._manifest_files(output)
        (output / site.MANIFEST_PATH).write_bytes(site._json_bytes(manifest))
        with self.assertRaisesRegex(site.SiteError, "unapproved or stale"):
            site.validate_site(output)

    def test_digest_bound_but_mismatched_report_fails_closed(self) -> None:
        output = self.root / "site"
        self.build(output)
        manifest = json.loads((output / site.MANIFEST_PATH).read_text())
        report_path = f"reports/run-{manifest['run_id']}/index.html"
        (output / report_path).write_text("<!doctype html><p>stale</p>\n")
        manifest["files"] = site._manifest_files(output)
        (output / site.MANIFEST_PATH).write_bytes(site._json_bytes(manifest))
        with self.assertRaisesRegex(site.SiteError, "does not match"):
            site.validate_site(output)

    def test_attempt_qualified_checkpoint_artifact_is_digest_validated(self) -> None:
        output = self.root / "site"
        artifact = self.root / "artifact"
        self.build(output)
        manifest = json.loads((output / site.MANIFEST_PATH).read_text())
        paths = (
            site.MANIFEST_PATH,
            site.STATE_PATH,
            site.ALERTS_PATH,
            f"points/run-{manifest['run_id']}.json",
        )
        for relative in paths:
            destination = artifact / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes((output / relative).read_bytes())
        self.assertEqual(site.validate_checkpoint_artifact(artifact), manifest)
        (artifact / site.ALERTS_PATH).write_text("{}\n")
        with self.assertRaisesRegex(site.SiteError, "digest mismatch"):
            site.validate_checkpoint_artifact(artifact)

    def test_checkpoint_artifact_point_is_bound_to_manifest_identity(self) -> None:
        output = self.root / "site"
        artifact = self.root / "artifact"
        self.build(output)
        manifest = json.loads((output / site.MANIFEST_PATH).read_text())
        point_path = f"points/run-{manifest['run_id']}.json"
        paths = (site.MANIFEST_PATH, site.STATE_PATH, site.ALERTS_PATH, point_path)
        for relative in paths:
            destination = artifact / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes((output / relative).read_bytes())
        detailed = json.loads((artifact / point_path).read_text())
        detailed["commit"] = "f" * 40
        data = site._json_bytes(detailed)
        (artifact / point_path).write_bytes(data)
        artifact_manifest = json.loads((artifact / site.MANIFEST_PATH).read_text())
        artifact_manifest["files"][point_path] = {
            "sha256": site._sha256(data),
            "size": len(data),
        }
        (artifact / site.MANIFEST_PATH).write_bytes(site._json_bytes(artifact_manifest))
        with self.assertRaisesRegex(site.SiteError, "not bound"):
            site.validate_checkpoint_artifact(artifact)

    def test_artifact_derived_labels_are_escaped_and_no_javascript_is_emitted(self) -> None:
        point = report.build_point(
            evidence(), commit="a" * 40, run_id="1", recorded_at="2026-08-01T00:00:00Z",
            environment=environment("1"),
        )
        point["checks"] = {"<script>alert(1)</script>": {"passed": False}}
        rendered = site.render_report(point).decode()
        self.assertIn("&lt;script&gt;alert(1)&lt;/script&gt;", rendered)
        self.assertNotIn("<script", rendered.lower())


if __name__ == "__main__":
    unittest.main()
