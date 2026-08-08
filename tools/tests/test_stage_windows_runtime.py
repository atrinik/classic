from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "ci" / "stage_windows_runtime.py"
SPEC = importlib.util.spec_from_file_location("stage_windows_runtime", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
stage_windows_runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage_windows_runtime)


class StageWindowsRuntimeTests(unittest.TestCase):
    def test_recursive_closure_is_case_insensitive_and_excludes_system_dlls(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = root / "runtime"
            output = root / "output"
            runtime.mkdir()
            executable = root / "path.exe"
            executable.write_bytes(b"exe")
            (runtime / "libcrypto.dll").write_bytes(b"crypto")
            (runtime / "zlib1.dll").write_bytes(b"zlib")
            imports = {
                "path.exe": {"KERNEL32.dll", "LIBCRYPTO.DLL"},
                "libcrypto.dll": {"zlib1.dll", "api-ms-win-core-file-l1-1-0.dll"},
                "zlib1.dll": {"msvcrt.dll"},
            }

            staged = stage_windows_runtime.stage_runtime(
                [executable],
                runtime,
                output,
                lambda binary: imports[binary.name],
            )

            self.assertEqual(staged, ["libcrypto.dll", "path.exe", "zlib1.dll"])
            self.assertEqual((output / "libcrypto.dll").read_bytes(), b"crypto")

    def test_unknown_non_system_dependency_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = root / "runtime"
            runtime.mkdir()
            executable = root / "path.exe"
            executable.write_bytes(b"exe")
            with self.assertRaisesRegex(
                stage_windows_runtime.StageError, "unresolved runtime DLL"
            ):
                stage_windows_runtime.stage_runtime(
                    [executable],
                    runtime,
                    root / "output",
                    lambda binary: {"missing.dll"},
                )

    def test_nonempty_output_directory_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = root / "runtime"
            output = root / "output"
            runtime.mkdir()
            output.mkdir()
            (output / "stale.dll").write_bytes(b"stale")
            executable = root / "path.exe"
            executable.write_bytes(b"exe")
            with self.assertRaisesRegex(
                stage_windows_runtime.StageError, "output directory is not empty"
            ):
                stage_windows_runtime.stage_runtime(
                    [executable], runtime, output, lambda binary: set()
                )

    def test_objdump_parser_accepts_pe_import_table_spacing(self) -> None:
        output = "\tDLL Name: KERNEL32.dll\n  DLL Name: libcrypto-4-x64.dll\n"
        self.assertEqual(
            {match.group(1) for match in stage_windows_runtime.DLL_NAME.finditer(output)},
            {"KERNEL32.dll", "libcrypto-4-x64.dll"},
        )


if __name__ == "__main__":
    unittest.main()
