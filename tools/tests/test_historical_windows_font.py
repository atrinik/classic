# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from tools.ci import stage_historical_windows_font as baseline

ROOT = Path(__file__).resolve().parents[2]
HELPER = Path(__file__).with_name("fixtures") / "windows-font-baseline/stage_windows_runtime.py.txt"


def objdump_output(imports=(), *, architecture="pei-x86-64", delay="00000000 00000000"):
    return (f"fixture: file format {architecture}\n"
            f"Entry d {delay} Delay Import Directory\n" +
            "".join(f"  DLL Name: {name}\n" for name in imports))


class HistoricalWindowsFontTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        self.notices = self.root / "notices"
        (self.notices / "SDL3").mkdir(parents=True)
        (self.notices / "SDL3/LICENSE.txt").write_text("original historical SDL notice")
        self.binary = self.root / "client-font-render-probe.exe"
        self.binary.write_text(objdump_output(["SDL3.dll", "libgcc_s_seh-1.dll"]))
        (self.runtime / "SDL3.dll").write_text(objdump_output([
            "IMM32.dll", "OLEAUT32.dll", "SETUPAPI.dll", "MSVCRT.dll", "KERNEL32.dll",
        ]))
        (self.runtime / "libgcc_s_seh-1.dll").write_text(objdump_output(["KERNEL32.dll"]))
        self.output = self.root / "bundle"
        self.objdump = self.root / "objdump"
        self.objdump.write_text("#!/usr/bin/env python3\nimport pathlib,sys\n"
                                "assert sys.argv[1] == '-p'\n"
                                "print(pathlib.Path(sys.argv[2]).read_text())\n")
        self.objdump.chmod(0o755)

    def stage(self, **kwargs):
        return baseline.stage_fixture(HELPER, self.binary, self.runtime, self.notices,
                                      self.output, str(self.objdump), "exact historical GCC", **kwargs)

    def assert_no_success(self):
        self.assertFalse((self.output / "historical-font-inventory.json").exists())

    def test_exact_helper_recursive_closure_copied_bytes_and_notices(self):
        self.assertEqual(hashlib.sha256(HELPER.read_bytes()).hexdigest(), baseline.HELPER_SHA256)
        manifest = self.stage()
        self.assertEqual(set(manifest["pe_files"]), {
            "client-font-render-probe.exe", "sdl3.dll", "libgcc_s_seh-1.dll",
        })
        self.assertEqual(manifest["additional_system_dlls"], ["imm32.dll", "oleaut32.dll", "setupapi.dll"])
        self.assertEqual(manifest["historical_helper_revision"], baseline.HELPER_REVISION)
        self.assertEqual(manifest["producer_image"], baseline.IMAGE)
        self.assertFalse(manifest["notice_coverage"]["complete"])
        self.assertFalse(manifest["native_windows_rendering_verified"])
        for name, record in manifest["pe_files"].items():
            path = next(path for path in self.output.iterdir() if path.name.lower() == name)
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), record["sha256"])
        notice = self.output / "licenses/SDL3/LICENSE.txt"
        self.assertEqual(notice.read_bytes(), (self.notices / "SDL3/LICENSE.txt").read_bytes())
        self.assertEqual(manifest["notices"]["SDL3/LICENSE.txt"], hashlib.sha256(notice.read_bytes()).hexdigest())

    def test_altered_helper_rejected_before_execution(self):
        changed = self.root / "changed.py"
        marker = self.root / "executed"
        changed.write_bytes(HELPER.read_bytes() + f"\nopen({str(marker)!r},'w').write('unsafe')\n".encode())
        with self.assertRaisesRegex(baseline.BaselineError, "checksum mismatch"):
            baseline.load_helper(changed)
        self.assertFalse(marker.exists())

    def test_load_executes_verified_buffer_without_rereading(self):
        with mock.patch.object(baseline, "regular_bytes", side_effect=[HELPER.read_bytes(), b"raise RuntimeError('changed')"]) as read:
            helper = baseline.load_helper(HELPER)
        self.assertTrue(callable(helper.stage_runtime))
        read.assert_called_once_with(HELPER)

    def test_unknown_private_or_unapproved_current_system_dll_rejected(self):
        for dependency in ("private-runtime.dll", "dwrite.dll"):
            with self.subTest(dependency=dependency):
                self.binary.write_text(objdump_output([dependency]))
                with self.assertRaisesRegex(RuntimeError, "unresolved runtime DLL"):
                    self.stage()
                self.assert_no_success()
                for path in self.output.iterdir():
                    path.unlink()
                self.output.rmdir()

    def test_architecture_delay_and_import_validation_fail_closed(self):
        helper = baseline.load_helper(HELPER)
        invalid = [
            objdump_output(architecture="pei-i386"),
            objdump_output(delay="00001000 00000008"),
            objdump_output().replace("Entry d", "Entry c"),
            objdump_output(["api-ms-win-private.pyd"]),
            objdump_output(["api-ms-win-../../private.dll"]),
            objdump_output().replace("Entry d", "Entry d invalid"),
            objdump_output() + " DLL Name:\n",
        ]
        for output in invalid:
            with self.subTest(output=output), self.assertRaises(baseline.BaselineError):
                baseline.parse_inspection(helper, output)
        self.assertEqual(baseline.parse_inspection(helper, objdump_output(["api-ms-win-core-file-l1-1-0.dll"])),
                         {"api-ms-win-core-file-l1-1-0.dll"})

    def test_policy_cannot_expand_or_lose_required_additions(self):
        policy = self.root / "policy.py"
        policy.write_text("SYSTEM_DLLS={'imm32.dll','oleaut32.dll'}")
        with self.assertRaisesRegex(baseline.BaselineError, "absent from current system policy"):
            baseline.current_policy(policy)

    def test_copy_mutation_or_unexpected_inventory_rejected(self):
        for tamper in ("bytes", "extra"):
            with self.subTest(tamper=tamper):
                helper = baseline.load_helper(HELPER)
                original = helper.stage_runtime

                def stage(*args):
                    result = original(*args)
                    path = self.output / ("SDL3.dll" if tamper == "bytes" else "unexpected.dll")
                    path.write_text(objdump_output(["KERNEL32.dll"]))
                    return result

                helper.stage_runtime = stage
                with mock.patch.object(baseline, "load_helper", return_value=helper):
                    with self.assertRaisesRegex(baseline.BaselineError, "copied historical fixture bytes changed|inventory differs"):
                        self.stage()
                self.assert_no_success()
                for path in self.output.iterdir():
                    path.unlink()
                self.output.rmdir()

    def test_inspection_source_mutation_rejected(self):
        helper = baseline.load_helper(HELPER)
        result = subprocess.CompletedProcess([], 0, objdump_output(), "")

        def mutate(*args, **kwargs):
            self.binary.write_text("changed after inspection")
            return result

        with mock.patch.object(baseline.subprocess, "run", side_effect=mutate):
            with self.assertRaisesRegex(baseline.BaselineError, "changed during inspection"):
                baseline.inspect(helper, str(self.objdump), self.binary)

    def test_notice_symlink_case_collision_and_empty_tree_rejected(self):
        for failure in ("symlink", "case", "empty"):
            with self.subTest(failure=failure):
                source = self.root / failure
                source.mkdir()
                if failure == "symlink":
                    (source / "COPYING").symlink_to(self.notices / "SDL3/LICENSE.txt")
                elif failure == "case":
                    (source / "COPYING").write_text("one")
                    (source / "copying").write_text("two")
                with self.assertRaises(baseline.BaselineError):
                    baseline.copy_notices(source, self.root / f"copied-{failure}")

    def test_cli_rejects_wrong_producer_inspector_or_sdk(self):
        for image, objdump, runtime in (("other-image", baseline.OBJDUMP, str(baseline.PREFIX / "bin")),
                                       (baseline.IMAGE, "other-objdump", str(baseline.PREFIX / "bin")),
                                       (baseline.IMAGE, baseline.OBJDUMP, str(self.runtime))):
            with self.subTest(image=image, objdump=objdump, runtime=runtime):
                result = subprocess.run([sys.executable, str(ROOT / "tools/ci/stage_historical_windows_font.py"),
                                         "--historical-helper", str(HELPER), "--producer-image", image,
                                         "--objdump", objdump, "--runtime-dir", runtime,
                                         "--notice-dir", str(baseline.PREFIX / "share/licenses"),
                                         "--output-dir", str(self.output), str(self.binary)],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires", result.stderr)
                self.assert_no_success()


if __name__ == "__main__":
    unittest.main()
