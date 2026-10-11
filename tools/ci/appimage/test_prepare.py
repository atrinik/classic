import importlib.util
from pathlib import Path
import tempfile
import unittest
import json
import sys
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('prepare', Path(__file__).with_name('prepare.py'))
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class IntegrityTests(unittest.TestCase):
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
