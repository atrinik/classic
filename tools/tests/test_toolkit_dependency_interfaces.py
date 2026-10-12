# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja") and shutil.which("cc"),
                     "CMake, Ninja and a fixture C compiler are required")
class ToolkitDependencyInterfaceTests(unittest.TestCase):
    """Synthetic target fixtures, not native application or SDK acceptance."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="toolkit dependencies ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.prefix = self.root / "readline"
        (self.prefix / "include/readline").mkdir(parents=True)
        (self.prefix / "lib").mkdir()
        (self.prefix / "include/readline/readline.h").write_text(
            "#define ATRINIK_READLINE_FIXTURE 1\n")
        # Static fixture consumers exercise compilation without linking an
        # application against this discovery-only placeholder archive.
        (self.prefix / "lib/libreadline.a").touch()
        self.openssl = self.root / "openssl"
        (self.openssl / "include/openssl").mkdir(parents=True)
        (self.openssl / "include/openssl/types.h").write_text(
            "#define ATRINIK_OPENSSL_FIXTURE 1\ntypedef struct ssl_st SSL;\n")
        self.sdk = self.root / "sdk"
        (self.sdk / "include/readline").mkdir(parents=True)
        (self.sdk / "include/readline/readline.h").write_text(
            '#error "SDK libedit header must not replace selected GNU Readline"\n')
        text = (ROOT / "libatrinik/CMakeLists.txt").read_text()
        discovery = text.split("find_library(READLINE_LIBRARY readline)\n", 1)[1]
        discovery = "find_library(READLINE_LIBRARY readline)\n" + discovery.split(
            "find_package(OpenSSL REQUIRED)", 1)[0]
        links = text.split("# Link readline if available.\n", 1)[1].split(
            "if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)", 1)[0]
        (self.root / "core.c").write_text(
            "#include <readline/readline.h>\nint core(void) { return ATRINIK_READLINE_FIXTURE; }\n")
        (self.root / "consumer.c").write_text(
            "#include <openssl/types.h>\n"
            '#ifndef ATRINIK_OPENSSL_FIXTURE\n#error "selected OpenSSL headers required"\n#endif\n'
            "SSL *consumer(void) { return (SSL *)0; }\n")
        (self.root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.21)
project(ToolkitDependencies LANGUAGES C)
{discovery}
add_library(OpenSSL::SSL INTERFACE IMPORTED)
add_library(OpenSSL::Crypto INTERFACE IMPORTED)
set_target_properties(OpenSSL::Crypto PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "{self.openssl}/include")
set(ATRINIK_TOOLKIT core)
add_library(core STATIC core.c)
target_compile_options(core PRIVATE "-idirafter{self.sdk}/include")
{links}
add_library(consumer STATIC consumer.c)
target_link_libraries(consumer PRIVATE core)
export(TARGETS core FILE "${{CMAKE_BINARY_DIR}}/CoreTargets.cmake")
''')

    def configure(self, *options):
        return subprocess.run([
            shutil.which("cmake"), "-S", str(self.root), "-B", str(self.root / "build"),
            "-G", "Ninja", f"-DCMAKE_C_COMPILER={shutil.which('cc')}",
            f"-DCMAKE_PREFIX_PATH={self.prefix}", *options,
        ], capture_output=True, text=True)

    def test_selected_readline_headers_and_public_openssl_compile_for_consumers(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([shutil.which("cmake"), "--build", str(self.root / "build"),
                                 "--parallel", "2"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        exported = (self.root / "build/CoreTargets.cmake").read_text()
        self.assertIn("OpenSSL::Crypto", exported)
        self.assertNotIn(str(self.openssl), exported)
        self.assertNotIn(str(self.prefix / "include"), exported)

    def test_selected_library_without_matching_header_path_is_rejected(self):
        result = self.configure(f"-DREADLINE_INCLUDE_DIR={self.root}/missing-headers")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires matching Readline headers", result.stderr)

    def test_selected_library_rejects_explicit_headers_from_another_prefix(self):
        other = self.root / "other-readline/include/readline"
        other.mkdir(parents=True)
        (other / "readline.h").write_text("#define ATRINIK_READLINE_FIXTURE 2\n")
        result = self.configure(f"-DREADLINE_INCLUDE_DIR={other.parent}")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("same resolved prefix", result.stderr)

    def test_missing_selected_headers_do_not_fall_back_to_another_prefix(self):
        (self.prefix / "include/readline/readline.h").unlink()
        other = self.root / "other-readline/include/readline"
        other.mkdir(parents=True)
        (other / "readline.h").write_text("#define ATRINIK_READLINE_FIXTURE 2\n")
        result = self.configure(f"-DCMAKE_PREFIX_PATH={self.prefix};{other.parent.parent}")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires matching Readline headers", result.stderr)

    def test_multiarch_library_pairs_with_the_prefix_include_directory(self):
        multiarch = self.prefix / "lib/x86_64-linux-gnu"
        multiarch.mkdir()
        library = multiarch / "libreadline.a"
        (self.prefix / "lib/libreadline.a").rename(library)
        result = self.configure(f"-DREADLINE_LIBRARY={library}")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([shutil.which("cmake"), "--build", str(self.root / "build"),
                                 "--parallel", "2"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_brew_style_symlink_prefix_pairs_with_resolved_headers(self):
        alias = self.root / "opt/readline"
        alias.parent.mkdir()
        alias.symlink_to(self.prefix, target_is_directory=True)
        result = self.configure(f"-DREADLINE_LIBRARY={alias}/lib/libreadline.a",
                                f"-DREADLINE_INCLUDE_DIR={alias}/include")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_no_selected_library_preserves_optional_console_fallback(self):
        (self.prefix / "include/readline/readline.h").unlink()
        result = self.configure("-DREADLINE_LIBRARY:FILEPATH=")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("simple interactive console", result.stdout)


if __name__ == "__main__":
    unittest.main()
