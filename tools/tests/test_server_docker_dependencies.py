# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 The Atrinik Project
"""Execute Docker's immutable-source staging and real offline CMake consumer."""

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("server_dependencies", ROOT / "server/tools/dependencies.py")
dependencies = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dependencies)


def source_staging_command():
    docker = (ROOT / "server/Dockerfile").read_text()
    match = re.search(r"^RUN python3 tools/dependencies.py source(?:[^\n]*\\\n)*[^\n]*", docker, re.MULTILINE)
    if match is None:
        raise AssertionError("Missing locked native source staging step")
    return match.group(0).removeprefix("RUN ").replace("\\\n", " ")


class ServerDockerDependenciesTests(unittest.TestCase):
    def fixture(self):
        temporary = tempfile.TemporaryDirectory(dir=os.environ.get("ATRINIK_TEST_TMPDIR"))
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        server = root / "server"
        (server / "cmake").mkdir(parents=True)
        (server / "tools").mkdir()
        for name in ("pcpnatpmp.cmake", "immutable_source_cache.cmake"):
            shutil.copy2(ROOT / "server/cmake" / name, server / "cmake" / name)
        shutil.copy2(ROOT / "server/tools/dependencies.py", server / "tools/dependencies.py")
        contents = {"CMakeLists.txt": b"cmake_minimum_required(VERSION 3.21)\nproject(fixture_pcp NONE)\nadd_library(pcpnatpmp INTERFACE)\n",
                    "README.md": b"checksum-bound fixture source\n"}
        archive = root / "fixture.tar.gz"
        with tarfile.open(archive, "w:gz") as stream:
            for name, data in contents.items():
                member = tarfile.TarInfo("fixture/" + name)
                member.size = len(data)
                stream.addfile(member, io.BytesIO(data))
        manifest = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in sorted(contents.items()))
        production = json.loads((ROOT / "server/cmake/immutable_sources.lock.json").read_text())
        source = production["sources"]["libpcpnatpmp"]
        source.update(url=archive.as_uri(), sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                      tree_sha256=hashlib.sha256(manifest.encode()).hexdigest())
        lock_path = server / "cmake/immutable_sources.lock.json"
        lock_path.write_text(json.dumps(production))
        (server / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.21)\nproject(offline_fixture NONE)\n'
                                                'include(cmake/pcpnatpmp.cmake)\n'
                                                'file(WRITE "${CMAKE_BINARY_DIR}/cache-used.txt" "${ATRINIK_DEPENDENCY_CACHE_DIR}")\n')
        deny = root / "deny-network"
        deny.mkdir()
        (deny / "sitecustomize.py").write_text('import urllib.request\n'
                                               'def deny(*args, **kwargs):\n'
                                               '    raise RuntimeError("fixture network forbidden")\n'
                                               'urllib.request.urlopen = deny\n')
        return root, server, archive, source, lock_path, deny

    def stage(self, server):
        return subprocess.run(["sh", "-eu", "-c", source_staging_command()], cwd=server,
                              capture_output=True, text=True)

    def configure_offline(self, server, lock_path, deny):
        lock = json.loads(lock_path.read_text())
        # Canonical production HTTPS URL remains unreachable in this fixture.
        # CMake must use and reverify the staged archive/tree without acquiring it.
        lock["sources"]["libpcpnatpmp"]["url"] = json.loads(
            (ROOT / "server/cmake/immutable_sources.lock.json").read_text())["sources"]["libpcpnatpmp"]["url"]
        lock_path.write_text(json.dumps(lock))
        environment = dict(os.environ, PYTHONPATH=str(deny), PYTHONDONTWRITEBYTECODE="1")
        return subprocess.run(["cmake", "-S", str(server), "-B", str(server / "build/release"),
                               f"-DPython3_EXECUTABLE={sys.executable}"],
                              capture_output=True, text=True, env=environment)

    def test_exact_docker_stage_feeds_real_cmake_default_cache_offline(self):
        _, server, archive, source, lock_path, deny = self.fixture()
        result = self.stage(server)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        cache = server / "build/dependency-cache"
        cached_archive = cache / "downloads" / f"libpcpnatpmp-{source['sha256']}.tar.gz"
        self.assertEqual(cached_archive.read_bytes(), archive.read_bytes())
        staged = Path(result.stdout.strip())
        if not staged.is_absolute():
            staged = server / staged
        self.assertEqual(dependencies.source_tree_sha256(staged), source["tree_sha256"])
        archive.unlink()
        result = self.configure_offline(server, lock_path, deny)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        used = (server / "build/release/cache-used.txt").read_text()
        self.assertEqual(Path(used).resolve(), cache.resolve())
        self.assertNotIn("fixture network forbidden", result.stdout + result.stderr)

    def test_stage_rejects_missing_lock_and_unchecked_archive_bytes(self):
        for mutation in ("missing-lock", "changed-download", "changed-tree-lock"):
            with self.subTest(mutation=mutation):
                _, server, archive, _, lock_path, _ = self.fixture()
                if mutation == "missing-lock":
                    lock_path.unlink()
                elif mutation == "changed-download":
                    archive.write_bytes(b"unreviewed replacement archive")
                else:
                    lock = json.loads(lock_path.read_text())
                    lock["sources"]["libpcpnatpmp"]["tree_sha256"] = "0" * 64
                    lock_path.write_text(json.dumps(lock))
                result = self.stage(server)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("dependency error:", result.stderr)
                self.assertFalse((server / "build/dependency-cache/sources-v1").exists() and
                                 any(p.name.startswith("libpcpnatpmp-") for p in
                                     (server / "build/dependency-cache/sources-v1").iterdir()))

    def test_cmake_revalidates_tree_and_refuses_missing_cache_with_network_denied(self):
        for mutation in ("source-bytes", "source-marker", "missing-cache"):
            with self.subTest(mutation=mutation):
                _, server, _, _, lock_path, deny = self.fixture()
                staged = self.stage(server)
                self.assertEqual(staged.returncode, 0, staged.stdout + staged.stderr)
                source = Path(staged.stdout.strip())
                if not source.is_absolute():
                    source = server / source
                if mutation == "source-bytes":
                    (source / "README.md").write_text("unchecked extracted tree")
                elif mutation == "source-marker":
                    (source / ".atrinik-source-sha256").write_text("unchecked marker")
                else:
                    shutil.rmtree(server / "build/dependency-cache")
                result = self.configure_offline(server, lock_path, deny)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                failure = result.stdout + result.stderr
                self.assertIn("Immutable libpcpnatpmp acquisition failed", failure)
                if mutation == "missing-cache":
                    self.assertIn("fixture network forbidden", " ".join(failure.split()))
                else:
                    self.assertIn("mismatched shared source", failure)
                    self.assertNotIn("fixture network forbidden", failure)

    def test_reachable_configure_sources_are_local_or_staged_and_compile_stays_offline(self):
        docker = (ROOT / "server/Dockerfile").read_text()
        source_lock = json.loads((ROOT / "server/cmake/immutable_sources.lock.json").read_text())
        declared = re.findall(r"FetchContent_Declare\((\w+)",
                              "\n".join((ROOT / path).read_text() for path in
                                        ("server/CMakeLists.txt", "libatrinik/CMakeLists.txt", "server/cmake/pcpnatpmp.cmake")))
        self.assertEqual(set(declared), {"atrinik_protocol", "libatrinik", "libpcpnatpmp"})
        self.assertEqual(set(source_lock["sources"]), {"libpcpnatpmp"})
        self.assertIn("--source-name libpcpnatpmp", source_staging_command())
        self.assertIn("--source-lock cmake/immutable_sources.lock.json", source_staging_command())
        self.assertIn("--cache build/dependency-cache", source_staging_command())
        compile_start = docker.index("RUN --network=none python3 tools/dependencies.py sync")
        self.assertLess(docker.index("RUN python3 tools/dependencies.py source"), compile_start)
        compile_step = docker[compile_start:docker.index("\n\n", compile_start)]
        self.assertIn("--refresh --offline", compile_step)
        for override in ("-DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL=/src/protocol",
                         "-DFETCHCONTENT_SOURCE_DIR_LIBATRINIK=/src/libatrinik"):
            self.assertIn(override, compile_step)
        self.assertNotIn("FETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP", compile_step)
        self.assertNotIn("CMAKE_DISABLE_FIND_PACKAGE", compile_step)
        self.assertNotIn("--network=default", compile_step)


if __name__ == "__main__":
    unittest.main()
