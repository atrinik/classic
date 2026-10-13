#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute packaging entrypoints without application builds or dependency fetches."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MARKER = "# atrinik-windows-host-python-contract: 1"
HELPER = '''import json, os, sys
with open(os.environ["FIXTURE_LOG"], "a") as stream:
    stream.write(json.dumps({"tool": "helper", "executable": sys.executable,
                             "argv": sys.argv}) + "\\n")
'''
COMMAND = '''#!/usr/bin/python3
import json, os, pathlib, shutil, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["FIXTURE_LOG"], "a") as stream:
    stream.write(json.dumps({"tool": name, "argv": args}) + "\\n")
if name == "python3":
    os.execv("/usr/bin/python3", ["/usr/bin/python3", *args])
if name == "cmake":
    if args[0] == "-E":
        command, values = args[1], args[2:]
        if command == "remove_directory":
            for value in values:
                shutil.rmtree(value, ignore_errors=True)
        elif command == "make_directory":
            for value in values:
                pathlib.Path(value).mkdir(parents=True, exist_ok=True)
        elif command == "copy_directory":
            shutil.copytree(*values, dirs_exist_ok=True)
        elif command == "copy":
            for value in values[:-1]:
                shutil.copy(value, values[-1])
        else:
            raise SystemExit("unexpected cmake -E operation")
    elif args[0] == "--build":
        build = pathlib.Path(args[1])
        build.mkdir(parents=True, exist_ok=True)
        if "windows-region-generator" in str(build):
            executable = build / "atrinik-server"
            executable.write_text('#!/bin/bash\\nfor arg; do\\ncase "$arg" in --assetspath=*) mkdir -p "${arg#*=}/client-maps";; esac\\ndone\\n')
            executable.chmod(0o755)
            for name in ("libplugin_arena.so", "libplugin_python.so"):
                (build / name).touch()
        else:
            (build / "atrinik.exe").write_text("fixture production executable")
    elif args[0] != "-S":
        raise SystemExit("unexpected cmake invocation")
if name == "cpack":
    output = pathlib.Path(args[args.index("-B") + 1])
    output.mkdir(parents=True, exist_ok=True)
    (output / ("atrinik-classic-" + pathlib.Path.cwd().name + "-0.0.0-windows-x86_64.zip")).touch()
'''


class WindowsHostPythonContractTests(unittest.TestCase):
    def fixture(self, directory, component):
        root = Path(directory)
        module = root / component
        (module / "tools").mkdir(parents=True)
        script = module / "tools/build-windows-package.sh"
        shutil.copy(ROOT / component / "tools/build-windows-package.sh", script)
        self.assertIn(MARKER, script.read_text().splitlines())
        (module / "tools/dependencies.py").write_text(HELPER)
        client_tools = root / "client/tools"
        client_tools.mkdir(parents=True, exist_ok=True)
        (client_tools / "verify_gpu_fixture_provenance.py").write_text(HELPER)
        toolchain = module / "cmake/toolchains/llvm.cmake"
        toolchain.parent.mkdir(parents=True)
        toolchain.touch()
        for relative in ("runtime/content/maps", "runtime/content/lib",
                         "runtime/content/attribution", "resources", "install_data"):
            (module / relative).mkdir(parents=True)
        for relative in ("runtime/content/compatibility.json", "runtime/content/manifest.json",
                         "server.cfg", "permissions.cfg"):
            (module / relative).touch()
        binary_directory = root / "bin"
        binary_directory.mkdir()
        for name in ("python3", "cmake", "cpack", "llvm-readobj"):
            binary = binary_directory / name
            binary.write_text(COMMAND)
            binary.chmod(0o755)
        dependency = root / "dependency"
        dependency.touch()
        # Keep the fixture independent of caller virtual environments and Python paths.
        environment = {"PATH": str(binary_directory) + ":/usr/bin:/bin",
                       "HOME": str(root), "FIXTURE_LOG": str(root / "log.jsonl"),
                       "ATRINIK_PACKAGE_VERSION": "0.0.0",
                       "ATRINIK_WINDOWS_TARGET": "x86_64-w64-mingw32",
                       "VIRTUAL_ENV": str(root / "hostile-venv")}
        for name in ("TOOLCHAIN_FILE", "RUNTIME_DIR", "PYTHON_INCLUDE_DIR",
                     "PYTHON_LIBRARY", "PYTHON_RUNTIME_DIR"):
            environment["ATRINIK_WINDOWS_" + name] = str(dependency)
        return module, environment

    def run_fixture(self, module, environment):
        result = subprocess.run(["/bin/bash", "tools/build-windows-package.sh"],
                                cwd=module, env=environment, capture_output=True,
                                text=True, timeout=30)
        log = Path(environment["FIXTURE_LOG"])
        records = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return result, records

    def test_bound_helpers_and_all_configure_entrypoints(self):
        for component in ("client", "server"):
            with self.subTest(component=component), tempfile.TemporaryDirectory() as directory:
                module, environment = self.fixture(directory, component)
                environment["ATRINIK_WINDOWS_HOST_PYTHON"] = "/usr/bin/python3"
                result, records = self.run_fixture(module, environment)
                self.assertEqual(result.returncode, 0, result.stderr)
                helpers = [record for record in records if record["tool"] == "helper"]
                self.assertEqual(len(helpers), 3)
                self.assertTrue(all(record["executable"] == "/usr/bin/python3" for record in helpers))
                self.assertFalse(any(record["tool"] == "python3" for record in records))
                configurations = [record["argv"] for record in records
                                  if record["tool"] == "cmake" and record["argv"][0] == "-S"]
                self.assertEqual([args[args.index("-B") + 1] for args in configurations],
                                 ["build/windows-release"] if component == "client" else
                                 ["build/windows-region-generator", "build/windows-release"])
                for args in configurations:
                    self.assertIn("-DPython3_EXECUTABLE=/usr/bin/python3", args)
                    self.assertIn("-DPython3_FIND_VIRTUALENV=STANDARD", args)
                if component == "server":
                    for suffix in ("INCLUDE_DIR", "LIBRARY", "RUNTIME_DIR"):
                        name = "ATRINIK_WINDOWS_PYTHON_" + suffix
                        self.assertIn("-D" + name + "=" + environment[name], configurations[-1])

    def test_unset_binding_preserves_generic_discovery(self):
        for component in ("client", "server"):
            with self.subTest(component=component), tempfile.TemporaryDirectory() as directory:
                module, environment = self.fixture(directory, component)
                result, records = self.run_fixture(module, environment)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(sum(record["tool"] == "python3" for record in records), 3)
                for record in records:
                    if record["tool"] == "cmake" and record["argv"][0] == "-S":
                        self.assertFalse(any(arg.startswith("-DPython3_") for arg in record["argv"]))

    def test_unknown_or_empty_binding_fails_before_helpers_and_mutations(self):
        for component in ("client", "server"):
            for value in ("", "python3", "/usr/local/bin/python3", "/usr/bin/../bin/python3"):
                with self.subTest(component=component, value=value), tempfile.TemporaryDirectory() as directory:
                    module, environment = self.fixture(directory, component)
                    environment["ATRINIK_WINDOWS_HOST_PYTHON"] = value
                    result, records = self.run_fixture(module, environment)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("ATRINIK_WINDOWS_HOST_PYTHON", result.stderr)
                    self.assertEqual(records, [])
                    self.assertFalse((module / "build").exists())


if __name__ == "__main__":
    unittest.main()
