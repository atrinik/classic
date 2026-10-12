"""Exercise AppImage archive-tool paths through real CMake static probes."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RECIPE = ROOT / 'tools/ci/appimage/build-dependencies.sh'
LLVM_BIN = '/opt/llvm-23.1.3/bin/'


class AppImageArchiveToolTests(unittest.TestCase):
    def recipe_arguments(self, directory):
        """Capture the actual recipe function without running dependency builds."""
        text = RECIPE.read_text()
        function = text.split('build() {\n', 1)[1].split('\n}\n', 1)[0]
        capture = directory / 'capture.py'
        capture.write_text('import json,os,sys\n'
                           'with open(os.environ["ARGV_LOG"], "a") as stream:\n'
                           ' stream.write(json.dumps(sys.argv[1:]) + "\\n")\n')
        log = directory / 'arguments.jsonl'
        environment = dict(os.environ, ARGV_LOG=str(log), FIXTURE_PYTHON=sys.executable,
                           CAPTURE=str(capture))
        script = ('set -euo pipefail\nprefix=/opt/atrinik-appimage\n'
                  'LDFLAGS="--rtlib=compiler-rt --unwindlib=libunwind -fuse-ld=lld"\n'
                  'cmake() { "$FIXTURE_PYTHON" "$CAPTURE" "$@"; }\n'
                  'build() {\n' + function + '\n}\nbuild curl-8.18.0 -DENABLE_ARES=ON\n')
        subprocess.run(['bash', '-c', script], env=environment, check=True,
                       capture_output=True, text=True, timeout=30)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual(len(calls), 3)
        self.assertEqual(calls[1], ['--build', '/build/curl-8.18.0-build', '--parallel', '4'])
        self.assertEqual(calls[2], ['--install', '/build/curl-8.18.0-build'])
        return calls[0]

    def test_recipe_binds_pinned_tools_and_keeps_release_link_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            arguments = self.recipe_arguments(Path(temporary))
        expected = {'C_COMPILER': 'clang', 'CXX_COMPILER': 'clang++',
                    'AR': 'llvm-ar', 'RANLIB': 'llvm-ranlib', 'NM': 'llvm-nm',
                    'STRIP': 'llvm-strip', 'OBJCOPY': 'llvm-objcopy',
                    'OBJDUMP': 'llvm-objdump', 'READELF': 'llvm-readelf'}
        for key, name in expected.items():
            self.assertIn('-DCMAKE_' + key + '=' + LLVM_BIN + name, arguments)
        for kind in ('EXE', 'SHARED'):
            self.assertIn('-DCMAKE_' + kind + '_LINKER_FLAGS=--rtlib=compiler-rt '
                          '--unwindlib=libunwind -fuse-ld=lld', arguments)
        self.assertIn('-DCMAKE_BUILD_TYPE=Release', arguments)
        self.assertIn('-DENABLE_ARES=ON', arguments)
        self.assertFalse(any('HAVE_' in argument for argument in arguments))

    def fixture(self, directory):
        # Host tools exercise CMake's archive plumbing only, not LLVM qualification.
        tools = {role: shutil.which(name) for role, name in
                 (('cmake', 'cmake'), ('compiler', 'cc'), ('ar', 'ar'),
                  ('ranlib', 'ranlib'), ('ninja', 'ninja'))}
        missing = [role for role, path in tools.items() if not path]
        if missing:
            self.skipTest('static-probe fixture requires host tools: ' + ', '.join(missing))
        source = directory / 'source'
        source.mkdir()
        bin_directory = directory / 'bin'
        bin_directory.mkdir()
        for role in ('ar', 'ranlib'):
            shutil.copy2(Path(tools[role]).resolve(), bin_directory / ('llvm-' + role))
        (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(archive_probe C)
include(CheckCSourceCompiles)
include(CheckTypeSize)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXTRA_INCLUDE_FILES sys/socket.h)
check_type_size("struct sockaddr_storage" SIZEOF_STRUCT_SOCKADDR_STORAGE)
check_c_source_compiles("#include <sys/socket.h>
int main(void) { return MSG_NOSIGNAL == 0; }" HAVE_MSG_NOSIGNAL)
check_c_source_compiles("#include <sys/time.h>
int main(void) { struct timeval value = {0}; return (int)value.tv_sec; }" HAVE_STRUCT_TIMEVAL)
file(WRITE "${CMAKE_BINARY_DIR}/result.txt"
  "${HAVE_SIZEOF_STRUCT_SOCKADDR_STORAGE}|${HAVE_MSG_NOSIGNAL}|${HAVE_STRUCT_TIMEVAL}")
''')
        return tools, source, bin_directory

    def configure(self, directory, tools, source, bin_directory, name, arguments):
        build = directory / name
        environment = dict(os.environ, PATH=str(bin_directory) + os.pathsep + os.environ['PATH'])
        result = subprocess.run([tools['cmake'], '-S', str(source), '-B', str(build), '-G', 'Ninja',
                                 '-DCMAKE_C_COMPILER=' + str(Path(tools['compiler']).resolve()),
                                 '-DCMAKE_MAKE_PROGRAM=' + tools['ninja'], *arguments],
                                cwd=directory, env=environment, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return build, (build / 'result.txt').read_text().split('|')

    def test_bare_archive_paths_fail_even_when_tools_are_on_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            tools, source, bin_directory = self.fixture(directory)
            for relative_role in ('AR', 'RANLIB'):
                with self.subTest(role=relative_role):
                    arguments = ['-DCMAKE_' + role + '=' +
                                 (name if role == relative_role else str(bin_directory / name))
                                 for role, name in (('AR', 'llvm-ar'), ('RANLIB', 'llvm-ranlib'))]
                    build, results = self.configure(directory, tools, source, bin_directory,
                                                    'relative-' + relative_role, arguments)
                    self.assertTrue(all(value in ('', 'FALSE') for value in results), results)
                    cache = (build / 'CMakeCache.txt').read_text()
                    name = 'llvm-' + relative_role.lower()
                    self.assertIn('CMAKE_' + relative_role + ':FILEPATH=' + str(directory / name), cache)

    def test_recipe_absolute_archive_paths_pass_all_static_probes(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            tools, source, bin_directory = self.fixture(directory)
            # Substitute only the fixed producer prefix with test-owned host copies.
            arguments = [argument.replace(LLVM_BIN, str(bin_directory) + '/')
                         for argument in self.recipe_arguments(directory)
                         if argument.startswith(('-DCMAKE_AR=', '-DCMAKE_RANLIB='))]
            _, results = self.configure(directory, tools, source, bin_directory, 'absolute', arguments)
            self.assertTrue(all(value in ('1', 'TRUE') for value in results), results)


if __name__ == '__main__':
    unittest.main()
