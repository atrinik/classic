# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

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
                toolchain = configured["cacheVariables"]["CMAKE_TOOLCHAIN_FILE"].replace("${sourceDir}", str(source))
                self.assertEqual(Path(toolchain).resolve(), ROOT / "cmake/toolchains/llvm.cmake")
                self.assertEqual(built["configurePreset"], "llvm-coverage")
                self.assertEqual(tested["configurePreset"], "llvm-coverage")
                self.assertEqual(tested["environment"]["LLVM_PROFILE_FILE"],
                                 configured["binaryDir"] + "/llvm-profiles/%m-%p.profraw")

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
