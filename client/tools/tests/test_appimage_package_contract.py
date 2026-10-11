#!/usr/bin/env python3
"""Exercise launcher behavior and package input boundaries without a native build."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / 'client/tools/build-appimage.sh'
LAUNCHER = ROOT / 'tools/ci/appimage/AppRun'


class AppImageContractTests(unittest.TestCase):
    def test_invalid_inputs_fail_before_build(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith('ATRINIK_')}
            for version in ('', '5.1', '5.1.0;bad', '../5.1.0'):
                environment['ATRINIK_PACKAGE_VERSION'] = version
                result = subprocess.run(['bash', str(SCRIPT)], cwd=directory,
                                        env=environment, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('must be MAJOR.MINOR.PATCH', result.stderr)
            environment['ATRINIK_PACKAGE_VERSION'] = '5.1.0'
            for value in ('0', '-1', '5', '2.0', '2;bad'):
                environment['CMAKE_BUILD_PARALLEL_LEVEL'] = value
                result = subprocess.run(['bash', str(SCRIPT)], cwd=directory,
                                        env=environment, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('integer from 1 to 4', result.stderr)
            self.assertFalse((Path(directory) / 'build').exists())

    def launcher_fixture(self, directory):
        appdir = Path(directory) / 'renamed folder/Atrinik.AppDir'
        (appdir / 'usr/bin').mkdir(parents=True)
        (appdir / 'usr/lib/ossl-modules').mkdir(parents=True)
        (appdir / 'usr/share/games/atrinik').mkdir(parents=True)
        (appdir / 'usr/share/atrinik').mkdir(parents=True)
        shutil.copyfile(LAUNCHER, appdir / 'AppRun')
        (appdir / 'AppRun').chmod(0o755)
        executable = appdir / 'usr/bin/atrinik'
        executable.write_text('#!/usr/bin/python3\nimport json, os, sys\n'
                              'print(json.dumps({"argv":sys.argv[1:],"cwd":os.getcwd(),'
                              '"config":os.environ.get("ATRINIK_CONFIG_DIR"),'
                              '"home":os.environ.get("HOME"),'
                              '"ld":os.environ["LD_LIBRARY_PATH"],'
                              '"modules":os.environ["OPENSSL_MODULES"],'
                              '"openssl_conf":os.environ["OPENSSL_CONF"],'
                              '"ca":os.environ["SSL_CERT_FILE"]}))\n')
        executable.chmod(0o755)
        return appdir

    def test_relocation_spaces_arguments_and_relative_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            appdir = self.launcher_fixture(directory)
            caller = Path(directory) / 'other cwd'
            caller.mkdir()
            environment = dict(os.environ, ATRINIK_CONFIG_DIR='my config',
                               LD_LIBRARY_PATH='/unrelated/host/libs', SSL_CERT_FILE='/custom/ca')
            arguments = ['--server', 'space ; $value', '', '--config=x y']
            result = subprocess.run([str(appdir / 'AppRun'), *arguments], cwd=caller,
                                    env=environment, capture_output=True, text=True, check=True)
            data = json.loads(result.stdout)
            self.assertEqual(data['argv'], arguments)
            self.assertEqual(data['cwd'], str(appdir / 'usr/share/games/atrinik'))
            self.assertEqual(data['config'], str(caller / 'my config'))
            self.assertEqual(data['ld'], str(appdir / 'usr/lib'))
            self.assertEqual(data['modules'], str(appdir / 'usr/lib/ossl-modules'))
            self.assertEqual(data['ca'], '/custom/ca')

    def test_default_home_remains_client_owned(self):
        with tempfile.TemporaryDirectory() as directory:
            appdir = self.launcher_fixture(directory)
            environment = dict(os.environ, HOME=str(Path(directory) / 'player home'))
            environment.pop('ATRINIK_CONFIG_DIR', None)
            result = subprocess.run([str(appdir / 'AppRun')], cwd='/', env=environment,
                                    capture_output=True, text=True, check=True)
            data = json.loads(result.stdout)
            self.assertIsNone(data['config'])
            self.assertEqual(data['home'], environment['HOME'])
            environment.pop('HOME')
            result = subprocess.run([str(appdir / 'AppRun')], cwd='/', env=environment,
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('HOME or ATRINIK_CONFIG_DIR', result.stderr)


if __name__ == '__main__':
    unittest.main()
