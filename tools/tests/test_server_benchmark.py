from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.ci import server_benchmark as benchmark

ROOT = Path(__file__).resolve().parents[2]
HISTORICAL_BASE = "a64ea9f7297772f652802147c2ec103152b3edc9"


class BenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        self.write(".gitignore", "build/\nserver/build/\n")
        self.write(benchmark.WORKFLOW, "env:\n" + "\n".join((
            f"  CLASSIC_LINUX_IMAGE: {benchmark.LLVM_IMAGE}",
            f"  CLASSIC_LINUX_IMAGE_DIGEST: {benchmark.LLVM_IMAGE.split('@')[1]}",
            f"  CLASSIC_LINUX_CCACHE_COMPILER: {benchmark.LLVM_COMPILER}",
        )) + "\n")
        self.write(benchmark.RUNNER, "#!/bin/bash\nexit 0\n")
        self.write("cmake/toolchains/llvm.cmake", "# fixture own toolchain\n")
        self.write("cmake/AtrinikLLVM.cmake", "# fixture LLVM policy\n")
        for lock in benchmark.LOCKS:
            self.write(lock, "{}\n")
        self.git("init", "-q")
        self.git("config", "user.name", "Benchmark fixture")
        self.git("config", "user.email", "benchmark@example.invalid")
        self.revision = self.commit()

    def write(self, path, content):
        destination = self.root / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(content)

    def git(self, *arguments):
        return subprocess.run(["git", "-C", str(self.root), *arguments], check=True,
                              capture_output=True, text=True).stdout.strip()

    def commit(self):
        self.git("add", ".")
        self.git("-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
        return self.git("rev-parse", "HEAD")

    def historical_fixture(self):
        self.write(benchmark.WORKFLOW, (self.root / benchmark.WORKFLOW).read_text()
                   .replace(benchmark.LLVM_IMAGE, benchmark.GCC_IMAGE)
                   .replace(benchmark.LLVM_IMAGE.split('@')[1], benchmark.GCC_IMAGE.split('@')[1])
                   .replace(benchmark.LLVM_COMPILER, benchmark.GCC_COMPILER))
        digest = {path: hashlib.sha256((self.root / path).read_bytes()).hexdigest()
                  for path in benchmark.HISTORICAL_CONTRACT}
        return self.commit(), digest

    def test_current_selection_binds_locks_image_and_compiler(self):
        result = benchmark.resolve(self.root, self.revision)
        self.assertEqual(result["image"], benchmark.LLVM_IMAGE)
        self.assertEqual(result["compiler"], benchmark.LLVM_COMPILER)
        self.assertEqual(set(result["locks"]), set(benchmark.LOCKS))

    def test_exact_original_contract_and_later_source_commits(self):
        exists = subprocess.run(["git", "-C", str(ROOT), "cat-file", "-e",
                                 HISTORICAL_BASE + "^{commit}"], capture_output=True)
        if exists.returncode:
            self.skipTest("original benchmark contract unavailable in shallow history")
        source = Path(self.temporary.name) / "historical"
        subprocess.run(["git", "clone", "--quiet", "--shared", "--no-checkout", str(ROOT), str(source)], check=True)
        subprocess.run(["git", "-C", str(source), "checkout", "--quiet", "--detach", HISTORICAL_BASE], check=True)
        result = benchmark.resolve(source, HISTORICAL_BASE)
        self.assertEqual(result["image"], benchmark.GCC_IMAGE)
        self.assertEqual(result["compiler"], benchmark.GCC_COMPILER)
        # Source-only descendants retaining this producer contract also qualify.
        (source / "benchmark-descendant.txt").write_text("future source fixture\n")
        subprocess.run(["git", "-C", str(source), "add", "benchmark-descendant.txt"], check=True)
        subprocess.run(["git", "-C", str(source), "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                        "-c", "commit.gpgsign=false", "commit", "-qm", "future fixture"], check=True)
        revision = benchmark.git(source, "rev-parse", "HEAD").decode().strip()
        self.assertEqual(benchmark.resolve(source, revision)["compiler"], benchmark.GCC_COMPILER)

    def test_unknown_tampered_dirty_and_nonexact_inputs_rejected(self):
        for revision in ("HEAD", self.revision.upper(), "--detach", "f" * 39):
            with self.subTest(revision=revision), self.assertRaises((ValueError, subprocess.SubprocessError)):
                benchmark.resolve(self.root, revision)
        self.write("untracked", "tamper")
        with self.assertRaisesRegex(ValueError, "clean"):
            benchmark.resolve(self.root, self.revision)
        (self.root / "untracked").unlink()
        self.write(benchmark.WORKFLOW, (self.root / benchmark.WORKFLOW).read_text().replace("1.18.0", "9.99.0"))
        revision = self.commit()
        with self.assertRaisesRegex(ValueError, "approved compiler/image"):
            benchmark.resolve(self.root, revision)

    def test_historical_compiler_fields_alone_do_not_authorize_gcc(self):
        revision, _ = self.historical_fixture()
        with self.assertRaisesRegex(ValueError, "approved historical"):
            benchmark.resolve(self.root, revision)

    def test_symlinked_committed_toolchain_rejected(self):
        path = self.root / "cmake/toolchains/llvm.cmake"
        path.unlink()
        path.symlink_to("../AtrinikLLVM.cmake")
        revision = self.commit()
        with self.assertRaisesRegex(ValueError, "regular Git blob"):
            benchmark.resolve(self.root, revision)

    def test_container_is_host_launched_offline_without_socket_or_credentials(self):
        args = benchmark.container(benchmark.GCC_IMAGE, self.root, self.root / "cache",
                                   {"ATRINIK_LLVM_TOOLCHAIN": "0"}, ["python3", "fixture.py"])
        self.assertEqual(args[:4], ["docker", "run", "--rm", "--user"])
        self.assertIn("none", args)
        self.assertIn(f"{self.root}:{self.root}", args)
        self.assertIn("ATRINIK_LLVM_TOOLCHAIN=0", args)
        self.assertEqual(args[-3:], [benchmark.GCC_IMAGE, "python3", "fixture.py"])
        for forbidden in ("docker.sock", "GH_TOKEN", "--privileged"):
            self.assertNotIn(forbidden, " ".join(args))

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("gcc") and shutil.which("ctest"), "fixture compiler unavailable")
    def test_executable_historical_configure_build_and_only_required_probe(self):
        self.write("server/CMakeLists.txt", '''cmake_minimum_required(VERSION 3.20)
project(BenchmarkFixture C)
foreach(dependency ATRINIK_PROTOCOL LIBATRINIK LIBPCPNATPMP)
  if(NOT FETCHCONTENT_SOURCE_DIR_${dependency})
    message(FATAL_ERROR "Offline sibling source omitted")
  endif()
endforeach()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Historical fixture inherited the LLVM toolchain")
endif()
enable_testing()
add_executable(probe main.c)
add_test(NAME server-content-benchmark COMMAND probe)
add_test(NAME unrelated-must-not-run COMMAND ${CMAKE_COMMAND} -E false)
''')
        self.write("server/main.c", "int main(void) { return 0; }\n")
        self.write("server/CMakePresets.json", json.dumps({"version": 3, "configurePresets": [{
            "name": "linux-release", "generator": "Unix Makefiles",
            "cacheVariables": {"CMAKE_BUILD_TYPE": "Release"}}]}))
        revision, approved = self.historical_fixture()
        trusted = Path(self.temporary.name) / "trusted"
        dependencies = trusted / "server/tools/dependencies.py"
        dependencies.parent.mkdir(parents=True)
        dependencies.write_text("import sys\nif sys.argv[1] == 'source': print('/fixture/pcp')\n")
        inventory = trusted / "inventory.json"
        inventory.write_text(json.dumps({"target": "classic-final", "platform": "linux/amd64", "tools": {"gcc": "15.2.0"}}))
        binaries = trusted / "bin"
        binaries.mkdir()
        launcher = binaries / "ccache"
        launcher.write_text('#!/bin/sh\nexec "$@"\n')
        launcher.chmod(0o755)
        receipt = trusted / "receipt.json"
        with patch.object(benchmark, "HISTORICAL_CONTRACT", approved), patch.dict(
                os.environ, {"PATH": str(binaries) + os.pathsep + os.environ["PATH"],
                             "HOME": str(trusted / "home"), "ATRINIK_LLVM_TOOLCHAIN": "1", "CC": "false"}):
            benchmark.baseline_probe(self.root, revision, trusted, receipt, inventory)
        result = json.loads(receipt.read_text())
        self.assertEqual(result["compiler"], benchmark.GCC_COMPILER)
        self.assertTrue(result["completed"])
        cache = (self.root / "server/build/gcc-benchmark-baseline/CMakeCache.txt").read_text()
        self.assertIn("CMAKE_C_COMPILER:FILEPATH=" + shutil.which("gcc"), cache)
        self.assertNotIn("CMAKE_TOOLCHAIN_FILE", cache)
        # A matching regex that finds no test is an error, not successful evidence.
        self.write("server/CMakeLists.txt", (self.root / "server/CMakeLists.txt").read_text()
                   .replace("NAME server-content-benchmark", "NAME unrelated-probe"))
        revision = self.commit()
        with patch.object(benchmark, "HISTORICAL_CONTRACT", approved), patch.dict(
                os.environ, {"PATH": str(binaries) + os.pathsep + os.environ["PATH"],
                             "HOME": str(trusted / "home")}):
            with self.assertRaises(subprocess.CalledProcessError):
                benchmark.baseline_probe(self.root, revision, trusted, receipt, inventory)
        self.assertFalse(receipt.exists())

    def test_probe_wrong_inventory_fails_before_configure(self):
        inventory = Path(self.temporary.name) / "inventory.json"
        inventory.write_text(json.dumps({"target": "classic-final", "platform": "linux/amd64", "tools": {"clang": "22.0.0"}}))
        with patch.object(benchmark, "run", wraps=benchmark.run) as runner:
            with self.assertRaisesRegex(ValueError, "wrong compiler"):
                benchmark.baseline_probe(self.root, self.revision, self.root, self.root / "receipt", inventory)
        self.assertFalse(any(call.args[0][0] == "cmake" for call in runner.call_args_list))

    def test_compare_routes_cohorts_and_failure_cannot_publish_success(self):
        historical, approved = self.historical_fixture()
        self.write(benchmark.WORKFLOW, (self.root / benchmark.WORKFLOW).read_text()
                   .replace(benchmark.GCC_IMAGE, benchmark.LLVM_IMAGE)
                   .replace(benchmark.GCC_IMAGE.split('@')[1], benchmark.LLVM_IMAGE.split('@')[1])
                   .replace(benchmark.GCC_COMPILER, benchmark.LLVM_COMPILER))
        head = self.commit()
        actual_run = benchmark.run
        commands = []

        def execution(arguments, **kwargs):
            if arguments[0] != "docker":
                return actual_run(arguments, **kwargs)
            commands.append(arguments)
            if arguments[1] == "run" and benchmark.GCC_IMAGE in arguments:
                root = Path(arguments[arguments.index("--source-root") + 1])
                receipt = Path(arguments[arguments.index("--evidence") + 1])
                receipt.write_text(json.dumps(dict(benchmark.resolve(root, historical), completed=True)))
            return subprocess.CompletedProcess(arguments, 0)

        with patch.object(benchmark, "HISTORICAL_CONTRACT", approved), patch.object(benchmark, "run", side_effect=execution):
            benchmark.compare(self.root, historical, head, Path(self.temporary.name) / "cache")
        evidence = self.root / "build/ci-evidence"
        self.assertTrue((evidence / "server-content-benchmark-summary.md").exists())
        launches = [args for args in commands if args[1] == "run"]
        self.assertEqual(len(launches), 2)
        self.assertIn(benchmark.GCC_IMAGE, launches[0])
        self.assertIn("ATRINIK_LLVM_TOOLCHAIN=0", launches[0])
        self.assertIn(benchmark.LLVM_IMAGE, launches[1])
        self.assertIn("ATRINIK_LLVM_TOOLCHAIN=1", launches[1])
        self.assertTrue(any("/base/gcc-15.2.0:/cache/ccache" in part for part in launches[0]))
        self.assertTrue(any("/head/clang-23.1.3-lld-compiler-rt:/cache/ccache" in part for part in launches[1]))

        def fail(arguments, **kwargs):
            if arguments[0] == "docker" and benchmark.LLVM_IMAGE in arguments:
                raise subprocess.CalledProcessError(9, arguments)
            return execution(arguments, **kwargs)

        with patch.object(benchmark, "HISTORICAL_CONTRACT", approved), patch.object(benchmark, "run", side_effect=fail):
            with self.assertRaises(subprocess.CalledProcessError):
                benchmark.compare(self.root, historical, head, Path(self.temporary.name) / "cache")
        self.assertFalse((evidence / "server-content-benchmark-summary.md").exists())
        self.assertFalse((evidence / "server-content-benchmark-head.json").exists())
        self.assertFalse((self.root / "build/ci-server-benchmark-baseline").exists())

    def test_current_runner_keeps_llvm_and_requires_one_head_probe(self):
        runner = (ROOT / benchmark.RUNNER).read_text()
        server = runner[runner.index("  server-benchmark)\n"):runner.index("  integrated)\n")]
        self.assertIn('"${launcher[@]}"', server)
        self.assertIn("--no-tests=error -R '^server-content-benchmark$'", server)
        self.assertNotIn("worktree add", server)
        self.assertIn("export CC=clang CXX=clang++", runner)
        self.assertNotIn("export CC=gcc", runner)
        self.assertIn('"-DCMAKE_TOOLCHAIN_FILE=${source_root}/cmake/toolchains/llvm.cmake"', runner)


if __name__ == "__main__":
    unittest.main()
