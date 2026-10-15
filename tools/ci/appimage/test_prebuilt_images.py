import contextlib
import importlib.util
import io
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('prebuilt_images', Path(__file__).with_name('prebuilt_images.py'))
prebuilt_images = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prebuilt_images)

REPOSITORY = 'ghcr.io/atrinik/classic-appimage'
DIGEST = f'{REPOSITORY}@sha256:' + 'b' * 64


def run(*arguments):
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        status = prebuilt_images.main(list(arguments))
    return status, stdout.getvalue().strip()


class PrebuiltImagesTests(unittest.TestCase):
    def test_record_then_select_round_trips_exact_digests(self):
        status, images = run('record', '--repository', REPOSITORY, '--target', 'build', '--reference', DIGEST)
        self.assertEqual(status, 0)
        status, images = run('record', '--repository', REPOSITORY, '--target', 'graphical',
                             '--images', images, '--reference', DIGEST)
        self.assertEqual(json.loads(images), {'build': DIGEST, 'graphical': DIGEST})
        self.assertEqual(run('select', '--repository', REPOSITORY, '--target', 'graphical', '--images', images),
                         (0, DIGEST))

    def test_select_rejects_missing_tags_foreign_and_malformed_references(self):
        good = json.dumps({'build': DIGEST})
        self.assertEqual(run('select', '--repository', REPOSITORY, '--target', 'smoke-ubuntu24', '--images', good)[0], 1)
        self.assertEqual(run('select', '--repository', REPOSITORY, '--target', 'build', '--images', '')[0], 1)
        for bad in (f'{REPOSITORY}:build-' + 'a' * 64, 'ghcr.io/other/image@sha256:' + 'b' * 64,
                    f'{REPOSITORY}@sha256:' + 'B' * 64, f'{REPOSITORY}@sha256:abc', 7):
            images = json.dumps({'build': bad})
            self.assertEqual(run('select', '--repository', REPOSITORY, '--target', 'build', '--images', images)[0], 1)
        for bad_map in ('[]', '{"other": "x"}', 'not json'):
            self.assertEqual(run('select', '--repository', REPOSITORY, '--target', 'build', '--images', bad_map)[0], 1)

    def test_record_rejects_unverifiable_references(self):
        self.assertEqual(run('record', '--repository', REPOSITORY, '--target', 'build',
                             '--reference', f'{REPOSITORY}:build-latest')[0], 1)


if __name__ == '__main__':
    unittest.main()
