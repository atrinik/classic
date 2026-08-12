from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "benchmark_lighting_regression.py"
SPEC = importlib.util.spec_from_file_location("benchmark_lighting_regression", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


class BenchmarkLightingRegressionTests(unittest.TestCase):
    def test_parses_closed_record(self) -> None:
        self.assertEqual(
            benchmark.parse_result("player-view-benchmark\tstandard\t101\t12345\n", "standard"),
            12345,
        )

    def test_rejects_wrong_mode(self) -> None:
        with self.assertRaisesRegex(benchmark.BenchmarkError, "incompatible"):
            benchmark.parse_result("player-view-benchmark\tlarge\t101\t12345\n", "standard")

    def test_rejects_even_iteration_count(self) -> None:
        with self.assertRaisesRegex(benchmark.BenchmarkError, "invalid bounds"):
            benchmark.parse_result("player-view-benchmark\tstandard\t100\t12345\n", "standard")

    def test_rejects_extra_output(self) -> None:
        with self.assertRaisesRegex(benchmark.BenchmarkError, "exactly one"):
            benchmark.parse_result(
                "noise\nplayer-view-benchmark\tstandard\t101\t12345\n", "standard"
            )


if __name__ == "__main__":
    unittest.main()
