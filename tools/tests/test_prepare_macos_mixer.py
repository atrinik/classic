# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from contextlib import contextmanager
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PrepareMacOSMixerTests(unittest.TestCase):
    @contextmanager
    def fixture(self, **extra_environment):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repository = root / "checkout with spaces"
            helper = repository / "tools/ci/prepare_macos_mixer.sh"
            helper.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / "tools/ci/prepare_macos_mixer.sh", helper)
            lock = repository / "tools/ci/appimage/packaging.lock.json"
            lock.parent.mkdir()
            archive = root / "source.tar.gz"
            with tarfile.open(archive, "w:gz") as output:
                content = b"verified source fixture\n"
                item = tarfile.TarInfo("SDL3_mixer-fixture/LICENSE.txt")
                item.size = len(content)
                output.addfile(item, io.BytesIO(content))
            lock.write_text(json.dumps({"sources": [{
                "name": "SDL3_mixer", "url": "https://example.invalid/source.tar.gz",
                "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "source_directory": "SDL3_mixer-fixture",
            }]}))
            binary = root / "bin"
            binary.mkdir()
            fake = binary / "fake"
            fake.write_text('''#!/usr/bin/env python3
import json, os, pathlib, shutil, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['COMMAND_LOG'], 'a') as log:
    log.write(json.dumps({'name': name, 'args': args}) + '\\n')
if name == 'uname':
    print(os.environ.get('FIXTURE_SYSTEM', 'Darwin'))
elif name == 'brew':
    print(os.environ['DEPENDENCY_PREFIX'])
elif name == 'curl':
    shutil.copyfile(os.environ['SOURCE_ARCHIVE'], args[args.index('--output') + 1])
elif name == 'cmake':
    if '--build' in args and os.environ.get('FAIL_BUILD') == '1':
        sys.exit(17)
    if '-S' in args and args[args.index('-S') + 1].endswith('macos_mixer_probe'):
        build = pathlib.Path(args[args.index('-B') + 1])
        build.mkdir(parents=True)
        probe = build / 'mixer-probe'
        probe.write_text('#!/bin/sh\\nprintf "decoder/Opus/playback probe\\\\n"\\nexit ' + os.environ.get('PROBE_EXIT', '0') + '\\n')
        probe.chmod(0o755)
''')
            fake.chmod(0o755)
            for name in ("uname", "brew", "curl", "cmake"):
                (binary / name).symlink_to(fake)
            environment = dict(os.environ, PATH=f"{binary}:{os.environ['PATH']}",
                               COMMAND_LOG=str(root / "commands.jsonl"),
                               SOURCE_ARCHIVE=str(archive), DEPENDENCY_PREFIX=str(root / "deps"),
                               **extra_environment)
            yield root, helper, lock, environment

    def invoke(self, root, helper, environment, destination=None):
        result = subprocess.run(["bash", str(helper), str(destination or root / "mixer prefix")],
                                env=environment, capture_output=True, text=True)
        log = root / "commands.jsonl"
        calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return result, calls

    def test_exact_supported_decoders_and_explicit_native_toolchain(self):
        with self.fixture() as (root, helper, lock, environment):
            result, calls = self.invoke(root, helper, environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            configurations = [call["args"] for call in calls
                              if call["name"] == "cmake" and "-S" in call["args"]]
            self.assertEqual(len(configurations), 2)
            options = dict(argument[2:].split("=", 1) for argument in configurations[0]
                           if argument.startswith("-D"))
            expected = {
                "AIFF": "ON", "WAVE": "ON", "VOC": "ON", "AU": "ON",
                "FLAC": "OFF", "GME": "OFF", "MIDI": "OFF", "MOD": "OFF",
                "MP3": "ON", "MP3_DRMP3": "ON", "MP3_MPG123": "OFF",
                "OPUS": "ON", "VORBIS_STB": "ON", "VORBIS_TREMOR": "OFF",
                "VORBIS_VORBISFILE": "OFF", "WAVPACK": "OFF", "STRICT": "ON",
            }
            for name, value in expected.items():
                self.assertEqual(options["SDLMIXER_" + name], value)
            self.assertEqual(options["CMAKE_BUILD_TYPE"], "Release")
            self.assertEqual(options["BUILD_SHARED_LIBS"], "ON")
            toolchain = str(helper.parents[2] / "cmake/toolchains/llvm-macos.cmake")
            for configure in configurations:
                self.assertIn("-DCMAKE_TOOLCHAIN_FILE=" + toolchain, configure)
            self.assertIn("-DSDL3_mixer_DIR=" + str(root / "mixer prefix/lib/cmake/SDL3_mixer"),
                          configurations[1])
            self.assertIn("decoder/Opus/playback probe", result.stdout)

    def test_changed_archive_fails_before_configuration(self):
        with self.fixture() as (root, helper, lock, environment):
            value = json.loads(lock.read_text())
            value["sources"][0]["sha256"] = "0" * 64
            lock.write_text(json.dumps(value))
            result, calls = self.invoke(root, helper, environment)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(any(call["name"] == "cmake" for call in calls))

    def test_build_failure_is_preserved(self):
        with self.fixture(FAIL_BUILD="1") as (root, helper, lock, environment):
            result, calls = self.invoke(root, helper, environment)
            self.assertEqual(result.returncode, 17, result.stderr)
            self.assertFalse(any("--install" in call["args"] for call in calls))

    def test_runtime_contract_failure_is_preserved(self):
        with self.fixture(PROBE_EXIT="9") as (root, helper, lock, environment):
            result, calls = self.invoke(root, helper, environment)
            self.assertEqual(result.returncode, 9, result.stderr)

    def test_other_hosts_fail_before_download(self):
        with self.fixture(FIXTURE_SYSTEM="Linux") as (root, helper, lock, environment):
            result, calls = self.invoke(root, helper, environment)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native macOS", result.stderr)
            self.assertEqual([call["name"] for call in calls], ["uname"])

    def test_existing_destination_is_preserved(self):
        with self.fixture() as (root, helper, lock, environment):
            destination = root / "existing"
            destination.mkdir()
            sentinel = destination / "owned-by-another-task"
            sentinel.write_text("preserve")
            result, calls = self.invoke(root, helper, environment, destination)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(sentinel.read_text(), "preserve")
            self.assertFalse(any(call["name"] == "curl" for call in calls))


if __name__ == "__main__":
    unittest.main()
