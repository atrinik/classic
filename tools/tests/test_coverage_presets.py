# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import importlib.util
import json
import platform
from pathlib import Path
import shutil
import subprocess
import tempfile
import tarfile
import unittest

from tools.release import package_sources

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("llvm_coverage", ROOT / "tools/ci/llvm_coverage.py")
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)
MODULES = (".", "client", "server", "libatrinik", "libatrinik/pathfinding")


def preset(section, name):
    values = {entry["name"]: entry for entry in section}
    selected = values[name]
    inherited = selected.get("inherits")
    if inherited is None:
        return selected.copy()
    result = preset(section, inherited)
    for key, value in selected.items():
        if isinstance(value, dict):
            result[key] = {**result.get(key, {}), **value}
        else:
            result[key] = value
    return result


class CoveragePresetTests(unittest.TestCase):
    def test_opt_in_has_matching_configure_build_and_test_presets(self):
        for module in MODULES:
            with self.subTest(module=module):
                source = ROOT / module
                data = json.loads((source / "CMakePresets.json").read_text())
                configured = preset(data["configurePresets"], "llvm-coverage")
                built = preset(data["buildPresets"], "llvm-coverage")
                tested = preset(data["testPresets"], "llvm-coverage")
                self.assertTrue(configured["cacheVariables"]["ENABLE_COVERAGE"])
                self.assertEqual(configured["cacheVariables"]["CMAKE_BUILD_TYPE"], "Debug")
                # The entry point selects the shared toolchain by physical module
                # location; a fixed preset path breaks scoped source archives.
                self.assertNotIn("CMAKE_TOOLCHAIN_FILE", configured["cacheVariables"])
                self.assertNotIn("toolchainFile", configured)
                self.assertEqual(built["configurePreset"], "llvm-coverage")
                self.assertEqual(tested["configurePreset"], "llvm-coverage")
                self.assertEqual(tested["environment"]["LLVM_PROFILE_FILE"],
                                 configured["binaryDir"] + "/llvm-profiles/%m-%p.profraw")


    def assert_configure_selects_toolchain(self, source, build, toolchain):
        # Run actual preset configuration and the actual authored CMake prefix.
        # Abort at project() before language detection: this verifies selection
        # without inventing compiler identity or permitting a product policy bypass.
        build.mkdir(parents=True)
        probe = build / "toolchain-selection-probe.cmake"
        probe.write_text(f'''
file(REAL_PATH "${{CMAKE_TOOLCHAIN_FILE}}" selected_toolchain)
if (NOT selected_toolchain STREQUAL "{toolchain.as_posix()}" OR
        NOT EXISTS "${{selected_toolchain}}")
    message(FATAL_ERROR "Unexpected selected toolchain: ${{CMAKE_TOOLCHAIN_FILE}}")
endif ()
if (NOT ENABLE_COVERAGE OR NOT BUILD_TESTING OR NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "Coverage preset options were not applied")
endif ()
message(FATAL_ERROR "ATRINIK_TOOLCHAIN_SELECTION_PROBE_COMPLETED")
''', encoding="utf-8")
        result = subprocess.run(
            ["cmake", "--preset", "llvm-coverage", "-B", str(build),
             "-DATRINIK_PACKAGE_VERSION=9.8.7",
             "-DCMAKE_PROJECT_INCLUDE_BEFORE=" + str(probe)],
            cwd=source, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ATRINIK_TOOLCHAIN_SELECTION_PROBE_COMPLETED",
                      result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which("cmake") and platform.system() == "Linux",
                         "Native Linux CMake selection probe unavailable")
    def test_actual_checkout_presets_select_shared_native_toolchain(self):
        with tempfile.TemporaryDirectory() as temporary:
            for index, module in enumerate(MODULES):
                with self.subTest(module=module):
                    self.assert_configure_selects_toolchain(
                        ROOT / module, Path(temporary) / str(index),
                        ROOT / "cmake/toolchains/llvm.cmake")

    @unittest.skipUnless(shutil.which("cmake") and platform.system() == "Linux",
                         "Native Linux CMake selection probe unavailable")
    def test_actual_scoped_archive_presets_select_packaged_native_toolchain(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source_tar = root / "source.tar"
            # Package the actual authored entry points/presets and shared modules
            # through the release archiver. Dependencies beyond project() are not
            # needed for this deliberately bounded selection probe.
            with tarfile.open(source_tar, "w") as archive:
                for module in MODULES:
                    for name in ("CMakeLists.txt", "CMakePresets.json"):
                        relative = Path(module) / name
                        archive.add(ROOT / relative, arcname=relative.as_posix())
                archive.add(ROOT / "cmake", arcname="cmake")
            for scope in ("root", "client", "server", "libatrinik"):
                with self.subTest(scope=scope):
                    package = root / (scope + ".tar.gz")
                    package_sources.build_archive(source_tar, package, scope,
                                                  "9.8.7", 42, "f" * 40)
                    destination = root / scope
                    with tarfile.open(package) as archive:
                        archive.extractall(destination, filter="data")
                    source = next(destination.iterdir())
                    toolchain = source / "cmake/toolchains/llvm.cmake"
                    self.assert_configure_selects_toolchain(
                        source, root / (scope + "-build"), toolchain)
                    pathfinding = source / ("libatrinik/pathfinding" if scope == "root" else
                                            "pathfinding" if scope == "libatrinik" else
                                            "dependencies/libatrinik/pathfinding")
                    self.assert_configure_selects_toolchain(
                        pathfinding, root / (scope + "-pathfinding-build"), toolchain)

    def test_native_profile_preparation_leaves_existing_gcc_cache_untouched(self):
        for module in MODULES[:-1]:
            with self.subTest(module=module), tempfile.TemporaryDirectory() as temporary:
                source = Path(temporary)
                data = json.loads((ROOT / module / "CMakePresets.json").read_text())
                legacy = preset(data["configurePresets"], "linux-coverage")
                native = preset(data["configurePresets"], "llvm-coverage")
                legacy_dir = Path(legacy["binaryDir"].replace("${sourceDir}", str(source)))
                native_dir = Path(native["binaryDir"].replace("${sourceDir}", str(source)))
                self.assertNotEqual(legacy_dir, native_dir)
                legacy_dir.mkdir(parents=True)
                cache = legacy_dir / "CMakeCache.txt"
                cache.write_text("CMAKE_C_COMPILER:FILEPATH=/usr/bin/gcc\n")
                raw = legacy_dir / "old.gcda"
                raw.write_bytes(b"retained GCC data")
                native_dir.mkdir(parents=True)
                pattern = coverage.prepare(native_dir)
                self.assertEqual(pattern, str(native_dir / "llvm-profiles/%m-%p.profraw"))
                self.assertEqual(cache.read_text(), "CMAKE_C_COMPILER:FILEPATH=/usr/bin/gcc\n")
                self.assertEqual(raw.read_bytes(), b"retained GCC data")
                self.assertNotIn("CMAKE_TOOLCHAIN_FILE", legacy.get("cacheVariables", {}))
                self.assertNotIn("LLVM_PROFILE_FILE", preset(data["testPresets"], "linux-coverage").get("environment", {}))

    @unittest.skipUnless(shutil.which("cmake"), "CMake unavailable")
    def test_cmake_accepts_all_coverage_preset_files_without_compiler_probes(self):
        for module in MODULES:
            with self.subTest(module=module):
                result = subprocess.run(["cmake", "--list-presets=all"], cwd=ROOT / module,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.count('"llvm-coverage"'), 3)


if __name__ == "__main__":
    unittest.main()
