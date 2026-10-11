# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the pinned pre-LLVM assembler's lock and complete-notice boundary."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from test_prepare import prepare

ROOT = Path(__file__).resolve().parents[3]
HISTORICAL_COMMIT = 'fd4943252ac872fce2995c286b10a0c6d249aba3'


def historical_blob(name):
    return subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{HISTORICAL_COMMIT}:{name}'])


class StopAfterCompleteNotices(Exception):
    pass


class HistoricalAssemblerTests(unittest.TestCase):
    def test_prepared_historical_lock_passes_actual_assembler_and_keeps_llvm_notices(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source'
            for name in ('tools/ci/appimage/assemble.py', 'tools/ci/appimage/packaging.lock.json',
                         'tools/ci/appimage/AppRun', 'tools/ci/appimage/openssl.cnf', 'client/ca-bundle.crt'):
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(historical_blob(name))
            historical_lock = source / 'tools/ci/appimage/packaging.lock.json'
            trusted = json.loads(Path(__file__).with_name('packaging.lock.json').read_text())
            image = trusted['compiler'].get('image') or 'ghcr.io/atrinik/classic-build:1.2.3@sha256:' + 'a' * 64
            context = root / 'context'
            def materialize(entry, destination, cache):
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(b'fixture')
            args = ['prepare.py', '--output', str(context), '--compiler-image', image,
                    '--source-packaging-lock', str(historical_lock)]
            with patch.object(sys, 'argv', args), patch.object(prepare, 'download', side_effect=materialize):
                prepare.main()
            self.assertEqual((context / 'packaging.lock.json').read_bytes(), historical_lock.read_bytes())
            execution = json.loads((context / 'dependency-inputs.lock.json').read_text())
            self.assertEqual(execution['compiler'], trusted['compiler'])

            prefix = root / 'prefix'
            prefix.mkdir()
            shutil.copyfile(context / 'packaging.lock.json', prefix / 'packaging.lock.json')
            (prefix / 'bin').mkdir()
            shutil.copyfile('/usr/bin/true', prefix / 'bin/openssl')
            (prefix / 'lib/ossl-modules').mkdir(parents=True)
            shutil.copyfile('/usr/bin/true', prefix / 'lib/ossl-modules/legacy.so')
            notices = prefix / 'share/licenses/llvm'
            notices.mkdir(parents=True)
            for name in trusted['compiler']['notices']:
                (notices / name).write_bytes((name + '\nComplete notice fixture\n').encode() * 100)
            appdir = root / 'AppDir'
            (appdir / 'usr/share/doc/atrinik').mkdir(parents=True)
            (appdir / 'usr/bin').mkdir(parents=True)
            shutil.copyfile('/usr/bin/true', appdir / 'usr/bin/atrinik')
            spec = importlib.util.spec_from_file_location('pre_llvm_assembler', source / 'tools/ci/appimage/assemble.py')
            assembler = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(assembler)
            copytree = shutil.copytree
            def copy_until_notices(source_path, destination, *args, **kwargs):
                if Path(source_path) == Path('/usr/share/common-licenses'):
                    raise StopAfterCompleteNotices()
                return copytree(source_path, destination, *args, **kwargs)
            with patch.object(assembler.shutil, 'copytree', side_effect=copy_until_notices):
                with self.assertRaises(StopAfterCompleteNotices):
                    assembler.assemble(appdir, prefix, '5.39.0', 'a' * 40)
            # Stop before the platform library closure, after the real historical
            # assembler passes its exact SHA gate and copies every LLVM notice.
            for name in trusted['compiler']['notices']:
                self.assertEqual((appdir / 'usr/share/doc/atrinik/licenses/llvm' / name).read_bytes(),
                                 (notices / name).read_bytes())
            (prefix / 'packaging.lock.json').write_bytes(Path(__file__).with_name('packaging.lock.json').read_bytes())
            with self.assertRaisesRegex(ValueError, 'packaging lock differs from source lock'):
                assembler.assemble(appdir, prefix, '5.39.0', 'a' * 40)


if __name__ == '__main__':
    unittest.main()
