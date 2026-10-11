# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).parent
spec = importlib.util.spec_from_file_location('appimage_compiler', HERE / 'compiler.py')
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)


class CompilerTests(unittest.TestCase):
    def fixture(self, root):
        lock = json.loads((HERE / 'packaging.lock.json').read_text())['compiler']
        lock['notices'] = {'compiler-rt-LICENSE.TXT': hashlib.sha256(b'notice').hexdigest()}
        lock['source']['sha256'] = hashlib.sha256(b'source').hexdigest()
        manifest = root / 'share/atrinik/llvm-toolchain.json'
        manifest.parent.mkdir(parents=True)
        manifest.write_text(json.dumps(lock))
        notice = root / 'share/licenses/llvm/compiler-rt-LICENSE.TXT'
        notice.parent.mkdir(parents=True)
        notice.write_bytes(b'notice')
        source = root / 'share/atrinik/sources/llvm-project-23.1.3.src.tar.xz'
        source.parent.mkdir()
        source.write_bytes(b'source')
        (root / 'bin').mkdir()
        for name in lock['tools']:
            (root / 'bin' / name).touch()
        return lock, manifest, notice, source

    def test_image_rejects_mutable_foreign_or_malformed_identity(self):
        expected = 'ghcr.io/atrinik/classic-build:1.2.3@sha256:' + 'a' * 64
        self.assertEqual(compiler.image_reference(expected), expected)
        for value in (expected.split('@')[0], expected.replace('classic-build', 'other'),
                      expected[:-1], expected + '\n', 'classic-build:latest'):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'immutable'):
                compiler.image_reference(value)

    def test_preparation_rejects_missing_or_mutable_image_before_download(self):
        with tempfile.TemporaryDirectory() as directory:
            for argument in ([], ['--compiler-image', 'classic-build:latest']):
                result = subprocess.run([sys.executable, str(HERE / 'prepare.py'), '--output', directory, *argument], capture_output=True, text=True)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(list(Path(directory).iterdir()), [])

    def test_cohort_and_notice_changes_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lock, manifest, notice, source = self.fixture(root)
            def version(args, **kwargs):
                return '23.1.3\n' if Path(args[0]).name == 'llvm-config' else 'LLVM version 23.1.3\n'
            with patch.object(compiler.subprocess, 'check_output', side_effect=version):
                compiler.verify(root, lock)
                original = manifest.read_bytes()
                changed = json.loads(original)
                changed['defaults']['runtime'] = 'libgcc'
                manifest.write_text(json.dumps(changed))
                with self.assertRaisesRegex(ValueError, 'identity'):
                    compiler.verify(root, lock)
                manifest.write_bytes(original)
                notice.unlink()
                with self.assertRaisesRegex(ValueError, 'notice'):
                    compiler.verify(root, lock)
                notice.write_bytes(b'wrong notice')
                with self.assertRaisesRegex(ValueError, 'notice'):
                    compiler.verify(root, lock)
                notice.write_bytes(b'notice')
                source.write_bytes(b'changed source')
                with self.assertRaisesRegex(ValueError, 'source archive'):
                    compiler.verify(root, lock)

    def test_wrong_tool_version_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lock, *_ = self.fixture(root)
            with patch.object(compiler.subprocess, 'check_output', return_value='LLVM version 23.1.30\n'):
                with self.assertRaisesRegex(ValueError, 'mismatched LLVM tool'):
                    compiler.verify(root, lock)


if __name__ == '__main__':
    unittest.main()
