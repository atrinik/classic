# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake" / "AtrinikLLVM.cmake"
TOOLCHAIN = ROOT / "cmake" / "toolchains" / "llvm-macos.cmake"
TOOLS = {
    "CMAKE_C_COMPILER": "clang", "CMAKE_CXX_COMPILER": "clang++",
    "CMAKE_AR": "llvm-ar", "CMAKE_RANLIB": "llvm-ranlib",
    "CMAKE_LINKER": "ld64.lld", "CMAKE_NM": "llvm-nm",
    "CMAKE_STRIP": "llvm-strip", "CMAKE_OBJCOPY": "llvm-objcopy",
    "CMAKE_OBJDUMP": "llvm-objdump", "CMAKE_READELF": "llvm-readelf",
}


@unittest.skipUnless(shutil.which("cmake"), "CMake unavailable")
class MacOSLLVMToolchainTests(unittest.TestCase):
    """Darwin policy fixtures; no claim of compiling against an actual SDK."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="macos llvm ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sdk = self.root / "MacOSX.sdk"
        self.sdk.mkdir()
        self.apple_usr = self.root / "Xcode/usr"
        (self.apple_usr / "include/c++/v1").mkdir(parents=True)
        self.runtime = self.root / "libclang_rt.osx.a"
        self.runtime.write_bytes(b"archive fixture")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.tool = self.bin / "tool"
        self.tool.write_text(
            '#!/bin/sh\nif test "$1" = --rtlib=compiler-rt; then\n'
            f"  printf '%s\\n' '{self.runtime}'\n"
            "else\n  printf 'LLVM version 23.1.3\\n'\nfi\n"
        )
        self.tool.chmod(0o755)
        for name in TOOLS.values():
            (self.bin / name).symlink_to(self.tool)
        self.xcrun = self.bin / "xcrun"
        self.xcrun.write_text(
            '#!/bin/sh\nif test "$3" = --show-sdk-path; then\n'
            f"  printf '%s\\n' '{self.sdk}'\n"
            f"else\n  printf '%s\\n' '{self.apple_usr}/bin/clang++'\nfi\n"
        )
        self.xcrun.chmod(0o755)

    def script(self, body):
        path = self.root / "test.cmake"
        path.write_text(f'include("{MODULE}")\n' + body)
        return subprocess.run([shutil.which("cmake"), "-P", str(path)], capture_output=True, text=True)

    def settings(self):
        return f'''
set(CMAKE_HOST_SYSTEM_NAME Darwin)
set(CMAKE_PROGRAM_PATH "{self.bin}")
include("{TOOLCHAIN}")
'''

    def test_darwin_default_uses_native_macos_toolchain(self):
        result = self.script(f'''
set(CMAKE_HOST_SYSTEM_NAME Darwin)
atrinik_initialize_llvm_toolchain()
if (NOT CMAKE_TOOLCHAIN_FILE STREQUAL "{TOOLCHAIN}")
    message(FATAL_ERROR "wrong Darwin selection")
endif ()
''')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_explicit_darwin_cross_toolchain_is_preserved(self):
        result = self.script('''
set(CMAKE_HOST_SYSTEM_NAME Darwin)
set(CMAKE_TOOLCHAIN_FILE custom.cmake)
atrinik_initialize_llvm_toolchain()
if (NOT CMAKE_TOOLCHAIN_FILE STREQUAL "custom.cmake")
    message(FATAL_ERROR "explicit toolchain overwritten")
endif ()
''')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_darwin_host_windows_target_avoids_macos_selection(self):
        result = self.script('''
set(CMAKE_HOST_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_NAME Windows)
atrinik_initialize_llvm_toolchain()
if (CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR "cross target selected native toolchain")
endif ()
''')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_macos_binds_macho_lld_sdk_and_system_cxx_headers(self):
        body = self.settings()
        for variable, name in TOOLS.items():
            body += f'''
if (NOT {variable} STREQUAL "{self.bin / name}")
    message(FATAL_ERROR "wrong {variable}")
endif ()
'''
        body += f'''
if (NOT CMAKE_OSX_SYSROOT STREQUAL "{self.sdk}" OR
        NOT CMAKE_CXX_FLAGS_INIT MATCHES "-nostdinc[+]?[+]" OR
        NOT CMAKE_CXX_FLAGS_INIT MATCHES "Xcode/usr/include/c[+][+]/v1")
    message(FATAL_ERROR "platform SDK/libc++ selection changed")
endif ()
foreach (flags CMAKE_EXE_LINKER_FLAGS_INIT CMAKE_SHARED_LINKER_FLAGS_INIT CMAKE_MODULE_LINKER_FLAGS_INIT)
    if (NOT "${{${{flags}}}}" MATCHES "--rtlib=compiler-rt" OR
            NOT "${{${{flags}}}}" MATCHES "ld64.lld" OR
            "${{${{flags}}}}" MATCHES "--unwindlib=|flto|fprofile|-O[0-9]")
        message(FATAL_ERROR "incorrect Darwin linker flags")
    endif ()
endforeach ()
'''
        result = self.script(body)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_explicit_sdk_deployment_and_architectures_are_preserved(self):
        result = self.script('''
set(CMAKE_OSX_SYSROOT explicit-sdk)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
''' + self.settings() + '''
if (NOT CMAKE_OSX_SYSROOT STREQUAL "explicit-sdk" OR
        NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "13.0" OR
        NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64;x86_64")
    message(FATAL_ERROR "caller platform selection overwritten")
endif ()
''')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_linux_host_cannot_select_macos_toolchain(self):
        result = self.script(f'set(CMAKE_HOST_SYSTEM_NAME Linux)\ninclude("{TOOLCHAIN}")\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("native macOS host", result.stderr)

    def test_missing_compiler_rt_and_platform_headers_fail(self):
        self.runtime.unlink()
        result = self.script(self.settings())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("compiler-rt builtins", result.stderr)
        self.runtime.write_bytes(b"archive fixture")
        (self.apple_usr / "include/c++/v1").rmdir()
        result = self.script(self.settings())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("platform libc++ headers", result.stderr)

    def test_apple_clang_and_other_versions_fail(self):
        for version in ("Apple clang version 23.1.3", "clang version 23.1.2", "clang version 23.1.30"):
            with self.subTest(version=version):
                self.tool.write_text(f"#!/bin/sh\nprintf '%s\\n' '{version}'\n")
                result = self.script(self.settings())
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("upstream LLVM 23.1.3", result.stderr)

    @unittest.skipUnless(shutil.which("shasum"), "SHA-256 utility unavailable")
    def test_preparation_rejects_tampered_archive_before_extraction(self):
        fake_bin = self.root / "download-bin"
        fake_bin.mkdir()
        commands = {
            "uname": "#!/bin/sh\ncase $1 in -s) echo Darwin ;; -m) echo arm64 ;; esac\n",
            "curl": "#!/bin/sh\nfor arg do output=$arg; done\nprintf tampered > \"$output\"\n",
            "tar": f"#!/bin/sh\ntouch '{self.root / 'extracted'}'\n",
        }
        for name, content in commands.items():
            path = fake_bin / name
            path.write_text(content)
            path.chmod(0o755)
        environment = dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ["PATH"])
        result = subprocess.run(
            ["bash", str(ROOT / "tools/ci/prepare_macos_llvm.sh"), str(self.root / "candidate")],
            env=environment, capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FAILED", result.stdout)
        self.assertFalse((self.root / "extracted").exists())

    def test_preparation_rejects_linux_without_writing(self):
        if shutil.which("uname") and subprocess.check_output(["uname", "-s"], text=True).strip() != "Darwin":
            destination = self.root / "uncreated"
            result = subprocess.run(
                ["bash", str(ROOT / "tools/ci/prepare_macos_llvm.sh"), str(destination)],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(destination.exists())


@unittest.skipUnless(shutil.which("cmake"), "CMake unavailable")
class MacOSRuntimeNotices(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.notices = self.root / "llvm/share/licenses/llvm"
        self.notices.mkdir(parents=True)
        compiler = self.root / "llvm/bin/clang"
        compiler.parent.mkdir()
        compiler.touch()
        self.names = ("llvm", "clang", "lld", "compiler-rt")
        for name in self.names:
            (self.notices / (name + "-LICENSE.TXT")).write_text(name + " complete notice\n")
        (self.root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.21)
project(MacOSNoticeFixture NONE)
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_C_COMPILER "{compiler}")
include("{ROOT / 'cmake/InstallLLVMRuntime.cmake'}")
atrinik_install_llvm_runtime_notices(share/doc/atrinik AtrinikClient)
''')

    def configure(self):
        return subprocess.run(
            ["cmake", "-S", str(self.root), "-B", str(self.root / "build")],
            capture_output=True, text=True,
        )

    def test_complete_darwin_runtime_notices_are_installed(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        prefix = self.root / "install"
        subprocess.run(
            ["cmake", "--install", str(self.root / "build"), "--prefix", str(prefix),
             "--component", "AtrinikClient"], check=True, capture_output=True,
        )
        installed = prefix / "share/doc/atrinik/licenses/llvm"
        self.assertEqual(sorted(p.name for p in installed.iterdir()),
                         sorted(name + "-LICENSE.TXT" for name in self.names))
        for path in installed.iterdir():
            self.assertEqual(path.read_bytes(), (self.notices / path.name).read_bytes())

    def test_missing_darwin_compiler_rt_notice_fails(self):
        (self.notices / "compiler-rt-LICENSE.TXT").unlink()
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("runtime installation requires complete notice", result.stderr)


if __name__ == "__main__":
    unittest.main()
