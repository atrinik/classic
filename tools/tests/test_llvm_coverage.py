# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import tarfile
import sys
import unittest
from unittest.mock import patch

from tools.release import package_sources

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
        (self.root / "client").mkdir()
        (self.root / "libatrinik").mkdir()

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
        report = "".join(record(self.root / path) for path in
                         ["libatrinik/socket.c", "libatrinik/pathfinding/pathfinding.c",
                          "libatrinik/pathfinding/tests/test.c", "libatrinik/dependencies/source.c"])
        self.assertEqual(coverage.filter_lcov(report, self.root, "libatrinik"),
                         record(self.root / "libatrinik/socket.c") +
                         record(self.root / "libatrinik/pathfinding/pathfinding.c"))

    def test_scoped_filter_requires_matching_archive_identity(self):
        scoped = self.root / "scoped"
        scoped.mkdir()
        (scoped / "server").mkdir()  # A directory name cannot override archive owner identity.
        cmake = scoped / "CMakeLists.txt"
        version = scoped / "VERSION"
        revision = scoped / "SOURCE_REVISION"
        report = record(scoped / "src/server/main.c")
        variants = (
            (None, "5.1.0", "a" * 40),
            ("project(atrinik-classic LANGUAGES C)", "5.1.0", "a" * 40),
            ("project(atrinik-client LANGUAGES C)", "5.1.0", "a" * 40),
            ("project(atrinik-server LANGUAGES C)", "not-a-version", "a" * 40),
            ("project(atrinik-server LANGUAGES C)", "5.1.0", "not-a-revision"),
            ("project(atrinik-server LANGUAGES C)\nproject(atrinik-client)", "5.1.0", "a" * 40),
        )
        for project, package_version, source_revision in variants:
            with self.subTest(project=project, version=package_version, revision=source_revision):
                if project is None:
                    cmake.unlink(missing_ok=True)
                else:
                    cmake.write_text(project)
                version.write_text(package_version)
                revision.write_text(source_revision)
                # An integrated archive identity keeps the narrower server/
                # prefix and rejects flattened src/ records at the filter stage.
                error = "no intended production" if project and "atrinik-classic" in project else "does not identify"
                with self.assertRaisesRegex(ValueError, error):
                    coverage.filter_lcov(report, scoped, "server")
        # A renamed generated archive remains valid; recognition does not depend
        # on its extraction directory's spelling.
        cmake.write_text('project("atrinik-server" LANGUAGES C)')
        version.write_text("5.1.0")
        revision.write_text("a" * 40)
        self.assertEqual(coverage.filter_lcov(report, scoped, "server"), report)

    def test_missing_monorepo_component_does_not_widen_filter_to_root(self):
        (self.root / "libatrinik").rmdir()
        (self.root / "CMakeLists.txt").write_text("project(atrinik-classic LANGUAGES C)")
        (self.root / "VERSION").write_text("5.1.0")
        (self.root / "SOURCE_REVISION").write_text("a" * 40)
        with self.assertRaisesRegex(ValueError, "does not identify"):
            coverage.filter_lcov(record(self.root / "server/src/server/main.c"),
                                 self.root, "libatrinik")

    def test_all_keeps_monorepo_owner_prefixes(self):
        production = ["client/src/client/main.c", "server/src/server/main.c",
                      "libatrinik/socket.c", "libatrinik/pathfinding/pathfinding.c"]
        excluded = ["src/server/main.c", "client/src/tests/unit.c",
                    "server/build/generated.c", "libatrinik/dependencies/upstream.c"]
        expected = "".join(record(self.root / name) for name in production)
        self.assertEqual(coverage.filter_lcov(
            expected + "".join(record(self.root / name) for name in excluded), self.root, "all"),
            expected)

    def test_generated_scoped_archives_export_production_lcov_through_packaged_cli(self):
        owner = Path(__file__).resolve().parents[2]
        source_tar = self.root / "source.tar"
        authored = ["tools/ci/llvm_coverage.py", "CMakeLists.txt",
                    "client/CMakeLists.txt", "server/CMakeLists.txt", "libatrinik/CMakeLists.txt",
                    "client/src/client/main.c", "server/src/server/main.c",
                    "libatrinik/socket.c", "libatrinik/pathfinding/pathfinding.c"]
        with tarfile.open(source_tar, "w") as archive:
            for name in authored:
                archive.add(owner / name, arcname=name)
        tool = self.root / "llvm-tool-fixture"
        tool.write_text(f'''#!{sys.executable}
import os
from pathlib import Path
import sys
if sys.argv[1] == "--version":
    print("LLVM version 23.1.3")
elif sys.argv[1] == "export":
    sys.stdout.write(Path(os.environ["COVERAGE_FIXTURE_REPORT"]).read_text())
''')
        tool.chmod(0o755)
        for scope in ("root", "client", "server", "libatrinik"):
            with self.subTest(scope=scope):
                archive = self.root / (scope + ".tar.gz")
                package_sources.build_archive(source_tar, archive, scope, "5.1.0", 42, "a" * 40)
                destination = self.root / (scope + "-archive")
                with tarfile.open(archive) as source:
                    source.extractall(destination, filter="data")
                package = next(destination.iterdir())
                build = package / "build/llvm-coverage"
                build.mkdir(parents=True)
                coverage.prepare(build)
                (build / "llvm-profiles/module-123.profraw").write_bytes(b"profile fixture")
                (build / "CMakeCache.txt").write_text("CMAKE_C_COMPILER:FILEPATH=" + str(tool) + "\n")
                (build / "native-test").write_bytes(b"\x7fELFnative fixture__llvm_covmap")
                production = (["client/src/client/main.c", "server/src/server/main.c",
                               "libatrinik/socket.c", "libatrinik/pathfinding/pathfinding.c"]
                              if scope == "root" else
                              ["socket.c", "pathfinding/pathfinding.c"] if scope == "libatrinik"
                              else [f"src/{scope}/main.c"])
                positive = "".join(record(package / name) for name in production)
                # Relative source paths are also valid in LLVM LCOV output.
                positive += record(production[0])
                negative = "".join(record(package / name) for name in (
                    "src/tests/unit.c", "tests/unit.c", "build/generated.c",
                    "dependencies/libatrinik/socket.c", "_deps/upstream.c",
                    "third_party/upstream.c", "third-party/upstream.c", "header.h"))
                negative += record(self.root / "client/src/client/main.c")
                negative += record("../outside.c")
                report = self.root / (scope + "-tool-report.lcov")
                report.write_text(positive + negative)
                output = package / "coverage.lcov"
                command = [sys.executable, str(package / "tools/ci/llvm_coverage.py"), "export",
                           "--build-dir", str(build), "--source-root", str(package),
                           "--component", "all" if scope == "root" else scope, "--output", str(output)]
                environment = {**os.environ, "LLVM_COV": str(tool), "LLVM_PROFDATA": str(tool),
                               "COVERAGE_FIXTURE_REPORT": str(report)}
                result = subprocess.run(command, env=environment, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(output.read_text(), positive)
                # Excluded-only output and malformed archive identity must fail
                # collection and remove the previous report instead of uploading it.
                report.write_text(negative)
                result = subprocess.run(command, env=environment, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("no intended production", result.stderr)
                self.assertFalse(output.exists())
                report.write_text(positive)
                output.write_text("stale")
                (package / "SOURCE_REVISION").write_text("malformed")
                result = subprocess.run(command, env=environment, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("does not identify", result.stderr)
                self.assertFalse(output.exists())

    def test_scoped_source_archive_uses_module_root_layout(self):
        scoped = self.root / "atrinik-classic-libatrinik-5.1.0"
        scoped.mkdir()
        (scoped / "VERSION").write_text("5.1.0")
        (scoped / "SOURCE_REVISION").write_text("a" * 40)
        (scoped / "CMakeLists.txt").write_text("project(libatrinik LANGUAGES C)")
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
