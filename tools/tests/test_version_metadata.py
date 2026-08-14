from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
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
    printf("%s|%d|%d|%d|%s|%s|%s|%s|%s\\n",
           PACKAGE_VERSION,
           PACKAGE_VERSION_MAJOR,
           PACKAGE_VERSION_MINOR,
           PACKAGE_VERSION_PATCH,
           ATRINIK_BUILD_TYPE,
           ATRINIK_COMPILER_ID,
           ATRINIK_COMPILER_VERSION,
           ATRINIK_SYSTEM_NAME,
           ATRINIK_BENCHMARK_REVISION);
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
            ["cmake", "-S", str(self.source), "-B", str(self.root / "build"), *arguments],
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

    def assert_component_version_probe(self, source: Path, build: Path) -> None:
        probe = self.root / "component-version-probe.cmake"
        probe.write_text(
            """
if (NOT PROJECT_VERSION STREQUAL "9.8.7")
    message(FATAL_ERROR "Unexpected project version: ${PROJECT_VERSION}")
endif ()
message(FATAL_ERROR "ATRINIK_VERSION_PROBE_COMPLETED")
""".lstrip(),
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                "cmake",
                "-S",
                str(source),
                "-B",
                str(build),
                "-DATRINIK_PACKAGE_VERSION=9.8.7",
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
        self.assertRegex(client, r"^6\.7\.8\|6\|7\|8\|Release\|.+\|.+\|.+\|unknown$")

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

    def test_component_configuration_has_no_source_tree_version_header(self) -> None:
        for component in ("client", "server"):
            cmake = (ROOT / component / "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertNotIn("version.h.def", cmake)
            self.assertNotIn("src/include/version.h", cmake)
            self.assertFalse((ROOT / component / "src/include/version.h.def").exists())
        for path in (
            ROOT / "client/src/include/global.h",
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
