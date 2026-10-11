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
    def test_discovery_requires_each_supported_native_component(self):
        names = [
            "protocol-generate-check", "libatrinik-smoke", "atrinik-pathfinding",
            "client-help-parser",
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

            (evidence / "tests.json").write_text(json.dumps({
                "tests": [{"name": name} for name in [*names, "server-unit-object"]]
            }))
            result = subprocess.run([shutil.which("python3"), str(script)], cwd=root,
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unsupported server tests", result.stderr)

    def test_incorrect_platform_scope_or_disabled_required_features_fail(self):
        enabled = {
            "ATRINIK_BUILD_CLIENT": "ON", "ATRINIK_BUILD_SERVER": "OFF",
            "ENABLE_WARNING_ERRORS": "ON", "BUILD_TESTING": "ON",
            "CMAKE_BUILD_TYPE": "Release", "READLINE_LIBRARY": "/native/libreadline.dylib",
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
                    cache[disabled] = {
                        "ATRINIK_BUILD_SERVER": "ON", "CMAKE_BUILD_TYPE": "Debug",
                        "READLINE_LIBRARY": "READLINE_LIBRARY-NOTFOUND",
                    }.get(disabled, "OFF")
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
        self.assertIn("-DATRINIK_BUILD_CLIENT=ON -DATRINIK_BUILD_SERVER=OFF", text)
        for option in (
            "ENABLE_PYTHON_PLUGIN", "ENABLE_PRECOMPILED_HEADERS", "ENABLE_WARNING_ERRORS",
            "ENABLE_COVERAGE",
            "ENABLE_SANITIZERS", "CMAKE_INTERPROCEDURAL_OPTIMIZATION",
        ):
            self.assertNotIn("-D" + option, text)
        self.assertNotIn("ATRINIK_GPU_CONFORMANCE_QUALIFIED_HARDWARE", text)
        self.assertIn("prepare_macos_llvm.sh", text)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", text)
        self.assertIn("cmake --build build/macos-qualification --parallel 2 -- -k 0", text)
        self.assertNotIn("server/tools/dependencies.py", text)
        self.assertNotIn("FETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP", text)
        self.assertNotIn("server-plugin-python", text)
        self.assertNotIn("complete Classic Release graph", text)
        self.assertNotIn("GD_LIBRARY", text)

    @unittest.skipUnless(shutil.which("bash"), "Bash unavailable")
    def test_build_collects_reachable_errors_and_preserves_failure_through_tee(self):
        text = WORKFLOW.read_text()
        build_step = text.split("      - name: Build the production client, shared libraries and test executables\n", 1)[1]
        recipe = build_step.split("        run: |\n", 1)[1].split("      - name:", 1)[0]
        script = "\n".join(line[10:] for line in recipe.splitlines()) + "\n"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "build/macos-qualification-evidence").mkdir(parents=True)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text('''#!/usr/bin/env bash
printf '%s\\n' "$@" > arguments.txt
echo 'reachable native compile failure'
exit 7
''')
            fake_cmake.chmod(0o755)
            script = f'export PATH="{fake_bin}:$PATH"\n' + script
            result = subprocess.run(["bash"], input=script, cwd=root,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertEqual((root / "arguments.txt").read_text().splitlines(), [
                "--build", "build/macos-qualification", "--parallel", "2", "--", "-k", "0",
            ])
            self.assertIn("reachable native compile failure",
                          (root / "build/macos-qualification-evidence/build.log").read_text())

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
