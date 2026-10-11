"""Check the argv produced by the generated Windows runtime install script."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "CMake unavailable")
class WindowsRuntimeCMakeArgumentsTests(unittest.TestCase):
    def test_generated_install_preserves_argument_boundaries(self):
        for python_runtime in (False, True):
            with self.subTest(python_runtime=python_runtime), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary) / "source with spaces"
                root.mkdir()
                runtime = root / "toolchain with spaces/bin"
                runtime.mkdir(parents=True)
                share = runtime.parent / "share"
                llvm_notices = share / "licenses/llvm-mingw"
                llvm_notices.mkdir(parents=True)
                for name in ("LICENSE.TXT", "COPYING", "COPYING.MinGW-w64-runtime.txt",
                             "COPYING.MinGW-w64.txt", "COPYING.winpthreads.txt",
                             "COPYING.winstorecompat.txt"):
                    (llvm_notices / name).write_text("fixture notice\n")
                dependency_notices = share / "licenses/fixture"
                dependency_notices.mkdir()
                (dependency_notices / "LICENSE").write_text("fixture notice\n")
                inventory = share / "atrinik/windows-dependencies"
                inventory.mkdir(parents=True)
                (inventory / "dependencies.json").write_text(json.dumps({
                    "schema_version": 1,
                    "dependencies": [{"name": "fixture", "licenses": ["LICENSE"]}],
                }))
                (inventory / "pe-closure.json").write_text("{}")
                inspector = root / "llvm readobj"
                inspector.write_text("fixture inspector\n")
                helper = root / "tools/ci/stage_windows_runtime.py"
                helper.parent.mkdir(parents=True)
                helper.write_text(
                    "import argparse, json, pathlib, sys\n"
                    "parser = argparse.ArgumentParser()\n"
                    "parser.add_argument('--readobj', required=True)\n"
                    "parser.add_argument('--dlls-only', action='store_true')\n"
                    "parser.add_argument('--output-dir', required=True)\n"
                    "parser.add_argument('--runtime-dir', required=True)\n"
                    "parser.add_argument('--producer-inventory', required=True)\n"
                    "parser.add_argument('--python-runtime')\n"
                    "parser.add_argument('--python-inventory')\n"
                    "parser.add_argument('binaries', nargs='+')\n"
                    "args = parser.parse_args()\n"
                    "output = pathlib.Path(args.output_dir)\n"
                    "output.mkdir()\n"
                    "(output / 'argv.json').write_text(json.dumps(sys.argv[1:]))\n"
                )
                python = root / "python runtime with spaces"
                binaries = [root / "client binary.exe", root / "arena plugin.dll"]
                cmake_source = (
                    "cmake_minimum_required(VERSION 3.21)\n"
                    "project(runtime_arguments NONE)\n"
                    f'include("{ROOT}/cmake/InstallWindowsRuntime.cmake")\n'
                    f'set(ATRINIK_WINDOWS_RUNTIME_DIR "{runtime}")\n'
                    f'set(ATRINIK_LLVM_READOBJ "{inspector}")\n'
                )
                if python_runtime:
                    cmake_source += f'set(ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR "{python}")\n'
                for index, binary in enumerate(binaries):
                    cmake_source += (
                        f"add_executable(binary{index} IMPORTED)\n"
                        f'set_target_properties(binary{index} PROPERTIES IMPORTED_LOCATION "{binary}")\n'
                    )
                cmake_source += ('atrinik_install_windows_runtime("runtime destination"\n'
                                 '    "$<TARGET_FILE:binary0>" "$<TARGET_FILE:binary1>")\n')
                (root / "CMakeLists.txt").write_text(cmake_source)
                build = root / "build with spaces"
                prefix = root / "install with spaces"
                for command in (["cmake", "-S", str(root), "-B", str(build)],
                                ["cmake", "--install", str(build), "--prefix", str(prefix)]):
                    result = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                expected = ["--readobj", str(inspector), "--dlls-only", "--output-dir",
                            str(build / "windows-runtime-stage"), "--runtime-dir", str(runtime),
                            "--producer-inventory",
                            str(runtime / "../share/atrinik/windows-dependencies/pe-closure.json")]
                if python_runtime:
                    expected += ["--python-runtime", str(python), "--python-inventory",
                                 str(runtime / "../share/atrinik/windows-dependencies/python-runtime.json")]
                expected += [str(binary) for binary in binaries]
                actual = json.loads((prefix / "runtime destination/argv.json").read_text())
                self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
