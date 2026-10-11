# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('llvm', 'clang', 'clang-tools-extra', 'lld', 'compiler-rt', 'libunwind')


@unittest.skipUnless(shutil.which('cmake'), 'CMake unavailable')
class LinuxRuntimeNotices(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.notices = self.root / 'llvm/share/licenses/llvm'
        self.notices.mkdir(parents=True)
        self.compiler = self.root / 'llvm/bin/clang'
        self.compiler.parent.mkdir()
        self.compiler.touch()
        for name in NAMES:
            (self.notices / (name + '-LICENSE.TXT')).write_text(name + ' complete notice\n')
        (self.root / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.25)
project(NoticeFixture NONE)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_C_COMPILER "{self.compiler}")
include("{ROOT / 'cmake/InstallLLVMRuntime.cmake'}")
atrinik_install_llvm_runtime_notices(share/doc/atrinik AtrinikClient)
''')

    def configure(self):
        return subprocess.run(['cmake', '-S', str(self.root), '-B', str(self.root / 'build')], text=True, capture_output=True)

    def test_client_component_installs_all_complete_runtime_notices(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        prefix = self.root / 'install'
        subprocess.run(['cmake', '--install', str(self.root / 'build'), '--prefix', str(prefix), '--component', 'AtrinikClient'], check=True, capture_output=True)
        for name in NAMES:
            expected = self.notices / (name + '-LICENSE.TXT')
            self.assertEqual((prefix / 'share/doc/atrinik/licenses/llvm' / expected.name).read_bytes(), expected.read_bytes())

    def test_missing_symlinked_or_empty_static_runtime_notice_is_rejected(self):
        notice = self.notices / 'compiler-rt-LICENSE.TXT'
        for kind in ('missing', 'symlink', 'empty'):
            notice.unlink(missing_ok=True)
            if kind == 'symlink':
                notice.symlink_to(self.notices / 'llvm-LICENSE.TXT')
            elif kind == 'empty':
                notice.touch()
            result = self.configure()
            with self.subTest(kind=kind):
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('runtime installation requires', result.stderr)
