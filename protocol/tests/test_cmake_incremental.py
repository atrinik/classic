from __future__ import annotations

import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


class CMakeIncrementalTests(unittest.TestCase):
    def run_command(self, *args: str, cwd: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            args,
            cwd=cwd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    def test_generator_check_is_dependency_stamped(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            source = temporary / "protocol"
            build = temporary / "build"
            shutil.copytree(ROOT, source, ignore=shutil.ignore_patterns("build"))
            configured = self.run_command(
                "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                "-DBUILD_TESTING=OFF", cwd=temporary
            )
            self.assertEqual(configured.returncode, 0, configured.stdout)

            initial = self.run_command("cmake", "--build", str(build), cwd=temporary)
            self.assertEqual(initial.returncode, 0, initial.stdout)
            self.assertIn("Checking generated classic game protocol bindings", initial.stdout)
            no_op = self.run_command("cmake", "--build", str(build), cwd=temporary)
            self.assertEqual(no_op.returncode, 0, no_op.stdout)
            self.assertIn("no work to do", no_op.stdout)

            dependencies = (
                source / "schema/game-commands.json",
                source / "tools/generate.py",
                source / "generated/c/include/atrinik/protocol/game_commands.h",
            )
            for dependency in dependencies:
                time.sleep(0.02)
                dependency.touch()
                rebuilt = self.run_command("cmake", "--build", str(build), cwd=temporary)
                self.assertEqual(rebuilt.returncode, 0, rebuilt.stdout)
                self.assertIn(
                    "Checking generated classic game protocol bindings", rebuilt.stdout
                )

            generated = dependencies[-1]
            generated.write_text(generated.read_text(encoding="utf-8") + "\n", encoding="utf-8")
            rejected = self.run_command("cmake", "--build", str(build), cwd=temporary)
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("out of date:", rejected.stdout)

    def test_integrated_build_does_not_register_duplicate_check(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            source = temporary / "source"
            build = temporary / "build"
            source.mkdir()
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.21)\n"
                "project(protocol-parent LANGUAGES NONE)\n"
                "include(CTest)\n"
                f'add_subdirectory("{ROOT.as_posix()}" protocol)\n',
                encoding="utf-8",
            )
            configured = self.run_command(
                "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                "-DBUILD_TESTING=ON", cwd=temporary
            )
            self.assertEqual(configured.returncode, 0, configured.stdout)
            listed = self.run_command("ctest", "--test-dir", str(build), "-N", cwd=temporary)
            self.assertEqual(listed.returncode, 0, listed.stdout)
            self.assertNotIn("Test #1: protocol-generate-check", listed.stdout)
            self.assertIn("protocol-python-tests", listed.stdout)


if __name__ == "__main__":
    unittest.main()
