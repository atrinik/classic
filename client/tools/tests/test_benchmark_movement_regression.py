from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "benchmark_movement_regression.py"
SPEC = importlib.util.spec_from_file_location("benchmark_movement_regression", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


def record() -> str:
    return json.dumps(
        {
            "schema_version": 1,
            "tick_ms": 125,
            "checkpoint_sha256": "a" * 64,
            "phases": [
                {"name": name, "samples": samples, "p50_ns": 1, "p95_ns": 2, "p99_ns": 3, "max_ns": 4}
                for name, samples in benchmark.REQUIRED_PHASES.items()
            ],
        }
    )


class BenchmarkMovementRegressionTests(unittest.TestCase):
    def test_parses_closed_record(self) -> None:
        self.assertEqual(benchmark.parse_result(record())["schema_version"], 1)

    def test_rejects_extra_output(self) -> None:
        with self.assertRaisesRegex(benchmark.BenchmarkError, "exactly one"):
            benchmark.parse_result(record() + "\nnoise\n")

    def test_rejects_missing_phase(self) -> None:
        malformed = json.loads(record())
        malformed["phases"].pop()
        with self.assertRaisesRegex(benchmark.BenchmarkError, "incomplete"):
            benchmark.parse_result(json.dumps(malformed))

    def test_phase_summary_reports_timing_and_render_throughput(self) -> None:
        summary = benchmark.phase_summary([benchmark.parse_result(record())], "sustained")
        self.assertEqual(summary["samples"], 480)
        self.assertEqual(summary["replay_tick_hz"], 8)
        self.assertEqual(summary["p50_ms"], 0.0)
        self.assertEqual(summary["p95_render_fps"], 500_000_000.0)


if __name__ == "__main__":
    unittest.main()
