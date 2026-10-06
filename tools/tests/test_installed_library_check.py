# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations

import io
import json
import os
import shutil
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "tools/ci/check_installed_library.sh"
IMAGE = "ghcr.io/atrinik/classic-build:1.16.0@sha256:" + "e1c366dbf83ef987765ff913bbb193868314a139ae1e00cc134b22a3159464f2"
VERSION = "5.9.1"
REVISION = "a" * 40


class InstalledLibraryCheckTests(unittest.TestCase):
    def run_fixture(self, *, bad_revision=False, failure="", existing_output=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "library.tar.gz"
            with tarfile.open(archive, "w:gz") as package:
                for prefix in ("", "/dependencies/protocol"):
                    for name, value in (("VERSION", VERSION), ("SOURCE_REVISION", "b" * 40 if bad_revision else REVISION)):
                        payload = (value + "\n").encode()
                        member = tarfile.TarInfo(f"atrinik-classic-libatrinik-{VERSION}{prefix}/{name}")
                        member.size = len(payload)
                        package.addfile(member, io.BytesIO(payload))
            system = root / "system"
            (system / "include/curl").mkdir(parents=True)
            (system / "libcurl.so").touch()
            (system / "include/curl/curl.h").touch()
            original = archive.read_bytes()
            binary = root / "bin"
            binary.mkdir()
            docker = binary / "docker"
            docker.write_text('''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys
args = sys.argv[1:]
pathlib.Path(os.environ["TRACE"]).write_text(json.dumps(args))
mounts = [args[i+1] for i, value in enumerate(args) if value == "--mount"]
paths = {}
for mount in mounts:
    fields = dict(piece.split("=", 1) for piece in mount.split(",") if "=" in piece)
    paths[fields["target"]] = fields["source"]
body = sys.stdin.read().replace("/input/library.tar.gz", paths["/input/library.tar.gz"])
body = body.replace("/usr/lib/x86_64-linux-gnu/libcurl.so", str(pathlib.Path(os.environ["SYSTEM_CURL"]) / "libcurl.so"))
body = body.replace("/usr/include/x86_64-linux-gnu", str(pathlib.Path(os.environ["SYSTEM_CURL"]) / "include"))
sys.exit(subprocess.run(["bash", "-s", "--", *args[-2:]], input=body, text=True, cwd=paths["/output"]).returncode)
''')
            docker.chmod(0o755)
            cmake = binary / "cmake"
            cmake.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ["CMAKE_TRACE"], "a") as stream:
    stream.write(json.dumps(args) + "\\n")
if any("system-curl" in arg for arg in args):
    print("unqualified CURL::libcurl provider: fixture system target")
    sys.exit(1)
failure = os.environ.get("FAILURE", "")
phase = "configure" if "-S" in args else "build" if "--build" in args else "install"
if failure == phase:
    sys.exit(9)
if args[:2] == ["--build", "consumer"]:
    pathlib.Path("consumer").mkdir(exist_ok=True)
    exe = pathlib.Path("consumer/libatrinik-consumer")
    exe.write_text("#!/usr/bin/env bash\\nexit " + ("9" if failure == "execute" else "0") + "\\n")
    exe.chmod(0o755)
