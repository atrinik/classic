from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "package_sources.py"
SPEC = importlib.util.spec_from_file_location("package_sources", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
package_sources = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package_sources)


class PackageSourcesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        subprocess.run(["git", "init", "-q", "-b", "main", self.root], check=True)
        subprocess.run(
            ["git", "-C", self.root, "config", "user.email", "test@example.invalid"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", self.root, "config", "user.name", "Release Test"],
            check=True,
        )
        files = {
            "LICENSE.md": "license\n",
            "ATTRIBUTIONS.md": "notices\n",
            "cmake/AtrinikVersion.cmake": "# version contract\n",
            "docs/history/imports.json": "{}\n",
            "client/.releaserc.json": "{}\n",
            "client/main.c": "int main(void) { return 0; }\n",
            "server/main.c": "int main(void) { return 0; }\n",
            "protocol/CMakeLists.txt": "# protocol\n",
            "libatrinik/CMakeLists.txt": "# library\n",
        }
        for name, value in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value, encoding="utf-8")
        subprocess.run(["git", "-C", self.root, "add", "."], check=True)
        subprocess.run(
            ["git", "-C", self.root, "commit", "-q", "-m", "test: fixture"],
            check=True,
            env={
                "PATH": "/usr/bin:/bin",
                "GIT_AUTHOR_DATE": "2020-01-02T03:04:05Z",
                "GIT_COMMITTER_DATE": "2020-01-02T03:04:05Z",
            },
        )
        self.source = self.root / "repository.tar"
        with self.source.open("wb") as stream:
            subprocess.run(
                ["git", "-C", self.root, "archive", "--format=tar", "HEAD"],
                check=True,
                stdout=stream,
            )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_scoped_archive_contains_shared_evidence_but_not_other_modules(self) -> None:
        output = self.root / "client.tar.gz"
        package_sources.build_archive(
            self.source, output, "client", "5.6.0", 1_577_934_245
        )
        with tarfile.open(output, "r:gz") as archive:
            names = set(archive.getnames())
            version = archive.extractfile("atrinik-classic-client-5.6.0/VERSION")
            assert version is not None
            self.assertEqual(version.read(), b"5.6.0\n")
            protocol_version = archive.extractfile(
                "atrinik-classic-client-5.6.0/dependencies/protocol/VERSION"
            )
            assert protocol_version is not None
            self.assertEqual(protocol_version.read(), b"5.6.0\n")
            libatrinik_version = archive.extractfile(
                "atrinik-classic-client-5.6.0/dependencies/libatrinik/VERSION"
            )
            assert libatrinik_version is not None
            self.assertEqual(libatrinik_version.read(), b"5.6.0\n")
        self.assertIn("atrinik-classic-client-5.6.0/main.c", names)
        self.assertIn("atrinik-classic-client-5.6.0/LICENSE.md", names)
        self.assertIn(
            "atrinik-classic-client-5.6.0/cmake/AtrinikVersion.cmake", names
        )
        self.assertIn(
            "atrinik-classic-client-5.6.0/PROVENANCE/history/imports.json", names
        )
        self.assertNotIn("atrinik-classic-client-5.6.0/server/main.c", names)
        self.assertNotIn("atrinik-classic-client-5.6.0/.releaserc.json", names)
        self.assertIn(
            "atrinik-classic-client-5.6.0/dependencies/protocol/CMakeLists.txt",
            names,
        )
        self.assertIn(
            "atrinik-classic-client-5.6.0/dependencies/libatrinik/CMakeLists.txt",
            names,
        )

    def test_archive_is_reproducible_and_refuses_overwrite(self) -> None:
        first = self.root / "first.tar.gz"
        second = self.root / "second.tar.gz"
        package_sources.build_archive(self.source, first, "root", "5.6.0", 42)
        package_sources.build_archive(self.source, second, "root", "5.6.0", 42)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with self.assertRaisesRegex(package_sources.PackageError, "refusing to overwrite"):
            package_sources.build_archive(self.source, first, "root", "5.6.0", 42)

    def test_unsafe_source_path_is_rejected(self) -> None:
        with self.assertRaisesRegex(package_sources.PackageError, "unsafe archive path"):
            package_sources.selected_name("root", "../escape", "package")


if __name__ == "__main__":
    unittest.main()
