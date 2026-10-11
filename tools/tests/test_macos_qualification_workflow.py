# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import json
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/llvm-macos-qualification.yml"


def embedded_python_script(index):
    text = WORKFLOW.read_text()
    matches = re.findall(r"          python3 - <<'PY'\n(.*?)          PY\n", text, re.S)
    match = matches[index] if matches else None
    if match is None:
        raise AssertionError("Native CPU discovery gate is absent")
    return "\n".join(line[10:] for line in match.splitlines()) + "\n"


class MacOSQualificationWorkflowTests(unittest.TestCase):
    def test_discovery_requires_each_native_component_and_python_plugin(self):
        names = [
            "protocol-generate-check", "libatrinik-smoke", "atrinik-pathfinding",
            "client-help-parser", "server-unit-object", "server-plugin-python",
        ]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence = root / "build/macos-qualification-evidence"
            evidence.mkdir(parents=True)
            script = root / "gate.py"
            script.write_text(embedded_python_script(-1))
            for excluded in [None, *names]:
                selected = [name for name in names if name != excluded]
                (evidence / "tests.json").write_text(json.dumps({
                    "tests": [{"name": name} for name in selected]
                }))
                result = subprocess.run(
                    [shutil.which("python3"), str(script)], cwd=root,
                    capture_output=True, text=True,
                )
                with self.subTest(missing=excluded):
                    self.assertEqual(result.returncode == 0, excluded is None, result.stderr)
                    if excluded:
                        self.assertIn("missing required native CPU tests", result.stderr)

    def test_disabled_python_pch_and_missing_world_maker_fail_qualification(self):
        enabled = {
            "ATRINIK_BUILD_CLIENT": "ON", "ATRINIK_BUILD_SERVER": "ON",
            "ENABLE_WARNING_ERRORS": "ON", "ENABLE_PYTHON_PLUGIN": "ON",
            "ATRINIK_SERVER_ENABLE_PRECOMPILED_HEADERS": "ON", "GD_LIBRARY": "/native/libgd.dylib",
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build/macos-qualification"
            build.mkdir(parents=True)
            script = root / "features.py"
            script.write_text(embedded_python_script(0))
            for disabled in [None, *enabled]:
                cache = dict(enabled)
                if disabled:
                    cache[disabled] = "GD_LIBRARY-NOTFOUND" if disabled == "GD_LIBRARY" else "OFF"
                (build / "CMakeCache.txt").write_text("".join(
                    f"{key}:STRING={value}\n" for key, value in cache.items()
                ))
                result = subprocess.run(
                    [shutil.which("python3"), str(script)], cwd=root,
                    capture_output=True, text=True,
                )
                with self.subTest(disabled=disabled):
                    self.assertEqual(result.returncode == 0, disabled is None, result.stderr)
                    if disabled:
                        self.assertIn("absent", result.stderr)

    def test_existing_check_dispatch_calls_native_lane_without_gpu_authority(self):
        text = (ROOT / ".github/workflows/check.yml").read_text()
        self.assertIn("      native_macos_qualification:\n", text)
        block = text.split("  native-macos-qualification:\n", 1)[1].split("\n  gpu-qualification:", 1)[0]
        self.assertIn("github.event_name == 'workflow_dispatch' && inputs.native_macos_qualification", block)
        self.assertIn("contents: read", block)
        self.assertIn("uses: ./.github/workflows/llvm-macos-qualification.yml", block)
        self.assertNotIn("secrets:", block)
        self.assertNotIn("with:", block)
        self.assertNotIn("gpu_qualification", block)

    def test_manual_lane_keeps_production_features_and_gpu_boundary(self):
        text = WORKFLOW.read_text()
        self.assertIn("  workflow_call:\n  workflow_dispatch:", text)
        for trigger in ("  pull_request:", "  push:", "  schedule:"):
            self.assertNotIn(trigger, text)
        self.assertIn('test "$(uname -m)" = arm64', text)
        self.assertIn("contents: read", text)
        self.assertIn("--no-tests=error", text)
        self.assertIn("--output-junit", text)
        self.assertIn("-LE 'gpu-conformance|performance'", text)
        for option in (
            "ENABLE_PYTHON_PLUGIN", "ENABLE_PRECOMPILED_HEADERS", "ENABLE_WARNING_ERRORS",
            "ATRINIK_BUILD_CLIENT", "ATRINIK_BUILD_SERVER", "ENABLE_COVERAGE",
            "ENABLE_SANITIZERS", "CMAKE_INTERPROCEDURAL_OPTIMIZATION",
        ):
            self.assertNotIn("-D" + option, text)
        self.assertNotIn("ATRINIK_GPU_CONFORMANCE_QUALIFIED_HARDWARE", text)
        self.assertIn("prepare_macos_llvm.sh", text)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", text)
        self.assertIn("cmake --build build/macos-qualification --parallel 2", text)

    @unittest.skipUnless(shutil.which("bash"), "Bash unavailable")
    def test_embedded_shell_recipes_are_valid(self):
        text = WORKFLOW.read_text()
        recipes = re.findall(r"        run: \|\n((?:          .*\n|\n)+)", text)
        self.assertGreaterEqual(len(recipes), 8)
        for recipe in recipes:
            script = "\n".join(line[10:] for line in recipe.splitlines()) + "\n"
            result = subprocess.run(["bash", "-n"], input=script, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
