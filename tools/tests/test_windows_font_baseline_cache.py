# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
IMAGE = "ghcr.io/atrinik/windows-build:1.16.0@sha256:4266256d4b4196241df6463e687dd286022e24e3bc46dfafb4f877744a731d69"


def baseline_recipe():
    text = (ROOT / ".github/workflows/check.yml").read_text()
    step = text.split("      - name: Build the original pinned Windows font baseline\n", 1)[1]
    step = step.split("      - name: Cross-build and stage standalone tests\n", 1)[0]
    return textwrap.dedent(step.split("        run: |\n", 1)[1])


class WindowsFontBaselineCacheTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("bash"), "Bash unavailable")
    def test_workflow_build_uses_writable_isolated_cache_without_changing_baseline(self):
        for fixed in (False, True):
            with self.subTest(fixed=fixed), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                binary = root / "bin"
                binary.mkdir()
                readonly = root / "image-cache"
                (readonly / "tmp").mkdir(parents=True)
                (readonly / "tmp").chmod(0o555)
                readonly.chmod(0o555)
                source = root / "build/font-baseline-source/tools/ci"
                source.mkdir(parents=True)
                (source / "stage_windows_runtime.py").write_text('''import pathlib, sys
assert '--objdump' in sys.argv and 'x86_64-w64-mingw32.shared-objdump' in sys.argv
output = pathlib.Path(sys.argv[sys.argv.index('--output-dir') + 1])
output.mkdir(parents=True)
(output / 'staged.txt').write_text('historical helper staged baseline')
''')
                fake = binary / "fake"
                fake.write_text('''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys, tempfile
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
root = pathlib.Path(os.environ['FIXTURE_ROOT'])
if name == 'docker':
    if args[0] == 'pull':
        assert args[1] == os.environ['CLASSIC_WINDOWS_FONT_BASELINE_IMAGE']
        sys.exit(0)
    assert args[0] == 'run'
    environment = dict(os.environ, CCACHE_DIR=str(root / 'image-cache'))
    environment.pop('CCACHE_TEMPDIR', None)
    declared = {}
    index = 1
    while index < len(args):
        value = args[index]
        if value == '--env':
            key, value = args[index + 1].split('=', 1)
            declared[key] = value
            if key in ('HOME', 'CCACHE_DIR', 'CCACHE_TEMPDIR'):
                value = str(root / 'container-tmp' / value.removeprefix('/tmp/'))
            environment[key] = value
            index += 2
        elif value in ('--network', '--cpus', '--memory', '--user', '--volume', '--workdir'):
            index += 2
        elif value == '--rm':
            index += 1
        else:
            break
    assert args[index] == os.environ['CLASSIC_WINDOWS_FONT_BASELINE_IMAGE']
    assert args[index + 1:index + 3] == ['bash', '-c']
    (root / 'declared-env.json').write_text(json.dumps(declared))
    environment['PATH'] = str(root / 'bin') + ':' + os.environ['PATH']
    sys.exit(subprocess.run(['bash', '-c', args[index + 3]], cwd=root, env=environment).returncode)
elif name == 'cmake' and '--build' in args:
    cache = pathlib.Path(os.environ['CCACHE_DIR'])
    temporary = pathlib.Path(os.environ.get('CCACHE_TEMPDIR', str(cache / 'tmp')))
    # Model ccache's actual write, including read-only image ownership. Tests
    # also remain meaningful when invoked by a privileged local test runner.
    if cache.stat().st_mode & 0o222 == 0 or temporary.stat().st_mode & 0o222 == 0:
        print('ccache: Failed to create temporary file: Permission denied', file=sys.stderr)
        sys.exit(1)
    with tempfile.NamedTemporaryFile(dir=temporary, prefix='font_render.stdout'):
        (cache / 'font-render-cache-entry').write_text('cached historical object')
    build = root / 'build/font-baseline'
    build.mkdir(parents=True)
    (build / 'client-font-render-probe.exe').write_text('historical baseline executable')
elif name == 'git':
    print('fd4943252ac872fce2995c286b10a0c6d249aba3')
elif name == 'x86_64-w64-mingw32.shared-gcc':
    print('historical GCC version retained')
''')
                fake.chmod(0o755)
                for name in ("docker", "cmake", "git", "x86_64-w64-mingw32.shared-cmake",
                             "x86_64-w64-mingw32.shared-gcc"):
                    (binary / name).symlink_to(fake)
                recipe = baseline_recipe()
                if not fixed:
                    recipe = "\n".join(line for line in recipe.splitlines()
                                       if "--env CCACHE_" not in line and
                                       'mkdir -p "${CCACHE_DIR}"' not in line)
                result = subprocess.run(["bash", "-c", recipe], cwd=root,
                                        env=dict(os.environ, PATH=f"{binary}:{os.environ['PATH']}",
                                                 FIXTURE_ROOT=str(root), GITHUB_WORKSPACE=str(root),
                                                 CLASSIC_WINDOWS_FONT_BASELINE_IMAGE=IMAGE,
                                                 MXE_RUNTIME_DIR="/immutable/historical/runtime"),
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, fixed, result.stderr)
                bundle = root / "build/font-baseline-bundle"
                self.assertEqual(bundle.exists(), fixed)
                self.assertEqual(list(readonly.iterdir()), [readonly / "tmp"])
                if fixed:
                    declared = json.loads((root / "declared-env.json").read_text())
                    self.assertEqual(declared["CCACHE_DIR"], "/tmp/atrinik-font-baseline-ccache")
                    self.assertEqual(declared["CCACHE_TEMPDIR"], "/tmp/atrinik-font-baseline-ccache-tmp")
                    self.assertEqual(declared["CCACHE_MAXSIZE"], "250M")
                    self.assertEqual((bundle / "producer-image.txt").read_text().strip(), IMAGE)
                    self.assertIn("historical GCC", (bundle / "compiler.txt").read_text())
                    self.assertTrue((bundle / "staging-helper.sha256").is_file())
                    self.assertTrue((bundle / "staged.txt").is_file())
                else:
                    self.assertIn("Permission denied", result.stderr)


if __name__ == "__main__":
    unittest.main()
