import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('prepare', Path(__file__).with_name('prepare.py'))
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class IntegrityTests(unittest.TestCase):
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
