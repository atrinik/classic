import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "benchmark_build_acceleration",
    ROOT / "tools" / "benchmark_build_acceleration.py",
)
assert SPEC is not None and SPEC.loader is not None
BENCHMARK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCHMARK)


class BenchmarkBuildAccelerationTests(unittest.TestCase):
    def test_parse_cmake_cache_excludes_internal_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "CMakeCache.txt"
            cache.write_text(
                "// comment\n"
                "CMAKE_BUILD_TYPE:STRING=Debug\n"
                "ENABLE_PRECOMPILED_HEADERS:BOOL=ON\n"
                "CMAKE_HOME_DIRECTORY:INTERNAL=/source\n",
                encoding="utf-8",
            )
            self.assertEqual(
                BENCHMARK.parse_cmake_cache(cache),
                {
                    "CMAKE_BUILD_TYPE": "Debug",
                    "ENABLE_PRECOMPILED_HEADERS": "ON",
                },
            )

    def test_changed_artifacts_counts_new_and_rewritten_files(self):
        first = Path("first.o")
        second = Path("second.gch")
        third = Path("third.o")
        self.assertEqual(
            BENCHMARK.changed_artifacts(
                {first: 1, second: 2},
                {first: 1, second: 3, third: 4},
            ),
            2,
        )

    def test_touch_for_build_can_be_restored_exactly(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.c"
            source.write_text("int value;\n", encoding="utf-8")
            original = source.stat()
            saved = BENCHMARK.touch_for_build(source)
            self.assertGreater(source.stat().st_mtime_ns, original.st_mtime_ns)
            BENCHMARK.os.utime(source, ns=saved)
            restored = source.stat()
            self.assertEqual(restored.st_atime_ns, original.st_atime_ns)
            self.assertEqual(restored.st_mtime_ns, original.st_mtime_ns)

    def test_build_command_is_argument_safe(self):
        self.assertEqual(
            BENCHMARK.build_command(Path("build tree"), "atrinik", 8),
            [
                "cmake",
                "--build",
                "build tree",
                "--target",
                "atrinik",
                "--parallel",
                "8",
            ],
        )

    def test_process_group_rss_includes_current_process(self):
        if not Path("/proc").is_dir():
            self.skipTest("process-group RSS sampling requires procfs")
        self.assertGreater(BENCHMARK.process_group_rss_kib(os.getpgrp()), 0)

    def test_run_measured_records_output_and_changed_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            artifact = build_dir / "result.o"
            result = BENCHMARK.run_measured(
                [
                    BENCHMARK.sys.executable,
                    "-c",
                    (
                        "from pathlib import Path; "
                        f"Path({str(artifact)!r}).write_text('object'); "
                        "print('built target')"
                    ),
                ],
                build_dir,
            )
            self.assertEqual(result["return_code"], 0)
            self.assertEqual(result["rebuilt_artifacts"], 1)
            self.assertEqual(result["output_tail"], ["built target"])
            self.assertGreaterEqual(result["elapsed_seconds"], 0)

    def test_run_measured_reports_failed_command_details(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, '"return_code": 3'):
                BENCHMARK.run_measured(
                    [
                        BENCHMARK.sys.executable,
                        "-c",
                        "print('failed target'); raise SystemExit(3)",
                    ],
                    Path(directory),
                )

    def test_measure_runs_each_phase_and_reports_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_dir = root / "build"
            build_dir.mkdir()
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc\n"
                "CMAKE_BUILD_TYPE:STRING=Debug\n"
                "ENABLE_PRECOMPILED_HEADERS:BOOL=ON\n"
                "ENABLE_COVERAGE:BOOL=OFF\n"
                "ENABLE_SANITIZERS:BOOL=OFF\n",
                encoding="utf-8",
            )
            source = root / "source.c"
            header = root / "global.h"
            source.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            header.write_text("#pragma once\n", encoding="utf-8")
            args = BENCHMARK.argparse.Namespace(
                build_dir=build_dir,
                target="atrinik",
                source=source,
                header=header,
                jobs=4,
                runs=2,
                output=None,
            )
            phase = {
                "elapsed_seconds": 0.1,
                "peak_rss_kib": 1024,
                "rebuilt_artifacts": 1,
                "return_code": 0,
                "output_tail": [],
            }
            cmake_version = mock.Mock(stdout="cmake version 4.2.3\n")
            with (
                mock.patch.object(
                    BENCHMARK, "run_measured", side_effect=[phase] * 8
                ) as measured,
                mock.patch.object(
                    BENCHMARK, "compiler_version", return_value="cc version"
                ),
                mock.patch.object(
                    BENCHMARK.subprocess, "run", return_value=cmake_version
                ) as run,
            ):
                report = BENCHMARK.measure(args)

            self.assertEqual(measured.call_count, 8)
            clean_command = ["cmake", "--build", str(build_dir), "--target", "clean"]
            self.assertEqual(
                sum(call.args[0] == clean_command for call in run.call_args_list), 2
            )
            self.assertEqual(len(report["runs"]), 2)
            self.assertEqual(report["environment"]["compiler_version"], "cc version")
            self.assertEqual(report["configuration"]["precompiled_headers"], "ON")
            self.assertEqual(report["configuration"]["command"][-2:], ["--parallel", "4"])


if __name__ == "__main__":
    unittest.main()
