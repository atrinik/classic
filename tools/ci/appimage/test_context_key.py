import contextlib
import importlib.util
import io
from pathlib import Path
import os
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('context_key', Path(__file__).with_name('context_key.py'))
context_key = importlib.util.module_from_spec(spec)
spec.loader.exec_module(context_key)


class ContextKeyTests(unittest.TestCase):
    def fixture(self, root):
        context = root / 'context'
        (context / 'inputs').mkdir(parents=True)
        (context / 'Dockerfile').write_text('FROM fixture AS build\n')
        (context / 'packaging.lock.json').write_text('{"compiler": {}}\n')
        (context / 'inputs' / 'ca-certificates.deb').write_bytes(b'deb')
        graphical = root / 'graphical.Dockerfile'
        graphical.write_text('FROM fixture\n')
        return context, graphical

    def test_key_is_stable_and_covers_every_input(self):
        with tempfile.TemporaryDirectory() as directory:
            context, graphical = self.fixture(Path(directory))
            key = context_key.context_key(context, graphical)
            self.assertRegex(key, r'^[0-9a-f]{64}$')
            self.assertEqual(key, context_key.context_key(context, graphical))
            for change in (context / 'Dockerfile', context / 'inputs' / 'ca-certificates.deb', graphical):
                original = change.read_bytes()
                change.write_bytes(original + b'#')
                self.assertNotEqual(key, context_key.context_key(context, graphical), change.name)
                change.write_bytes(original)
            self.assertEqual(key, context_key.context_key(context, graphical))
            (context / 'extra').write_text('x')
            self.assertNotEqual(key, context_key.context_key(context, graphical))

    def test_rename_changes_key(self):
        with tempfile.TemporaryDirectory() as directory:
            context, graphical = self.fixture(Path(directory))
            key = context_key.context_key(context, graphical)
            (context / 'packaging.lock.json').rename(context / 'other.json')
            self.assertNotEqual(key, context_key.context_key(context, graphical))

    def test_rejects_symlinks_and_missing_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context, graphical = self.fixture(root)
            os.symlink(context / 'Dockerfile', context / 'linked')
            with self.assertRaisesRegex(ValueError, 'symlink'):
                context_key.context_key(context, graphical)
            (context / 'linked').unlink()
            with self.assertRaisesRegex(ValueError, 'graphical'):
                context_key.context_key(context, root / 'missing')
            empty = root / 'empty'
            empty.mkdir()
            with self.assertRaisesRegex(ValueError, 'empty'):
                context_key.context_key(empty, graphical)

    def test_image_reference_accepts_only_known_targets(self):
        key = 'a' * 64
        self.assertEqual(context_key.image_reference('ghcr.io/atrinik/classic-appimage', 'smoke-debian13', key),
                         'ghcr.io/atrinik/classic-appimage:smoke-debian13-' + key)
        with self.assertRaisesRegex(ValueError, 'unknown'):
            context_key.image_reference('ghcr.io/atrinik/classic-appimage', 'other', key)

    def test_cli_reports_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context, graphical = self.fixture(root)
            stdout, stderr = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                self.assertEqual(context_key.main(['--context', str(context), '--graphical-dockerfile', str(graphical)]), 0)
                self.assertEqual(context_key.main(['--context', str(root / 'nope'), '--graphical-dockerfile', str(graphical)]), 1)
            self.assertEqual(stdout.getvalue().strip(), context_key.context_key(context, graphical))
            self.assertIn('context key failed', stderr.getvalue())


if __name__ == '__main__':
    unittest.main()
