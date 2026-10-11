import importlib.util
from pathlib import Path
import tempfile
import unittest
import json
import sys
import copy
import os
import contextlib
import io
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('prepare', Path(__file__).with_name('prepare.py'))
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class IntegrityTests(unittest.TestCase):
    def trusted_lock(self):
        lock = json.loads((Path(__file__).with_name('packaging.lock.json')).read_text())
        lock['compiler']['image'] = 'ghcr.io/atrinik/classic-build:1.2.3@sha256:' + 'a' * 64
        return lock

    def test_historical_lock_changes_only_approved_image_and_retains_exact_bytes(self):
        trusted = self.trusted_lock()
        image = 'ghcr.io/atrinik/classic-build:1.1.0@sha256:' + 'b' * 64
        historical = copy.deepcopy(trusted)
        historical['compiler']['image'] = image
        raw = (json.dumps(historical, indent=7) + '\n\n').encode()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'release-lock.json'
            path.write_bytes(raw)
            selected, selected_bytes = prepare.select_packaging_lock(trusted, image, path)
            self.assertEqual(selected, historical)
            self.assertEqual(selected_bytes, raw)
            self.assertNotEqual(trusted['compiler']['image'], image)
            with self.assertRaisesRegex(ValueError, 'trusted packaging lock'):
                prepare.select_packaging_lock(trusted, image)
            with self.assertRaisesRegex(ValueError, 'differs from --compiler-image'):
                prepare.select_packaging_lock(trusted, trusted['compiler']['image'], path)
            for section, key, value in (('source', 'url', 'https://example.invalid/other'),
                                        ('defaults', 'runtime', 'libgcc'),
                                        ('notices', 'compiler-rt-LICENSE.TXT', '0' * 64)):
                changed = copy.deepcopy(historical)
                changed['compiler'][section][key] = value
                path.write_text(json.dumps(changed))
                with self.subTest(section=section), self.assertRaisesRegex(ValueError, 'SDK recipe or LLVM cohort'):
                    prepare.select_packaging_lock(trusted, image, path)
            changed = copy.deepcopy(historical)
            changed['sources'][0]['url'] = 'https://example.invalid/private-source'
            path.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, 'SDK recipe or LLVM cohort'):
                prepare.select_packaging_lock(trusted, image, path)
            changed = copy.deepcopy(historical)
            changed['schema_version'] = float(changed['schema_version'])
            path.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, 'SDK recipe or LLVM cohort'):
                prepare.select_packaging_lock(trusted, image, path)

    def test_historical_default_argv_and_present_missing_image_fail_closed(self):
        trusted = self.trusted_lock()
        image = trusted['compiler']['image']
        self.assertEqual(prepare.select_packaging_lock(trusted, image), (trusted, None))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'release-lock.json'
            historical = copy.deepcopy(trusted)
            del historical['compiler']['image']
            path.write_text(json.dumps(historical))
            with self.assertRaisesRegex(ValueError, 'differs from --compiler-image'):
                prepare.select_packaging_lock(trusted, image, path)
            del historical['compiler']
            path.write_text(json.dumps(historical))
            with self.assertRaisesRegex(ValueError, 'supported pre-LLVM recipe'):
                prepare.select_packaging_lock(trusted, image, path)

    def test_pre_llvm_projection_preserves_source_bytes_and_current_approved_image(self):
        trusted = self.trusted_lock()
        image = trusted['compiler']['image']
        legacy = {key: value for key, value in trusted.items() if key != 'compiler'}
        legacy['notices'] = [path for path in trusted['notices'] if path not in prepare.LLVM_NOTICE_PATHS]
        raw = (json.dumps(legacy, indent=7) + '\n\n').encode()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'legacy-lock.json'
            path.write_bytes(raw)
            self.assertEqual(prepare.select_packaging_lock(trusted, image, path), (legacy, raw))
            with self.assertRaisesRegex(ValueError, 'trusted packaging lock'):
                prepare.select_packaging_lock(trusted, image.replace('a' * 64, 'b' * 64), path)
            changed = copy.deepcopy(legacy)
            changed['sources'][0]['url'] = 'https://example.invalid/private-source'
            path.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, 'supported pre-LLVM recipe'):
                prepare.select_packaging_lock(trusted, image, path)
            changed = copy.deepcopy(legacy)
            changed['compiler'] = None
            path.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, 'requires a compiler descriptor'):
                prepare.select_packaging_lock(trusted, image, path)
            changed = copy.deepcopy(legacy)
            changed['notices'].pop()
            path.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, 'supported pre-LLVM recipe'):
                prepare.select_packaging_lock(trusted, image, path)

    def test_pre_llvm_context_keeps_execution_compiler_and_all_notice_installation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trusted = json.loads((Path(__file__).with_name('packaging.lock.json')).read_text())
            image = trusted['compiler'].get('image') or 'ghcr.io/atrinik/classic-build:1.2.3@sha256:' + 'a' * 64
            legacy = {key: value for key, value in trusted.items() if key != 'compiler'}
            legacy['notices'] = [path for path in trusted['notices'] if path not in prepare.LLVM_NOTICE_PATHS]
            raw = (json.dumps(legacy, sort_keys=True, indent=7) + '\n\n').encode()
            path = root / 'legacy-lock.json'
            path.write_bytes(raw)
            output = root / 'context'
            def materialize(entry, destination, cache):
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(b'fixture')
            args = ['prepare.py', '--output', str(output), '--compiler-image', image,
                    '--source-packaging-lock', str(path)]
            with patch.object(sys, 'argv', args), patch.object(prepare, 'download', side_effect=materialize):
                prepare.main()
            self.assertEqual((output / 'packaging.lock.json').read_bytes(), raw)
            inputs = json.loads((output / 'dependency-inputs.lock.json').read_text())
            self.assertEqual(inputs['compiler'], trusted['compiler'])

    def test_source_lock_paths_shapes_and_duplicate_keys_fail_before_materialization(self):
        trusted = self.trusted_lock()
        image = trusted['compiler']['image']
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'source-lock'
            for data in ('[]', '{', 'null', '{"compiler":{},"compiler":{}}', ' ' * (4 * 1024 * 1024 + 1)):
                path.write_text(data)
                with self.subTest(data_length=len(data)), self.assertRaises(ValueError):
                    prepare.select_packaging_lock(trusted, image, path)
            alias = root / 'alias'
            alias.symlink_to(path)
            with self.assertRaises(OSError):
                prepare.select_packaging_lock(trusted, image, alias)
            fifo = root / 'fifo'
            os.mkfifo(fifo)
            with self.assertRaisesRegex(ValueError, 'bounded regular file'):
                prepare.select_packaging_lock(trusted, image, fifo)

    def test_recovery_context_retains_exact_source_lock_bytes_and_current_recipe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_lock = json.loads((Path(__file__).with_name('packaging.lock.json')).read_text())
            image = 'ghcr.io/atrinik/classic-build:1.1.0@sha256:' + 'b' * 64
            historical = copy.deepcopy(source_lock)
            historical['compiler']['image'] = image
            raw = (json.dumps(historical, sort_keys=True, indent=7) + '\n\n').encode()
            source_lock_path = root / 'release-lock.json'
            source_lock_path.write_bytes(raw)
            output = root / 'context'
            def materialize(entry, destination, cache):
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(b'fixture')
            args = ['prepare.py', '--output', str(output), '--compiler-image', image,
                    '--source-packaging-lock', str(source_lock_path)]
            with patch.object(sys, 'argv', args), patch.object(prepare, 'download', side_effect=materialize):
                prepare.main()
            self.assertEqual((output / 'packaging.lock.json').read_bytes(), raw)
            staged = json.loads((output / 'packaging.lock.json').read_text())
            self.assertEqual({key: value for key, value in staged.items() if key != 'compiler'},
                             {key: value for key, value in source_lock.items() if key != 'compiler'})
            self.assertEqual(json.loads((output / 'dependency-inputs.lock.json').read_text())['compiler'], historical['compiler'])
            self.assertIn(f'FROM {image} AS compiler', (output / 'Dockerfile').read_text())

    def test_changed_release_url_is_rejected_before_download_or_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_lock = json.loads((Path(__file__).with_name('packaging.lock.json')).read_text())
            image = 'ghcr.io/atrinik/classic-build:1.1.0@sha256:' + 'b' * 64
            source_lock['compiler']['image'] = image
            source_lock['sources'][0]['url'] = 'https://example.invalid/private-source'
            path = root / 'release-lock.json'
            path.write_text(json.dumps(source_lock))
            output = root / 'context'
            args = ['prepare.py', '--output', str(output), '--compiler-image', image,
                    '--source-packaging-lock', str(path)]
            with patch.object(sys, 'argv', args), patch.object(prepare, 'download') as download, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as rejected:
                    prepare.main()
            self.assertEqual(rejected.exception.code, 2)
            download.assert_not_called()
            self.assertFalse(output.exists())

    def test_context_embeds_the_exact_immutable_compiler_cohort(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_lock = json.loads((Path(__file__).with_name('packaging.lock.json')).read_text())
            image = source_lock['compiler'].get('image') or 'ghcr.io/atrinik/classic-build:1.2.3@sha256:' + 'a' * 64
            def materialize(entry, destination, cache):
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(b'fixture')
            with patch.object(sys, 'argv', ['prepare.py', '--output', directory, '--compiler-image', image]), patch.object(prepare, 'download', side_effect=materialize):
                prepare.main()
            staged_lock = json.loads((root / 'dependency-inputs.lock.json').read_text())
            self.assertEqual(staged_lock['compiler'], source_lock['compiler'])
            self.assertIn(f'FROM {image} AS compiler', (root / 'Dockerfile').read_text())
            self.assertEqual((root / 'compiler-image.txt').read_text(), image + '\n')
            self.assertEqual((root / 'compiler.py').read_bytes(), (Path(__file__).with_name('compiler.py')).read_bytes())

    def test_corrupt_cache_fails_without_network_or_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sha = '0' * 64
            (root / sha).write_bytes(b'corrupt')
            with patch.object(prepare.urllib.request, 'urlopen') as network:
                with self.assertRaisesRegex(ValueError, 'corrupt cached input'):
                    prepare.download({'name': 'fixture', 'sha256': sha, 'url': 'https://example.invalid'}, root / 'output', root)
                network.assert_not_called()
                self.assertFalse((root / 'output').exists())

    def test_wrong_download_never_enters_cache(self):
        import io
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sha = '0' * 64
            with patch.object(prepare.urllib.request, 'urlopen', return_value=io.BytesIO(b'wrong')):
                with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                    prepare.download({'name': 'fixture', 'sha256': sha, 'url': 'https://example.invalid'}, root / 'output', root)
            self.assertEqual(list(root.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
