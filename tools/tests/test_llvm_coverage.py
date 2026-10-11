# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "llvm_coverage", Path(__file__).resolve().parents[1] / "ci" / "llvm_coverage.py")
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


def record(path):
    return f"SF:{path}\nFN:1,example\nFNDA:1,example\nDA:1,1\nBRDA:1,0,0,1\nend_of_record\n"


class NativeCoverageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "server" / "build" / "linux-coverage"
        self.build.mkdir(parents=True)

    def fixture(self):
        coverage.prepare(self.build)
        (self.build / "llvm-profiles" / "binary-123.profraw").write_bytes(b"profile")
        (self.build / "CMakeCache.txt").write_text("CMAKE_C_COMPILER:FILEPATH=/opt/llvm/bin/clang\n")
        (self.build / "atrinik-server").write_bytes(b"\x7fELFbinary__llvm_covmap")
        plugins = self.build / "plugins"
        plugins.mkdir()
        (plugins / "python.so").write_bytes(b"\x7fELFplugin__llvm_covmap")

    def test_prepare_clears_previous_profiles_and_separates_processes(self):
        pattern = coverage.prepare(self.build)
        profiles = self.build / "llvm-profiles"
        (profiles / "stale.profraw").write_bytes(b"old")
        (self.build / "keep").write_text("keep")
        self.assertEqual(coverage.prepare(self.build), pattern)
        self.assertTrue(pattern.endswith("/%m-%p.profraw"))
        self.assertEqual(list(profiles.iterdir()), [])
        self.assertEqual((self.build / "keep").read_text(), "keep")

    def test_prepare_rejects_symlinked_profile_directory(self):
        other = self.root / "other"
        other.mkdir()
        (self.build / "llvm-profiles").symlink_to(other)
        with self.assertRaisesRegex(ValueError, "symlink"):
            coverage.prepare(self.build)

    def test_discovers_tests_and_plugins_but_excludes_probes_and_archives(self):
        self.fixture()
        (self.build / "test").write_bytes(b"\x7fELFtest__llvm_covmap")
        (self.build / "libcore.a").write_bytes(b"!<arch>__llvm_covmap")
        (self.build / "ordinary.so").write_bytes(b"\x7fELFplain")
        probes = self.build / "CMakeFiles"
        probes.mkdir()
        (probes / "probe").write_bytes(b"\x7fELFprobe__llvm_covmap")
        (self.build / "alias").symlink_to(self.build / "test")
        self.assertEqual([str(p.relative_to(self.build)) for p in coverage.coverage_objects(self.build)],
                         ["atrinik-server", "plugins/python.so", "test"])

    def test_filters_component_sources_and_retains_branch_function_counts(self):
        paths = ["server/src/server/main.c", "server/src/plugins/python/python.c",
                 "server/src/tests/unit.c", "server/build/generated.c",
                 "server/src/third_party/upstream.c", "client/src/client/main.c",
                 "server/src/include/header.h", "server-other/src/main.c"]
        report = "".join(record(self.root / path) for path in paths)
        actual = coverage.filter_lcov(report, self.root, "server")
        self.assertEqual(actual, record(self.root / paths[0]) + record(self.root / paths[1]))

    def test_library_includes_standalone_pathfinding_without_tests(self):
        (self.root / "libatrinik").mkdir()
        report = "".join(record(self.root / path) for path in
                         ["libatrinik/socket.c", "libatrinik/pathfinding/pathfinding.c",
                          "libatrinik/pathfinding/tests/test.c", "libatrinik/dependencies/source.c"])
        self.assertEqual(coverage.filter_lcov(report, self.root, "libatrinik"),
                         record(self.root / "libatrinik/socket.c") +
                         record(self.root / "libatrinik/pathfinding/pathfinding.c"))

    def test_scoped_source_archive_uses_module_root_layout(self):
        scoped = self.root / "atrinik-classic-libatrinik-5.1.0"
        scoped.mkdir()
        report = record(scoped / "socket.c") + record(scoped / "pathfinding/pathfinding.c")
        self.assertEqual(coverage.filter_lcov(report, scoped, "libatrinik"), report)

    def test_no_profile_is_a_failure(self):
        coverage.prepare(self.build)
        output = self.root / "coverage.lcov"
        output.write_text("stale report")
        with self.assertRaisesRegex(ValueError, "missing"):
            coverage.export(self.build, self.root, "server", str(output))
        self.assertFalse(output.exists())

    def test_successful_tool_with_stale_profile_warning_is_a_failure(self):
        result = subprocess.CompletedProcess(["llvm-cov"], 0, "partial report", "warning: mismatched data")
        with patch.object(coverage.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(ValueError, "mismatched data"):
                coverage.run(["llvm-cov"])

    def test_empty_profile_is_a_failure(self):
        self.fixture()
        (self.build / "llvm-profiles" / "empty.profraw").touch()
        with self.assertRaisesRegex(ValueError, "empty"):
            coverage.export(self.build, self.root, "server", "-")

    def test_mismatched_version_is_a_failure(self):
        with patch.object(coverage, "run", return_value="LLVM version 23.1.30"):
            with self.assertRaisesRegex(ValueError, "23.1.3"):
                coverage.verify_tool("llvm-cov")

    def test_export_validates_tools_merges_profiles_and_adds_plugin_objects(self):
        self.fixture()
        report = record(self.root / "server/src/server/main.c")
        calls = []

        def execute(command):
            calls.append(command)
            if "--version" in command:
                return "LLVM version 23.1.3\n"
            if command[1] == "export":
                return report
            return ""

        output = io.StringIO()
        with patch.object(coverage, "run", side_effect=execute), contextlib.redirect_stdout(output):
            coverage.export(self.build, self.root, "server", "-")
        self.assertEqual(output.getvalue(), report)
        self.assertEqual(len([call for call in calls if "--version" in call]), 3)
        self.assertIn("-sparse", calls[3])
        self.assertIn("-object", calls[4])
        self.assertIn(str(self.build / "plugins/python.so"), calls[4])

    def test_failed_merge_does_not_write_report(self):
        self.fixture()
        output = self.root / "coverage.lcov"
        with patch.object(coverage, "verify_tool"), patch.object(
                coverage, "run", side_effect=subprocess.CalledProcessError(1, "llvm-profdata")):
            with self.assertRaises(subprocess.CalledProcessError):
                coverage.export(self.build, self.root, "server", str(output))
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
