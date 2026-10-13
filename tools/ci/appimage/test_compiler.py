# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import importlib.util
import json
import os
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
validator_spec = importlib.util.spec_from_file_location('appimage_validator', HERE.parents[1] / 'release/appimage.py')
validator = importlib.util.module_from_spec(validator_spec)
sys.modules[validator_spec.name] = validator
validator_spec.loader.exec_module(validator)


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


class InspectorTests(unittest.TestCase):
    def fixture(self, root, version='LLVM version 23.1.3', status=0):
        pinned = root / 'opt/llvm-23.1.3/bin/llvm-readelf'
        pinned.parent.mkdir(parents=True)
        alias = root / 'usr/bin/llvm-readelf'
        alias.parent.mkdir(parents=True)
        capture = root / 'probe.jsonl'
        pinned.write_text('#!' + sys.executable + '\n'
                          'import json,os,sys\n'
                          f'with open({str(capture)!r}, "a") as stream:\n'
                          ' stream.write(json.dumps({"argv":sys.argv,"env":dict(os.environ)}) + "\\n")\n'
                          f'print({version!r})\nsys.exit({status})\n')
        pinned.chmod(0o755)
        return pinned, alias, capture

    def coordinates(self, pinned, alias):
        # Only tests replace the fixed producer and validator coordinates.
        return patch.multiple(compiler, PINNED_INSPECTOR=pinned, VALIDATOR_INSPECTOR=alias)

    def test_real_alias_preflight_matches_actual_validator_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            pinned, alias, capture = self.fixture(Path(temporary))
            injected = {'PATH': '/candidate/bin', 'LD_LIBRARY_PATH': '/candidate/lib',
                        'LD_PRELOAD': '/candidate/preload.so', 'LD_AUDIT': '/candidate/audit.so',
                        'LD_FUTURE_OPTION': '/candidate/other', 'PYTHONHOME': '/candidate/python',
                        'PYTHONPATH': '/candidate/python', 'GIT_CONFIG_COUNT': '1',
                        'GIT_CONFIG_KEY_0': 'core.sshCommand', 'GIT_CONFIG_VALUE_0': 'candidate',
                        'LC_ALL': 'C.UTF-8', 'TZ': 'candidate'}
            with self.coordinates(pinned, alias), patch.dict(os.environ, injected):
                compiler.provision_inspector()
                self.assertTrue(alias.is_symlink())
                self.assertEqual(alias.readlink(), pinned)
                # The validator itself still chooses /usr/bin; replace just
                # that root with our owned filesystem to exercise its real code.
                with patch.object(validator, 'Path', side_effect=lambda value:
                                  alias.parent if value == '/usr/bin' else Path(value)):
                    self.assertIn('LLVM version 23.1.3', validator._run('llvm-readelf', '--version'))
            observations = [json.loads(line) for line in capture.read_text().splitlines()]
            self.assertEqual(len(observations), 2)
            self.assertEqual(observations[0], observations[1])
            self.assertEqual(observations[0]['argv'], [str(alias), '--version'])
            environment = observations[0]['env']
            for key in injected:
                if key.startswith(('LD_', 'GIT_')) or key in {'PYTHONHOME', 'PYTHONPATH'}:
                    self.assertNotIn(key, environment)
            self.assertEqual(environment['PATH'], '/usr/bin:/bin')
            self.assertEqual(environment['LC_ALL'], 'C')
            self.assertEqual(environment['TZ'], 'UTC')
            self.assertEqual(environment['GIT_NO_REPLACE_OBJECTS'], '1')

    def test_occupied_coordinate_including_dangling_link_rejects_before_execution(self):
        for kind in ('file', 'directory', 'valid-symlink', 'dangling-symlink'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                pinned, alias, capture = self.fixture(Path(temporary))
                if kind == 'file':
                    alias.write_bytes(b'existing inspector')
                elif kind == 'directory':
                    alias.mkdir()
                else:
                    alias.symlink_to(pinned if kind == 'valid-symlink' else pinned.parent / 'missing')
                before = alias.lstat()
                with self.coordinates(pinned, alias), patch.object(compiler.subprocess, 'run') as execute:
                    with self.assertRaisesRegex(ValueError, 'coordinate already exists'):
                        compiler.provision_inspector()
                execute.assert_not_called()
                self.assertEqual(alias.lstat(), before)
                self.assertFalse(capture.exists())

    def test_missing_nonexecutable_or_directory_target_rejects_before_alias(self):
        for kind in ('missing', 'nonexecutable', 'directory'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                pinned, alias, capture = self.fixture(Path(temporary))
                if kind == 'nonexecutable':
                    pinned.chmod(0o644)
                else:
                    pinned.unlink()
                    if kind == 'directory':
                        pinned.mkdir()
                with self.coordinates(pinned, alias), patch.object(compiler.subprocess, 'run') as execute:
                    with self.assertRaisesRegex(ValueError, 'not executable'):
                        compiler.provision_inspector()
                execute.assert_not_called()
                self.assertFalse(os.path.lexists(alias))
                self.assertFalse(capture.exists())

    def test_wrong_family_version_or_nonzero_exit_fails_actual_probe(self):
        for version, status in (('GNU readelf version 23.1.3', 0), ('LLVM version 23.1.30', 0),
                                ('LLVM version 23.1.2', 0), ('LLVM version 23.1.3', 7)):
            with self.subTest(version=version, status=status), tempfile.TemporaryDirectory() as temporary:
                pinned, alias, capture = self.fixture(Path(temporary), version, status)
                with self.coordinates(pinned, alias):
                    with self.assertRaisesRegex(ValueError, 'does not execute the pinned'):
                        compiler.provision_inspector()
                self.assertEqual(len(capture.read_text().splitlines()), 1)

    def test_exclusive_creation_rejects_late_coordinate_claim_before_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            pinned, alias, capture = self.fixture(Path(temporary))
            create = Path.symlink_to
            def claim_then_create(path, target):
                self.assertEqual(path, alias)
                alias.write_bytes(b'late claimant')
                return create(path, target)
            with self.coordinates(pinned, alias), patch.object(Path, 'symlink_to', claim_then_create), \
                    patch.object(compiler.subprocess, 'run') as execute:
                with self.assertRaises(FileExistsError):
                    compiler.provision_inspector()
            execute.assert_not_called()
            self.assertEqual(alias.read_bytes(), b'late claimant')
            self.assertFalse(capture.exists())

    def test_image_action_verifies_cohort_before_provisioning(self):
        with tempfile.TemporaryDirectory() as temporary:
            lock = Path(temporary) / 'lock.json'
            lock.write_text((HERE / 'packaging.lock.json').read_text())
            order = []
            with patch.object(sys, 'argv', ['compiler.py', str(lock), '--provision-inspector']), \
                    patch.object(compiler, 'verify', side_effect=lambda *args: order.append('verify')), \
                    patch.object(compiler, 'provision_inspector', side_effect=lambda: order.append('provision')):
                compiler.main()
            self.assertEqual(order, ['verify', 'provision'])
            with patch.object(sys, 'argv', ['compiler.py', str(lock), '--provision-inspector']), \
                    patch.object(compiler, 'verify', side_effect=ValueError('changed producer')), \
                    patch.object(compiler, 'provision_inspector') as provide:
                with self.assertRaisesRegex(ValueError, 'changed producer'):
                    compiler.main()
            provide.assert_not_called()
        dockerfile = (HERE / 'Dockerfile').read_text()
        self.assertIn('python3 /inputs/compiler.py /inputs/packaging.lock.json --provision-inspector '
                      '&& bash /inputs/build-dependencies.sh', dockerfile)


if __name__ == '__main__':
    unittest.main()
