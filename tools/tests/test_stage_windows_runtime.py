from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import subprocess


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
                "path.exe": {"imagehlp.dll", "KERNEL32.dll", "LIBCRYPTO.DLL"},
                "libcrypto.dll": {"zlib1.dll", "api-ms-win-core-file-l1-1-0.dll"},
                "zlib1.dll": {"ucrtbase.dll"},
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

    def test_llvm_parser_accepts_normal_and_delayed_imports(self) -> None:
        output = """File: fixture.exe
Format: COFF-x86-64
Arch: x86_64
AddressSize: 64bit
Import {
  Name: KERNEL32.dll
  ImportLookupTableRVA: 0x17290
  Symbol: GetLastError (627)
}
DelayImport {
  Name: libcrypto-4-x64.dll
  Attributes: 0x1
  Import {
    Symbol: SSL_connect (0)
    Address: 0x180001AF7
  }
  Import {
    Symbol: SSL_read (1)
    Address: 0x180001AD3
  }
}
"""
        self.assertEqual(
            stage_windows_runtime.parse_imports(output),
            {"KERNEL32.dll", "libcrypto-4-x64.dll"},
        )

    def test_llvm_parser_rejects_unrecognized_and_malformed_output(self) -> None:
        for output in ("DLL Name: fixture.dll", "Format: elf64-x86-64\n",
                       "Format: COFF-i386\n", "Format: COFF-x86-64\nImport {\n",
                       "Format: COFF-x86-64\nImport {\nName: ../bad.dll\n}\n",
                       "Format: COFF-x86-64\nImport {\n}\n",
                       "Format: COFF-x86-64\nImport {\nName: a.dll\nName: b.dll\n}\n",
                       "Format: COFF-x86-64\nImport {\nName: a.dll\nImport {\n}\n}\n",
                       "Format: COFF-x86-64\nDelayImport {\nName: a.dll\nImport {\nName: b.dll\n}\n}\n",
                       "Format: COFF-x86-64\nDelayImport {\nName: a.dll\nImport {\nSymbol: foo\n}\n",
                       "Format: COFF-x86-64\nDelayImport {\nName: a.dll\nUnknown {\n}\n}\n"):
            with self.subTest(output=output), self.assertRaises(stage_windows_runtime.StageError):
                stage_windows_runtime.parse_imports(output)

    def test_no_imports_is_valid_only_with_known_pe_format(self) -> None:
        self.assertEqual(stage_windows_runtime.parse_imports("Format: COFF-x86-64\n"), set())

    def test_inspector_uses_llvm_coff_import_mode_and_propagates_failure(self) -> None:
        success = subprocess.CompletedProcess([], 0, "Format: COFF-x86-64\n", "")
        with patch.object(stage_windows_runtime.subprocess, "run", return_value=success) as run:
            self.assertEqual(stage_windows_runtime.imported_dlls("llvm-readobj", Path("fixture.exe")), set())
            self.assertEqual(run.call_args.args[0], ["llvm-readobj", "--coff-imports", "fixture.exe"])
        failure = subprocess.CompletedProcess([], 1, "", "invalid PE")
        with patch.object(stage_windows_runtime.subprocess, "run", return_value=failure):
            with self.assertRaisesRegex(stage_windows_runtime.StageError, "invalid PE"):
                stage_windows_runtime.imported_dlls("llvm-readobj", Path("fixture.exe"))

    def test_system_ucrt_and_external_python_crt_are_not_staged(self) -> None:
        for name in ("ucrtbase.dll", "api-ms-win-crt-runtime-l1-1-0.dll"):
            self.assertTrue(stage_windows_runtime.is_system_dll(name))
        self.assertFalse(stage_windows_runtime.is_system_dll("msvcrt.dll"))
        self.assertTrue(stage_windows_runtime.is_system_dll("msvcrt.dll", external_python=True))

    def test_inbox_driver_import_is_parsed_and_only_explicit_system_driver_is_accepted(self) -> None:
        imports = stage_windows_runtime.parse_imports("Format: COFF-x86-64\nImport {\nName: WINSPOOL.DRV\n}\n")
        self.assertEqual(imports, {"WINSPOOL.DRV"})
        self.assertTrue(stage_windows_runtime.is_system_dll("WINSPOOL.DRV"))
        self.assertFalse(stage_windows_runtime.is_system_dll("unknown.drv"))
        self.assertFalse(stage_windows_runtime.is_system_dll("api-ms-win-private.drv"))
        self.assertFalse(stage_windows_runtime.is_system_dll("ext-ms-win-private.pyd"))
        for name in ("dwrite.dll", "d3d11.dll", "netapi32.dll", "userenv.dll", "usp10.dll", "avrt.dll"):
            self.assertTrue(stage_windows_runtime.is_system_dll(name))

    def test_gcc_runtime_in_imports_or_unused_prefix_fails(self) -> None:
        for name in ("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libssp-0.dll",
                     "libgomp-1.dll", "libatomic-1.dll", "libquadmath-0.dll"):
            for unused in (False, True):
                with self.subTest(name=name, unused=unused), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    runtime = root / "runtime"
                    runtime.mkdir()
                    executable = root / "path.exe"
                    executable.write_bytes(b"exe")
                    if unused:
                        (runtime / name).write_bytes(b"stale")
                    with self.assertRaisesRegex(stage_windows_runtime.StageError, "GCC runtime DLL"):
                        stage_windows_runtime.stage_runtime(
                            [executable], runtime, root / "output",
                            lambda binary: set() if unused else {name})

    def test_dlls_only_preserves_root_plugin_ownership_and_closes_external_python(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime, python = root / "runtime", root / "python"
            runtime.mkdir()
            python.mkdir()
            executable, plugin = root / "server.exe", root / "plugin_python.dll"
            executable.write_bytes(b"exe")
            plugin.write_bytes(b"plugin")
            (python / "python314.dll").write_bytes(b"python")
            (runtime / "libcurl.dll").write_bytes(b"curl")
            (runtime / "unused.dll").write_bytes(b"unused")
            imports = {"server.exe": {"plugin_python.dll", "libcurl.dll"},
                       "plugin_python.dll": {"python314.dll"},
                       "python314.dll": {"VCRUNTIME140.dll"},
                       "VCRUNTIME140.dll": {"ucrtbase.dll"}, "libcurl.dll": set()}
            (python / "VCRUNTIME140.dll").write_bytes(b"external runtime")
            staged = stage_windows_runtime.stage_runtime(
                [executable, plugin], [runtime, python], root / "output",
                lambda binary: imports[binary.name], dlls_only=True)
            self.assertEqual(staged, ["libcurl.dll", "python314.dll", "VCRUNTIME140.dll"])

    def test_import_name_cannot_escape_runtime_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = root / "runtime"
            runtime.mkdir()
            executable = root / "path.exe"
            executable.write_bytes(b"exe")
            with self.assertRaisesRegex(stage_windows_runtime.StageError, "invalid imported DLL name"):
                stage_windows_runtime.stage_runtime(
                    [executable], runtime, root / "output", lambda binary: {"../escape.dll"})

    def test_duplicate_case_insensitive_dll_across_runtime_roots_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime, python = root / "runtime", root / "python"
            runtime.mkdir()
            python.mkdir()
            (runtime / "zlib1.dll").write_bytes(b"one")
            (python / "ZLIB1.dll").write_bytes(b"two")
            executable = root / "path.exe"
            executable.write_bytes(b"exe")
            with self.assertRaisesRegex(stage_windows_runtime.StageError, "duplicate case-insensitive"):
                stage_windows_runtime.stage_runtime(
                    [executable], [runtime, python], root / "output", lambda binary: set())


if __name__ == "__main__":
    unittest.main()
