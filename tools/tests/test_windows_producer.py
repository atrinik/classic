from __future__ import annotations

import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools.release import windows_producer as producer


class WindowsProducerTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.git('init', '--quiet')
        self.git('config', 'user.name', 'Windows producer fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.recipe = {'client/CMakeLists.txt': self.blob('client/CMakeLists.txt', b'recipe\n')}
        self.descriptor_path = 'tools/ci/appimage/packaging.lock.json'
        self.blob(self.descriptor_path, json.dumps({'compiler': {
            'family': 'Clang', 'image': 'ghcr.io/atrinik/classic-build:1.18.0@sha256:' + 'a' * 64
        }}).encode())
        self.revision = self.commit()
        self.selector = Path(producer.__file__).resolve()

    def git(self, *arguments: str) -> str:
        return subprocess.run(['git', '-C', str(self.root), *arguments], check=True,
                              capture_output=True, text=True).stdout.strip()

    def blob(self, path: str, contents: bytes) -> str:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(contents)
        return hashlib.sha256(contents).hexdigest()

    def commit(self) -> str:
        self.git('add', '.')
        self.git('-c', 'commit.gpgsign=false', 'commit', '--quiet', '-m', 'fixture')
        return self.git('rev-parse', 'HEAD')

    def test_current_llvm_image_environment_and_compiler_cache(self) -> None:
        with patch.object(producer, 'LLVM_RECIPE', self.recipe):
            selected = producer.select_producer(self.root, self.revision)
        self.assertEqual(selected['windows_image'], producer.LLVM_IMAGE)
        self.assertEqual(selected['windows_mode'], 'llvm')
        self.assertIn('llvm-mingw', selected['windows_cache_epoch'])
        environment = selected['environment']
        self.assertEqual(environment['ATRINIK_WINDOWS_TARGET'], 'x86_64-w64-mingw32')
        self.assertTrue(environment['PATH'].startswith('/opt/llvm-mingw/bin:'))
        self.assertFalse(any(name.startswith('MXE_') for name in environment))

    def test_approved_historical_uses_general_sdk_and_inherits_python_worldmaker(self) -> None:
        with patch.object(producer, 'HISTORICAL', {self.revision: {}}), \
                patch.object(producer, 'MXE_RECIPE', self.recipe):
            selected = producer.select_producer(self.root, self.revision)
        self.assertEqual(selected['windows_image'], producer.MXE_IMAGE)
        self.assertEqual(selected['windows_mode'], 'mxe')
        self.assertIn('mxe-gcc-general', selected['windows_cache_epoch'])
        self.assertNotEqual(selected['windows_cache_epoch'], 'llvm-mingw-1.18-01065f08')
        self.assertEqual(selected['environment'], {
            'PATH': '/opt/mxe/.ccache/bin:/opt/mxe/usr/bin:' + producer.SYSTEM_PATH,
            'MXE_TARGET': 'x86_64-w64-mingw32.shared',
            'MXE_TOOLCHAIN_FILE': '/opt/mxe/usr/x86_64-w64-mingw32.shared/share/cmake/mxe-conf.cmake',
            'MXE_RUNTIME_DIR': '/opt/mxe/usr/x86_64-w64-mingw32.shared/bin',
        })
        # Original image ENV supplies Python and native tools. These overrides
        # must not request LLVM receipts, invoke staging or alter package scripts.
        self.assertEqual(producer.docker_environment(selected), [
            argument for name, value in selected['environment'].items()
            for argument in ('--env', name + '=' + value)])

    def test_unknown_history_and_descendants_are_not_automatically_approved(self) -> None:
        historical = self.revision
        self.blob('unrelated', b'next revision')
        descendant = self.commit()
        with patch.object(producer, 'HISTORICAL', {historical: {}}), \
                patch.object(producer, 'MXE_RECIPE', self.recipe):
            with self.assertRaises(ValueError):
                producer.select_producer(self.root, descendant)

    def test_tampered_historical_registry_blob_is_rejected(self) -> None:
        with patch.object(producer, 'HISTORICAL', {self.revision: {}}), \
                patch.object(producer, 'MXE_RECIPE', {'client/CMakeLists.txt': '0' * 64}):
            with self.assertRaisesRegex(ValueError, 'unapproved Windows'):
                producer.select_producer(self.root, self.revision)

    def test_dirty_checkout_symlink_missing_blob_and_other_head_are_rejected(self) -> None:
        with patch.object(producer, 'LLVM_RECIPE', self.recipe):
            (self.root / 'client/CMakeLists.txt').write_text('tampered')
            with self.assertRaisesRegex(ValueError, 'checkout differs'):
                producer.select_producer(self.root, self.revision)
            (self.root / 'client/CMakeLists.txt').unlink()
            (self.root / 'client/CMakeLists.txt').symlink_to('/dev/null')
            with self.assertRaises(ValueError):
                producer.select_producer(self.root, self.revision)
            self.git('checkout', '--', 'client/CMakeLists.txt')
            with patch.object(producer, 'LLVM_RECIPE', {'missing': '0' * 64}):
                with self.assertRaisesRegex(ValueError, 'regular source blob'):
                    producer.select_producer(self.root, self.revision)
            self.blob('new', b'new')
            self.commit()
            with self.assertRaisesRegex(ValueError, 'HEAD differs'):
                producer.select_producer(self.root, self.revision)

    def test_cli_rejects_revision_arguments_and_does_not_write_outputs_on_failure(self) -> None:
        output = self.root / 'outputs'
        for revision in ('HEAD', '--help', self.revision.upper(), 'a' * 39, self.revision):
            result = subprocess.run(['python3', str(self.selector), '--source-root', str(self.root),
                                     '--revision=' + revision, '--github-output', str(output),
                                     '--format', 'docker-env'], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, '')
            self.assertFalse(output.exists())

    def test_cli_success_emits_bounded_metadata_and_docker_argument_lines(self) -> None:
        output = self.root / 'outputs'
        printed = io.StringIO()
        with patch.object(producer, 'LLVM_RECIPE', self.recipe), \
                patch('sys.argv', [str(self.selector), '--source-root', str(self.root),
                                   '--revision', self.revision, '--github-output', str(output),
                                   '--format', 'docker-env']), contextlib.redirect_stdout(printed):
            self.assertEqual(producer.main(), 0)
        self.assertEqual(output.read_text().splitlines(), [
            'windows_image=' + producer.LLVM_IMAGE, 'windows_mode=llvm',
            'windows_cache_epoch=llvm-mingw-1.18-01065f08'])
        arguments = printed.getvalue().splitlines()
        self.assertEqual(arguments[::2], ['--env'] * 4)
        self.assertTrue(all('=' in argument for argument in arguments[1::2]))

    def test_current_llvm_rejects_absent_malformed_and_dirty_compiler_descriptor(self) -> None:
        with patch.object(producer, 'LLVM_RECIPE', self.recipe):
            (self.root / self.descriptor_path).write_text('{}')
            with self.assertRaisesRegex(ValueError, 'descriptor checkout differs'):
                producer.select_producer(self.root, self.revision)
            self.revision = self.commit()
            with self.assertRaisesRegex(ValueError, 'immutable LLVM compiler descriptor'):
                producer.select_producer(self.root, self.revision)
            self.blob(self.descriptor_path, b'{"compiler":{},"compiler":{}}')
            self.revision = self.commit()
            with self.assertRaisesRegex(ValueError, 'duplicate LLVM source descriptor'):
                producer.select_producer(self.root, self.revision)
            (self.root / self.descriptor_path).unlink()
            self.revision = self.commit()
            with self.assertRaisesRegex(ValueError, 'LLVM compiler descriptor'):
                producer.select_producer(self.root, self.revision)

    def test_actual_current_recipe_selects_without_registry_overrides(self) -> None:
        repository = Path(__file__).resolve().parents[2]
        revision = subprocess.run(['git', '-C', str(repository), 'rev-parse', 'HEAD'],
                                  check=True, capture_output=True, text=True).stdout.strip()
        source = self.root / 'current-source'
        subprocess.run(['git', 'clone', '--quiet', '--shared', '--no-checkout',
                        str(repository), str(source)], check=True, capture_output=True)
        subprocess.run(['git', '-C', str(source), 'checkout', '--quiet', '--detach', revision],
                       check=True, capture_output=True)
        result = subprocess.run([sys.executable, str(self.selector), '--source-root', str(source),
                                 '--revision', revision], check=True, capture_output=True, text=True)
        selected = json.loads(result.stdout)
        self.assertEqual(selected['windows_image'], producer.LLVM_IMAGE)
        self.assertEqual(selected['windows_mode'], 'llvm')
        self.assertEqual(selected['environment']['ATRINIK_WINDOWS_TARGET'], 'x86_64-w64-mingw32')
        self.assertFalse(subprocess.run(['git', '-C', str(source), 'status', '--porcelain'],
                                        check=True, capture_output=True).stdout)
        # Authenticate the authored scripts, including rejecting committed drift
        # on either helper rather than silently admitting a newer source recipe.
        for path in ('client/tools/build-windows-package.sh', 'server/tools/build-windows-package.sh'):
            with self.subTest(path=path):
                subprocess.run(['git', '-C', str(source), 'checkout', '--quiet', '--detach', revision],
                               check=True, capture_output=True)
                target = source / path
                target.write_bytes(target.read_bytes() + b'\n# unreviewed recipe change\n')
                subprocess.run(['git', '-C', str(source), 'add', path], check=True, capture_output=True)
                subprocess.run(['git', '-C', str(source), '-c', 'commit.gpgsign=false',
                                '-c', 'user.name=Windows producer fixture',
                                '-c', 'user.email=fixture@example.invalid', 'commit', '--quiet',
                                '-m', 'unreviewed recipe fixture'], check=True, capture_output=True)
                changed = subprocess.run(['git', '-C', str(source), 'rev-parse', 'HEAD'],
                                         check=True, capture_output=True, text=True).stdout.strip()
                rejected = subprocess.run([sys.executable, str(self.selector), '--source-root',
                                           str(source), '--revision', changed], capture_output=True, text=True)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertEqual(rejected.stdout, '')
                self.assertIn('unapproved Windows source recipe: ' + path, rejected.stderr)

    def test_actual_reviewed_source_commits_when_history_is_available(self) -> None:
        repository = Path(__file__).resolve().parents[2]
        for revision in producer.HISTORICAL:
            available = subprocess.run(['git', '-C', str(repository), 'cat-file', '-e',
                                        revision + '^{commit}'], capture_output=True)
            if available.returncode != 0:
                self.skipTest('reviewed source history is absent from this checkout')
        source = self.root / 'historical-source'
        subprocess.run(['git', 'clone', '--quiet', '--shared', '--no-checkout',
                        str(repository), str(source)], check=True, capture_output=True)
        for revision in producer.HISTORICAL:
            with self.subTest(revision=revision):
                subprocess.run(['git', '-C', str(source), 'checkout', '--quiet', '--detach',
                                revision], check=True, capture_output=True)
                result = subprocess.run(['python3', str(self.selector), '--source-root', str(source),
                                         '--revision', revision], check=True, capture_output=True,
                                        text=True)
                selected = json.loads(result.stdout)
                self.assertEqual(selected['windows_image'], producer.MXE_IMAGE)
                self.assertEqual(selected['windows_mode'], 'mxe')
                self.assertEqual(selected['environment']['MXE_TARGET'],
                                 'x86_64-w64-mingw32.shared')

    def test_historical_registry_is_exact_six_source_commits(self) -> None:
        self.assertEqual(set(producer.HISTORICAL), {
            '34bbad5c3f4d9b303c17aec5e2c75b3550ec8983',
            '994e6fdc20da962be9c11055bda28ef5561a1dea',
            '24bee6c8a830aa7372a30be6b842210b07639f73',
            'c5a8f9fa2bdcfbae9311093fb69907c8d64b35d4',
            'fd4943252ac872fce2995c286b10a0c6d249aba3',
            '2349e0e14276fee251250cd451da6da0e68a3148',
        })
        self.assertIn('server/CMakeLists.txt', producer.MXE_RECIPE)
        self.assertIn('tools/ci/stage_windows_runtime.py', producer.MXE_RECIPE)


if __name__ == '__main__':
    unittest.main()