''')
            cmake.chmod(0o755)
            output = root / "output"
            if existing_output:
                output.mkdir()
            trace = root / "docker.json"
            cmake_trace = root / "cmake.jsonl"
            env = dict(os.environ, PATH=f"{binary}:{os.environ['PATH']}", TRACE=str(trace), CMAKE_TRACE=str(cmake_trace), FAILURE=failure, SYSTEM_CURL=str(system))
            result = subprocess.run(["bash", str(HELPER), IMAGE, str(archive), VERSION, REVISION, str(output)], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(original, archive.read_bytes())
            return result, json.loads(trace.read_text()) if trace.exists() else [], [json.loads(line) for line in cmake_trace.read_text().splitlines()] if cmake_trace.exists() else []

    def test_executes_exact_version_installed_consumer_in_fenced_container(self):
        result, args, calls = self.run_fixture()
        self.assertEqual(result.returncode, 0, result.stderr)
        for option, value in (("--network", "none"), ("--cap-drop", "ALL"), ("--security-opt", "no-new-privileges"), ("--user", f"{os.getuid()}:{os.getgid()}"), ("--pull", "never")):
            self.assertEqual(args[args.index(option) + 1], value)
        self.assertIn("--read-only", args)
        self.assertIn(IMAGE, args)
        mounts = [args[index + 1] for index, value in enumerate(args) if value == "--mount"]
        self.assertEqual(len(mounts), 2)
        self.assertTrue(mounts[0].endswith("target=/input/library.tar.gz,readonly"))
        self.assertFalse(any(".git" in mount or ".docker" in mount for mount in mounts))
        self.assertNotIn("--env", args)
        self.assertEqual(len(calls), 7)
        for call in (calls[0], calls[3]):
            self.assertTrue(any(arg.startswith("-DCMAKE_PROJECT_INCLUDE=") for arg in call))
        for call in calls[-2:]:
            self.assertIn("-DCURL_NO_CURL_CMAKE=TRUE", call)
            self.assertTrue(any(arg.startswith("-DCURL_LIBRARY_RELEASE=") for arg in call))
        self.assertEqual(calls[2], ["--install", "library"])
        self.assertIn(f"-DATRINIK_CONSUMER_VERSION={VERSION}", calls[3])
        self.assertTrue(any(value.endswith("/install") and value.startswith("-DCMAKE_PREFIX_PATH=") for value in calls[3]))
        self.assertFalse(any("LINKER_FLAGS" in arg for call in calls for arg in call))

    @unittest.skipUnless(shutil.which("cmake"), "CMake unavailable")
    def test_real_cmake_deferred_imported_target_guard(self):
        # Run the exact generated guard with fixture-owned provider roots; no
        # native compiler, real system library writes or image pulls are needed.
        helper = HELPER.read_text()
        guard = helper.split("cat > provider-guard.cmake <<'CMAKE'\n", 1)[1].split("\nCMAKE\n", 1)[0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            qualified = root / "local"
            (qualified / "lib").mkdir(parents=True)
            (qualified / "include/curl").mkdir(parents=True)
            (qualified / "lib/libcurl.so.4").touch()
            (qualified / "include/curl/curl.h").touch()
            system = root / "system"
            system.mkdir()
            (system / "curl").mkdir()
            (system / "curl/curl.h").touch()
            (system / "libcurl.so").touch()
            hook = root / "guard.cmake"
            hook.write_text(guard.replace("/usr/local", str(qualified)))
            for scenario, location, include, accepted in (
                ("qualified", qualified / "lib/libcurl.so.4", qualified / "include", True),
                ("qualified-release", qualified / "lib/libcurl.so.4", qualified / "include", True),
                ("system-library", system / "libcurl.so", qualified / "include", False),
                ("system-header", qualified / "lib/libcurl.so.4", system, False),
            ):
                with self.subTest(scenario=scenario):
                    source = root / scenario
                    source.mkdir()
                    location_property = "IMPORTED_LOCATION_RELEASE" if scenario == "qualified-release" else "IMPORTED_LOCATION"
                    (source / "CMakeLists.txt").write_text(
                        'cmake_minimum_required(VERSION 3.21)\nproject(provider LANGUAGES NONE)\n'
                        'add_library(CURL::libcurl UNKNOWN IMPORTED)\n'
                        f'set_target_properties(CURL::libcurl PROPERTIES IMPORTED_CONFIGURATIONS RELEASE {location_property} "{location}" INTERFACE_INCLUDE_DIRECTORIES "{include}")\n'
                    )
                    build = root / (scenario + "-build")
                    result = subprocess.run(["cmake", "-S", str(source), "-B", str(build), f"-DCMAKE_PROJECT_INCLUDE={hook}"], capture_output=True, text=True, timeout=15)
                    if accepted:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        record = (build / "curl-provider.txt").read_text()
                        self.assertIn(str(location), record)
                        self.assertIn(str(include / "curl/curl.h"), record)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("unqualified CURL::libcurl provider:", result.stderr)

    def test_archive_identity_mismatch_prevents_configuration(self):
        result, _, calls = self.run_fixture(bad_revision=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_existing_output_is_never_reused(self):
        result, args, calls = self.run_fixture(existing_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(args, [])
        self.assertEqual(calls, [])

    def test_native_and_executable_failures_are_fatal(self):
        for failure in ("configure", "build", "install", "execute"):
            with self.subTest(failure=failure):
                result, _, _ = self.run_fixture(failure=failure)
                self.assertEqual(result.returncode, 9, result.stderr)


if __name__ == "__main__":
    unittest.main()
