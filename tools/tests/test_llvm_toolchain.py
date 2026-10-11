# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake" / "AtrinikLLVM.cmake"
TOOLCHAIN = ROOT / "cmake" / "toolchains" / "llvm.cmake"


@unittest.skipUnless(shutil.which("cmake"), "CMake unavailable")
class LLVMToolchainTests(unittest.TestCase):
    """Exercise selection/rejection as CMake scripts, without compiling code."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.capture = self.root / "options.txt"
        self.tool = self.root / "llvm-tool"
        self.tool.write_text("#!/bin/sh\nprintf 'LLVM version 23.1.3\\n'\n")
        self.tool.chmod(0o755)

    def script(self, body):
        path = self.root / "test.cmake"
        path.write_text(f'include("{MODULE}")\n' + body)
        return subprocess.run([shutil.which("cmake"), "-P", str(path)], capture_output=True, text=True)

    def settings(self, platform="Linux", compiler="Clang", version="23.1.3"):
        return f'''
set(CMAKE_C_COMPILER_ID "{compiler}")
set(CMAKE_C_COMPILER_VERSION "{version}")
set(CMAKE_SYSTEM_NAME "{platform}")
set(CMAKE_AR "{self.tool}")
set(CMAKE_RANLIB "{self.tool}")
set(CMAKE_LINKER "{self.tool}")
set(CMAKE_NM "{self.tool}")
set(CMAKE_STRIP "{self.tool}")
set(CMAKE_OBJCOPY "{self.tool}")
set(CMAKE_OBJDUMP "{self.tool}")
set(CMAKE_READELF "{self.tool}")
function(add_link_options)
    file(WRITE "{self.capture}" "${{ARGV}}")
endfunction()
'''

    def test_native_default_selects_pinned_toolchain_from_module_directory(self):
        result = self.script(f'''
set(CMAKE_HOST_SYSTEM_NAME Linux)
atrinik_initialize_llvm_toolchain()
file(WRITE "{self.capture}" "${{CMAKE_TOOLCHAIN_FILE}}")
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.capture.read_text(), str(TOOLCHAIN))

    def test_explicit_portable_toolchain_and_cpu_flags_are_preserved(self):
        result = self.script(self.settings() + f'''
set(CMAKE_TOOLCHAIN_FILE portable.cmake)
set(CMAKE_C_FLAGS "-march=x86-64 -mtune=generic")
atrinik_initialize_llvm_toolchain()
atrinik_require_llvm()
if (NOT CMAKE_TOOLCHAIN_FILE STREQUAL "portable.cmake" OR
        NOT CMAKE_C_FLAGS STREQUAL "-march=x86-64 -mtune=generic")
    message(FATAL_ERROR "portable selection changed")
endif ()
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.capture.read_text(), "--rtlib=compiler-rt;--unwindlib=libunwind;-fuse-ld=lld")

    def test_windows_toolchain_avoids_native_linux_runtime_flags(self):
        result = self.script(self.settings("Windows") + '''
set(CMAKE_TOOLCHAIN_FILE llvm-mingw.cmake)
atrinik_initialize_llvm_toolchain()
atrinik_require_llvm()
if (NOT CMAKE_TOOLCHAIN_FILE STREQUAL "llvm-mingw.cmake")
    message(FATAL_ERROR "Windows selection changed")
endif ()
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.capture.exists())

    def test_gcc_and_mismatched_clang_are_rejected(self):
        for compiler, version in (("GNU", "15.2.0"), ("Clang", "23.1.2"), ("Clang", "23.1.30")):
            with self.subTest(compiler=compiler, version=version):
                result = self.script(self.settings(compiler=compiler, version=version) + "atrinik_require_llvm()\n")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("require Clang 23.1.3", result.stderr)

    def test_mismatched_archive_tools_are_rejected(self):
        self.tool.write_text("#!/bin/sh\nprintf 'GNU ar 2.45\\n'\n")
        result = self.script(self.settings() + "atrinik_require_llvm()\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CMAKE_AR must be LLVM 23.1.3", result.stderr)

    def test_native_toolchain_binds_all_llvm_inspection_tools(self):
        names = {
            "CMAKE_C_COMPILER": "clang", "CMAKE_CXX_COMPILER": "clang++",
            "CMAKE_AR": "llvm-ar", "CMAKE_RANLIB": "llvm-ranlib", "CMAKE_LINKER": "ld.lld",
            "CMAKE_NM": "llvm-nm", "CMAKE_STRIP": "llvm-strip", "CMAKE_OBJCOPY": "llvm-objcopy",
            "CMAKE_OBJDUMP": "llvm-objdump", "CMAKE_READELF": "llvm-readelf",
        }
        for name in names.values():
            (self.root / name).symlink_to(self.tool)
        body = f'''
set(CMAKE_HOST_SYSTEM_NAME Linux)
set(CMAKE_PROGRAM_PATH "{self.root}")
include("{TOOLCHAIN}")
'''
        for variable, name in names.items():
            body += f'''
if (NOT {variable} STREQUAL "{self.root / name}")
    message(FATAL_ERROR "Unexpected {variable}: ${{{variable}}}")
endif ()
'''
        result = self.script(body)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wrong_inspection_utility_version_is_rejected(self):
        wrong = self.root / "old-llvm-tool"
        wrong.write_text("#!/bin/sh\nprintf 'LLVM version 23.1.2\\n'\n")
        wrong.chmod(0o755)
        for variable in ("CMAKE_NM", "CMAKE_STRIP", "CMAKE_OBJCOPY", "CMAKE_OBJDUMP", "CMAKE_READELF"):
            with self.subTest(variable=variable):
                result = self.script(self.settings() + f'set({variable} "{wrong}")\natrinik_require_llvm()\n')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"{variable} must be LLVM 23.1.3", result.stderr)

    def test_missing_llvm_fails_toolchain_selection(self):
        empty = self.root / "empty"
        empty.mkdir()
        result = self.script(f'''
set(CMAKE_HOST_SYSTEM_NAME Linux)
set(CMAKE_FIND_ROOT_PATH "{empty}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM ONLY)
include("{TOOLCHAIN}")
''')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CMAKE_C_COMPILER", result.stderr)


if __name__ == "__main__":
    unittest.main()
