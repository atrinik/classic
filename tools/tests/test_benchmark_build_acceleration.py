import importlib.util
import os
from pathlib import Path
import tempfile
import unittest


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


if __name__ == "__main__":
    unittest.main()
