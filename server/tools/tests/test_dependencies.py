from __future__ import annotations

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "dependencies.py"
SPEC = importlib.util.spec_from_file_location("atrinik_dependencies", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
dependencies = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dependencies)


class DependencyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "client").mkdir()
        (self.root / "build").mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def make_archive(self, members: list[tuple[str, bytes, str]]) -> Path:
        path = self.root / "asset.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for name, contents, kind in members:
                info = tarfile.TarInfo(name)
                if kind == "file":
                    info.size = len(contents)
                    archive.addfile(info, io.BytesIO(contents))
                elif kind == "symlink":
                    info.type = tarfile.SYMTYPE
                    info.linkname = contents.decode()
                    archive.addfile(info)
                else:
                    raise AssertionError(kind)
        return path

    def dependency(self, archive: Path) -> dict[str, object]:
        return {
            "name": "sound",
            "repository": "atrinik/atrinik-sound",
            "tag": "v1.0.0",
            "commit": "1" * 40,
            "url": archive.as_uri(),
            "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "destination": "client/sound",
            "strip_components": 1,
        }

    def test_installs_and_verifies_pinned_archive(self) -> None:
        archive = self.make_archive([("sound-v1.0.0/effects/test.ogg", b"sound", "file")])
        dependency = self.dependency(archive)
        status = dependencies.install_dependency(
            self.root,
            self.root / "build/cache",
            dependency,
        )
        self.assertEqual(status, "installed")
        self.assertEqual((self.root / "client/sound/effects/test.ogg").read_bytes(), b"sound")
        dependencies.verify_dependency(self.root, dependency)
        self.assertEqual(
            dependencies.install_dependency(self.root, self.root / "build/cache", dependency),
            "current",
        )

    def test_refuses_unmanaged_destination(self) -> None:
        archive = self.make_archive([("sound-v1.0.0/test.ogg", b"sound", "file")])
        (self.root / "client/sound").mkdir()
        with self.assertRaisesRegex(dependencies.DependencyError, "unmanaged"):
            dependencies.install_dependency(
                self.root,
                self.root / "build/cache",
                self.dependency(archive),
            )

    def test_rejects_parent_traversal(self) -> None:
        archive = self.make_archive([("sound-v1.0.0/../../escape", b"bad", "file")])
        staging = self.root / "staging"
        staging.mkdir()
        with self.assertRaisesRegex(dependencies.DependencyError, "unsafe archive member"):
            dependencies.extract_archive(archive, staging, 1)

    def test_rejects_symbolic_links(self) -> None:
        archive = self.make_archive([("sound-v1.0.0/link", b"../../escape", "symlink")])
        staging = self.root / "staging"
        staging.mkdir()
        with self.assertRaisesRegex(dependencies.DependencyError, "unsupported archive member"):
            dependencies.extract_archive(archive, staging, 1)

    def test_rejects_windows_path_separators(self) -> None:
        archive = self.make_archive([("sound-v1.0.0\\..\\escape", b"bad", "file")])
        staging = self.root / "staging"
        staging.mkdir()
        with self.assertRaisesRegex(dependencies.DependencyError, "unsafe archive member"):
            dependencies.extract_archive(archive, staging, 1)

    def test_rejects_case_colliding_paths(self) -> None:
        archive = self.make_archive(
            [
                ("sound-v1.0.0/A.ogg", b"a", "file"),
                ("sound-v1.0.0/a.ogg", b"b", "file"),
            ]
        )
        staging = self.root / "staging"
        staging.mkdir()
        with self.assertRaisesRegex(dependencies.DependencyError, "duplicate archive output"):
            dependencies.extract_archive(archive, staging, 1)

    def test_lock_rejects_duplicate_keys(self) -> None:
        lock = self.root / "lock.json"
        lock.write_text('{"schema_version": 1, "schema_version": 1, "dependencies": []}')
        with self.assertRaisesRegex(dependencies.DependencyError, "duplicate JSON key"):
            dependencies.load_lock(lock, allow_file_urls=True)

    def test_loads_strict_lock(self) -> None:
        archive = self.make_archive([("sound-v1.0.0/test", b"ok", "file")])
        lock = self.root / "lock.json"
        lock.write_text(
            json.dumps({"schema_version": 1, "dependencies": [self.dependency(archive)]})
        )
        loaded = dependencies.load_lock(lock, allow_file_urls=True)
        self.assertEqual(loaded[0]["name"], "sound")


if __name__ == "__main__":
    unittest.main()
