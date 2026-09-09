from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import tarfile

from tools.release import package_sources
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake" / "AtrinikVersion.cmake"


class VersionMetadataTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "main.c").write_text(
            """
#include <stdio.h>

int main(void) {
    printf("%s|%d|%d|%d|%s|%s|%s|%s|%s|%s\\n",
           PACKAGE_VERSION,
           PACKAGE_VERSION_MAJOR,
           PACKAGE_VERSION_MINOR,
           PACKAGE_VERSION_PATCH,
           ATRINIK_BUILD_TYPE,
           ATRINIK_COMPILER_ID,
           ATRINIK_COMPILER_VERSION,
           ATRINIK_SYSTEM_NAME,
           ATRINIK_BENCHMARK_REVISION, ATRINIK_BENCHMARK_DIRTY);
    return 0;
}
""".lstrip(),
            encoding="utf-8",
        )
        (self.source / "CMakeLists.txt").write_text(
            f"""
cmake_minimum_required(VERSION 3.21)
include("{MODULE.as_posix()}")
atrinik_resolve_version(ATRINIK_SOURCE_VERSION)
project(version-probe VERSION "${{ATRINIK_SOURCE_VERSION}}" LANGUAGES C)
atrinik_initialize_version_metadata()
foreach(consumer IN ITEMS client-version-probe server-version-probe)
    add_executable(${{consumer}} main.c)
    atrinik_apply_version_metadata(${{consumer}})
endforeach()
""".lstrip(),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def configure(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["cmake", "-Werror=dev", "-S", str(self.source), "-B", str(self.root / "build"), *arguments],
            check=False,
            capture_output=True,
            text=True,
        )

    def build_and_outputs(self) -> tuple[str, str]:
        subprocess.run(
            ["cmake", "--build", str(self.root / "build"), "--parallel", "2"],
            check=True,
            capture_output=True,
            text=True,
        )
        outputs = []
        for consumer in ("client-version-probe", "server-version-probe"):
            result = subprocess.run(
                [str(self.root / "build" / consumer)],
                check=True,
                capture_output=True,
                text=True,
            )
            outputs.append(result.stdout.strip())
        return outputs[0], outputs[1]

    def assert_component_version_probe(self, source: Path, build: Path, *, version: str = "9.8.7", explicit: bool = True) -> None:
        probe = self.root / "component-version-probe.cmake"
        probe.write_text(
            """
if (NOT PROJECT_VERSION STREQUAL "9.8.7")
    message(FATAL_ERROR "Unexpected project version: ${PROJECT_VERSION}")
endif ()
message(FATAL_ERROR "ATRINIK_VERSION_PROBE_COMPLETED")
""".lstrip().replace("9.8.7", version),
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                "cmake",
                "-S",
                str(source),
                "-B",
                str(build),
                *(["-DATRINIK_PACKAGE_VERSION=" + version] if explicit else []),
                f"-DCMAKE_PROJECT_INCLUDE={probe}",
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "ATRINIK_VERSION_PROBE_COMPLETED", result.stdout + result.stderr
        )

    def test_explicit_version_is_embedded_identically_for_both_consumers(self) -> None:
        result = self.configure(
            "-DATRINIK_PACKAGE_VERSION=6.7.8",
            "-DCMAKE_BUILD_TYPE=Release",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        client, server = self.build_and_outputs()
        self.assertEqual(client, server)
        self.assertRegex(client, r"^6\.7\.8\|6\|7\|8\|Release\|.+\|.+\|.+\|unknown\|unknown$")

    def test_invalid_explicit_version_is_rejected(self) -> None:
        result = self.configure("-DATRINIK_PACKAGE_VERSION=6.7")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ATRINIK_PACKAGE_VERSION must be MAJOR.MINOR.PATCH", result.stderr)

    def test_packaged_version_file_is_the_offline_fallback(self) -> None:
        (self.source / "VERSION").write_text("7.8.9\n", encoding="utf-8")
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        client, server = self.build_and_outputs()
        self.assertTrue(client.startswith("7.8.9|7|8|9|"))
        self.assertEqual(client, server)

    def test_exact_tag_and_untagged_developer_fallbacks_are_deterministic(self) -> None:
        subprocess.run(["git", "init", "-q", "-b", "main", self.source], check=True)
        subprocess.run(
            ["git", "-C", str(self.source), "config", "user.email", "test@example.invalid"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(self.source), "config", "user.name", "Version Test"],
            check=True,
        )
        subprocess.run(["git", "-C", str(self.source), "add", "."], check=True)
        subprocess.run(
            ["git", "-C", str(self.source), "commit", "-q", "-m", "test: fixture"],
            check=True,
            env={
                **os.environ,
                "GIT_AUTHOR_DATE": "2020-01-02T03:04:05Z",
                "GIT_COMMITTER_DATE": "2020-01-02T03:04:05Z",
            },
        )
        subprocess.run(["git", "-C", str(self.source), "tag", "v8.9.10"], check=True)
        tagged = self.configure()
        self.assertEqual(tagged.returncode, 0, tagged.stderr)
        client, server = self.build_and_outputs()
        self.assertTrue(client.startswith("8.9.10|8|9|10|"))
        self.assertEqual(client, server)

        (self.root / "build" / "CMakeCache.txt").unlink()
        subprocess.run(["git", "-C", str(self.source), "tag", "-d", "v8.9.10"], check=True)
        untagged = self.configure()
        self.assertEqual(untagged.returncode, 0, untagged.stderr)
        client, server = self.build_and_outputs()
        self.assertTrue(client.startswith("5.1.0|5|1|0|"))
        self.assertEqual(client, server)

    def git(self, directory: Path, *arguments: str) -> str:
        return subprocess.check_output(
            ["git", "-C", str(directory), *arguments], text=True,
            env={**os.environ, "GIT_AUTHOR_NAME": "Fixture", "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
                 "GIT_COMMITTER_NAME": "Fixture", "GIT_COMMITTER_EMAIL": "fixture@example.invalid"},
        ).strip()

    def initialize_git(self, directory: Path) -> str:
        self.git(directory, "init", "-q", "-b", "main")
        self.git(directory, "add", ".")
        self.git(directory, "commit", "-qm", "test: owner fixture")
        return self.git(directory, "rev-parse", "HEAD")

    def test_archive_below_foreign_repository_does_not_inherit_metadata(self) -> None:
        foreign = self.initialize_git(self.root)
        self.git(self.root, "tag", "v88.99.100")
        (self.source / "VERSION").write_text("5.68.0\n")
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        client, server = self.build_and_outputs()
        self.assertEqual(client, server)
        self.assertTrue(client.startswith("5.68.0|5|68|0|"))
        self.assertTrue(client.endswith("|unknown|unknown"), client)
        self.assertNotIn(foreign, client)

    def test_untagged_source_below_foreign_tag_uses_development_version(self) -> None:
        self.initialize_git(self.root)
        self.git(self.root, "tag", "v88.99.100")
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        client, _ = self.build_and_outputs()
        self.assertTrue(client.startswith("5.1.0|"), client)
        self.assertTrue(client.endswith("|unknown|unknown"), client)

    def test_linked_worktree_and_dirty_identity_are_physical(self) -> None:
        revision = self.initialize_git(self.source)
        self.git(self.source, "tag", "v5.68.0")
        linked = self.root / "linked"
        self.git(self.source, "worktree", "add", "-b", "fixture", str(linked))
        self.source = linked
        self.assertTrue((linked / ".git").is_file())
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        client, _ = self.build_and_outputs()
        self.assertTrue(client.startswith("5.68.0|"), client)
        self.assertTrue(client.endswith("|" + revision + "|false"), client)
        (linked / "untracked.txt").write_text("dirty")
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        client, _ = self.build_and_outputs()
        self.assertTrue(client.endswith("|" + revision + "|true"), client)

    def test_explicit_identity_reconfigures_without_source_changes(self) -> None:
        for version, revision, dirty in (("5.68.0", "a" * 40, "false"), ("5.69.0", "b" * 40, "true")):
            result = self.configure("-DATRINIK_PACKAGE_VERSION=" + version,
                                    "-DATRINIK_SOURCE_REVISION=" + revision,
                                    "-DATRINIK_SOURCE_DIRTY=" + dirty)
            self.assertEqual(result.returncode, 0, result.stderr)
            client, server = self.build_and_outputs()
            self.assertEqual(client, server)
            self.assertTrue(client.startswith(version + "|"), client)
            self.assertTrue(client.endswith("|" + revision + "|" + dirty), client)

    def test_all_component_entrypoints_accept_explicit_owner_version(self) -> None:
        for component in ("client", "server", "protocol", "libatrinik"):
            with self.subTest(component=component):
                self.assert_component_version_probe(ROOT / component, self.root / (component + "-explicit"))

    def test_invalid_explicit_revision_and_dirty_state_are_rejected(self) -> None:
        for argument in ("-DATRINIK_SOURCE_REVISION=not-a-commit", "-DATRINIK_SOURCE_DIRTY=maybe"):
            with self.subTest(argument=argument):
                result = self.configure(argument)
                self.assertNotEqual(result.returncode, 0)
                if (self.root / "build/CMakeCache.txt").exists():
                    (self.root / "build/CMakeCache.txt").unlink()

    def test_git_selectors_cannot_redirect_owner_metadata(self) -> None:
        revision = self.initialize_git(self.source)
        self.git(self.source, "tag", "v5.68.0")
        foreign = self.root / "foreign"
        foreign.mkdir()
        (foreign / "README").write_text("foreign")
        self.initialize_git(foreign)
        self.git(foreign, "tag", "v88.99.100")
        from unittest import mock
        with mock.patch.dict(os.environ, {"GIT_DIR": str(foreign / ".git"), "GIT_WORK_TREE": str(self.source)}):
            result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        client, _ = self.build_and_outputs()
        self.assertTrue(client.startswith("5.68.0|"), client)
        self.assertTrue(client.endswith("|" + revision + "|false"), client)

    def test_real_protocol_unified_and_embedded_archive_versions(self) -> None:
        self.initialize_git(self.root)
        self.git(self.root, "tag", "v88.99.100")
        for layout in ("unified", "embedded", "standalone"):
            with self.subTest(layout=layout):
                package = self.root / layout
                package.mkdir()
                (package / "cmake").mkdir()
                shutil.copy2(MODULE, package / "cmake/AtrinikVersion.cmake")
                (package / "VERSION").write_text("5.68.0\n")
                if layout == "standalone":
                    protocol = package
                else:
                    protocol = package / ("protocol" if layout == "unified" else "dependencies/protocol")
                shutil.copytree(ROOT / "protocol", protocol, dirs_exist_ok=True,
                                ignore=shutil.ignore_patterns("build", "__pycache__"))
                if layout == "unified":
                    shutil.copy2(ROOT / "CMakeLists.txt", package / "CMakeLists.txt")
                    # Exercise actual root/protocol configuration and package-version
                    # generation without compiling unrelated gameplay dependencies.
                    for consumer in ("libatrinik", "client", "server"):
                        (package / consumer).mkdir()
                        (package / consumer / "CMakeLists.txt").write_text("")
                    configure_source = package
                else:
                    (protocol / "VERSION").write_text("5.68.0\n")
                    configure_source = protocol
                build = self.root / (layout + "-build")
                result = subprocess.run(["cmake", "-Werror=dev", "-S", str(configure_source), "-B", str(build)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                config = build / ("protocol" if layout == "unified" else "") / "AtrinikProtocolConfigVersion.cmake"
                self.assertIn('set(PACKAGE_VERSION "5.68.0")', config.read_text())

    def test_physical_component_and_symlink_views_use_owner_tag_without_override(self) -> None:
        owner = self.root / "classic-owner"
        (owner / "cmake").mkdir(parents=True)
        shutil.copy2(MODULE, owner / "cmake/AtrinikVersion.cmake")
        for component in ("client", "server", "protocol", "libatrinik"):
            (owner / component).mkdir()
            shutil.copy2(ROOT / component / "CMakeLists.txt", owner / component / "CMakeLists.txt")
        self.initialize_git(owner)
        self.git(owner, "tag", "v5.68.0")
        for component in ("client", "server", "protocol", "libatrinik"):
            for linked in (False, True):
                with self.subTest(component=component, linked=linked):
                    source = owner / component
                    label = component + ("-view" if linked else "-physical")
                    if linked:
                        source = self.root / label
                        source.mkdir()
                        (source / "CMakeLists.txt").symlink_to(owner / component / "CMakeLists.txt")
                    self.assert_component_version_probe(source, self.root / (label + "-build"),
                                                        version="5.68.0", explicit=False)

    def test_pathfinding_physical_tag_untagged_and_installed_package(self) -> None:
        owner = self.root / "classic-owner"
        (owner / "cmake").mkdir(parents=True)
        shutil.copy2(MODULE, owner / "cmake/AtrinikVersion.cmake")
        shutil.copytree(ROOT / "libatrinik/pathfinding", owner / "libatrinik/pathfinding")
        self.initialize_git(owner)
        self.git(owner, "tag", "v5.68.0")
        for tagged in (True, False):
            if not tagged:
                self.git(owner, "tag", "-d", "v5.68.0")
            version = "5.68.0" if tagged else "5.1.0"
            build = self.root / ("pathfinding-tagged" if tagged else "pathfinding-untagged")
            install = build / "install"
            result = subprocess.run([
                "cmake", "-S", str(owner / "libatrinik/pathfinding"), "-B", str(build),
                "-DCMAKE_INSTALL_PREFIX=" + str(install),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run(["cmake", "--build", str(build), "--parallel", "2"],
                           check=True, capture_output=True)
            subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure"],
                           check=True, capture_output=True)
            subprocess.run(["cmake", "--install", str(build)], check=True, capture_output=True)
            config = install / "lib/cmake/AtrinikPathfinding/AtrinikPathfindingConfigVersion.cmake"
            self.assertIn(f'set(PACKAGE_VERSION "{version}")', config.read_text())
            consumer = build / "consumer"
            consumer.mkdir()
            (consumer / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.21)\nproject(consumer LANGUAGES C)\n'
                f'find_package(AtrinikPathfinding {version} EXACT CONFIG REQUIRED)\n'
                'add_executable(consumer main.c)\ntarget_link_libraries(consumer PRIVATE Atrinik::Pathfinding)\n')
            (consumer / "main.c").write_text('int main(void) { return 0; }\n')
            subprocess.run(["cmake", "-S", str(consumer), "-B", str(consumer / "build"),
                            "-DCMAKE_PREFIX_PATH=" + str(install)], check=True, capture_output=True)
            subprocess.run(["cmake", "--build", str(consumer / "build")],
                           check=True, capture_output=True)

    def test_generated_archives_keep_owner_identity_and_pathfinding_package_version(self) -> None:
        owner = self.root / "owner"
        (owner / "cmake").mkdir(parents=True)
        shutil.copy2(MODULE, owner / "cmake/AtrinikVersion.cmake")
        shutil.copytree(ROOT / "protocol", owner / "protocol",
                        ignore=shutil.ignore_patterns("build", "__pycache__"))
        shutil.copytree(ROOT / "libatrinik/pathfinding", owner / "libatrinik/pathfinding")
        for component in ("client", "server"):
            (owner / component).mkdir()
            shutil.copy2(self.source / "main.c", owner / component / "main.c")
            cmake = (self.source / "CMakeLists.txt").read_text().replace(
                MODULE.as_posix(), '${CMAKE_CURRENT_LIST_DIR}/cmake/AtrinikVersion.cmake')
            (owner / component / "CMakeLists.txt").write_text(cmake)
        revision = self.initialize_git(owner)
        self.git(owner, "tag", "v5.68.0")
        source_tar = self.root / "source.tar"
        with source_tar.open("wb") as stream:
            subprocess.run(["git", "-C", str(owner), "archive", "HEAD"], check=True, stdout=stream)
        foreign = self.root / "foreign"
        foreign.mkdir()
        (foreign / "README").write_text("foreign")
        foreign_revision = self.initialize_git(foreign)
        self.git(foreign, "tag", "v88.99.100")
        for scope in ("root", "client", "server", "libatrinik"):
            with self.subTest(scope=scope):
                archive = self.root / (scope + ".tar.gz")
                package_sources.build_archive(source_tar, archive, scope, "5.68.0", 42, revision)
                destination = foreign / scope
                with tarfile.open(archive) as source:
                    source.extractall(destination, filter="data")
                package = next(destination.iterdir())
                pathfinding = package / ("libatrinik/pathfinding" if scope == "root" else
                                        "pathfinding" if scope == "libatrinik" else
                                        "dependencies/libatrinik/pathfinding")
                build = self.root / (scope + "-pathfinding-build")
                result = subprocess.run(["cmake", "-Werror=dev", "-S", str(pathfinding), "-B", str(build)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('set(PACKAGE_VERSION "5.68.0")',
                              (build / "AtrinikPathfindingConfigVersion.cmake").read_text())
                if scope in ("client", "server"):
                    self.source = package
                    result = self.configure()
                    self.assertEqual(result.returncode, 0, result.stderr)
                    client, server = self.build_and_outputs()
                    self.assertEqual(client, server)
                    self.assertTrue(client.startswith("5.68.0|"), client)
                    self.assertTrue(client.endswith("|" + revision + "|unknown"), client)
                    self.assertNotIn(foreign_revision, client)
                    shutil.rmtree(self.root / "build")

    def test_component_configuration_has_no_source_tree_version_header(self) -> None:
        for component in ("client", "server"):
            cmake = (ROOT / component / "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertNotIn("version.h.def", cmake)
            self.assertNotIn("src/include/version.h", cmake)
            self.assertFalse((ROOT / component / "src/include/version.h.def").exists())
        for path in (
            ROOT / "client/src/client/window_title.c",
            ROOT / "client/src/tests/window_title.c",
            ROOT / "server/src/include/includes.h",
        ):
            self.assertNotIn("#include <version.h>", path.read_text(encoding="utf-8"))

    def test_component_version_module_resolution_supports_symlinked_views(self) -> None:
        for component in ("client", "server"):
            source = ROOT / component
            view = self.root / f"{component}-view"
            view.mkdir()
            for entry in source.iterdir():
                (view / entry.name).symlink_to(
                    entry, target_is_directory=entry.is_dir()
                )
            self.assert_component_version_probe(
                view, self.root / f"{component}-build"
            )

    def test_component_version_module_resolution_supports_scoped_packages(self) -> None:
        for component in ("client", "server"):
            source = ROOT / component
            package = self.root / f"{component}-package"
            package.mkdir()
            for entry in source.iterdir():
                if entry.name in {"CMakeLists.txt", "cmake"}:
                    continue
                (package / entry.name).symlink_to(
                    entry, target_is_directory=entry.is_dir()
                )
            shutil.copy2(source / "CMakeLists.txt", package / "CMakeLists.txt")
            for document in ("LICENSE.md", "ATTRIBUTIONS.md"):
                shutil.copy2(ROOT / document, package / document)
            (package / "cmake").mkdir()
            for entry in (source / "cmake").iterdir():
                (package / "cmake" / entry.name).symlink_to(
                    entry, target_is_directory=entry.is_dir()
                )
            shutil.copy2(MODULE, package / "cmake" / MODULE.name)
            self.assert_component_version_probe(
                package, self.root / f"{component}-package-build"
            )


if __name__ == "__main__":
    unittest.main()
