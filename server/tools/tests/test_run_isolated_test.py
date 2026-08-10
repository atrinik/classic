from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "run_isolated_test.py"
SPEC = importlib.util.spec_from_file_location("run_isolated_test", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class IsolatedServerTestRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.seed = self.root / "seed"
        server = self.seed / "server"
        for directory in ("data", "lib", "maps/python/events", "resources"):
            (server / directory).mkdir(parents=True, exist_ok=True)
        (self.seed / ".prepared").touch()
        (server / "data/seed").write_text("clean\n", encoding="utf-8")
        (server / "maps/world").write_text("read only\n", encoding="utf-8")
        (server / "maps/python/events/python_unit.py").write_text(
            "content\n", encoding="utf-8"
        )
        (server / "server.cfg").write_text("config\n", encoding="utf-8")
        self.runtimes = self.root / "runtimes"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_prepares_fresh_named_runtime_with_private_writes(self) -> None:
        first = runner.prepare_runtime(self.seed, self.runtimes, "server.unit", False)
        (first / "server/data/changed").write_text("changed\n", encoding="utf-8")
        (first / "server/tests").mkdir()
        second = runner.prepare_runtime(self.seed, self.runtimes, "server.unit", False)

        self.assertEqual(first, second)
        self.assertFalse((second / "server/data/changed").exists())
        self.assertFalse((second / "server/tests").exists())
        self.assertTrue((second / "server/maps").is_symlink())
        self.assertTrue((second / "server/lib").is_symlink())
        self.assertTrue((second / "server/resources").is_symlink())
        self.assertEqual((second / "server/data/seed").read_text(), "clean\n")

    def test_python_map_tree_is_private_while_other_maps_are_shared(self) -> None:
        runtime = runner.prepare_runtime(self.seed, self.runtimes, "plugin", True)
        maps = runtime / "server/maps"
        self.assertFalse(maps.is_symlink())
        self.assertTrue((maps / "world").is_symlink())
        self.assertFalse((maps / "python").is_symlink())
        entrypoint = maps / "python/events/python_unit.py"
        entrypoint.write_text("fixture\n", encoding="utf-8")
        self.assertEqual(
            (self.seed / "server/maps/python/events/python_unit.py").read_text(),
            "content\n",
        )

    def test_rejects_names_that_can_escape_the_runtime_root(self) -> None:
        for name in ("", ".hidden", "../escape", "a..b", "slash/name"):
            with self.subTest(name=name), self.assertRaisesRegex(
                RuntimeError, "invalid server test name"
            ):
                runner.prepare_runtime(self.seed, self.runtimes, name, False)

    def test_command_receives_private_working_directory_and_environment(self) -> None:
        command = [
            sys.executable,
            "-c",
            (
                "import os,pathlib; "
                "pathlib.Path('result').write_text(os.environ['TMPDIR'] + '\\n' + "
                "os.environ['ATRINIK_TEST_ARTIFACT_DIR'])"
            ),
        ]
        result = runner.main(
            [
                "--seed",
                str(self.seed),
                "--runtimes",
                str(self.runtimes),
                "--name",
                "environment",
                "--",
                *command,
            ]
        )
        runtime = self.runtimes / "environment"
        self.assertEqual(result, 0)
        self.assertEqual(
            (runtime / "server/result").read_text().splitlines(),
            [str(runtime / "tmp"), str(runtime)],
        )

    def test_failed_command_retains_its_named_artifact(self) -> None:
        result = runner.main(
            [
                "--seed",
                str(self.seed),
                "--runtimes",
                str(self.runtimes),
                "--name",
                "failing-test",
                "--",
                sys.executable,
                "-c",
                (
                    "import pathlib,sys; "
                    "pathlib.Path('failure.log').write_text('details'); "
                    "sys.exit(7)"
                ),
            ]
        )

        self.assertEqual(result, 7)
        self.assertEqual(
            (self.runtimes / "failing-test/server/failure.log").read_text(),
            "details",
        )

    @unittest.skipUnless(hasattr(os, "killpg"), "requires POSIX process groups")
    def test_timeout_reaps_the_complete_process_group(self) -> None:
        pid_file = self.root / "child.pid"
        command = [
            sys.executable,
            "-c",
            (
                "import pathlib,subprocess,sys,time; "
                "child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']); "
                f"pathlib.Path({str(pid_file)!r}).write_text(str(child.pid)); "
                "time.sleep(60)"
            ),
        ]
        result = runner.run_owned(command, self.root, 0.5, os.environ.copy())
        self.assertEqual(result, 124)
        child_pid = int(pid_file.read_text())
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                os.kill(child_pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.05)
        else:
            self.fail(f"child process {child_pid} survived timeout cleanup")

    @unittest.skipUnless(hasattr(os, "killpg"), "requires POSIX process groups")
    def test_completed_parent_cannot_leave_a_descendant_running(self) -> None:
        for returncode in (0, 7):
            with self.subTest(returncode=returncode):
                pid_file = self.root / f"child-{returncode}.pid"
                command = [
                    sys.executable,
                    "-c",
                    (
                        "import pathlib,subprocess,sys; "
                        "child=subprocess.Popen([sys.executable,'-c',"
                        "'import time;time.sleep(60)']); "
                        f"pathlib.Path({str(pid_file)!r}).write_text(str(child.pid)); "
                        f"sys.exit({returncode})"
                    ),
                ]
                result = runner.run_owned(
                    command, self.root, 10, os.environ.copy()
                )
                self.assertEqual(result, returncode)
                child_pid = int(pid_file.read_text())
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    try:
                        os.kill(child_pid, 0)
                    except ProcessLookupError:
                        break
                    time.sleep(0.05)
                else:
                    self.fail(
                        f"child process {child_pid} survived parent exit"
                    )


if __name__ == "__main__":
    unittest.main()
