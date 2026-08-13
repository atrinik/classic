from __future__ import annotations

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
import urllib.error


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
            "repository": "atrinik/sound",
            "tag": "v1.0.0",
            "commit": "1" * 40,
            "url": archive.as_uri(),
            "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "destination": "sound",
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
        self.assertEqual((self.root / "sound/effects/test.ogg").read_bytes(), b"sound")
        dependencies.verify_dependency(self.root, dependency)
        self.assertEqual(
            dependencies.install_dependency(self.root, self.root / "build/cache", dependency),
            "current",
        )

    def test_refuses_unmanaged_destination(self) -> None:
        archive = self.make_archive([("sound-v1.0.0/test.ogg", b"sound", "file")])
        (self.root / "sound").mkdir()
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

    def test_retries_transient_http_failure_and_removes_partial(self) -> None:
        archive = self.make_archive([("sound-v1.0.0/test", b"ok", "file")])
        dependency = self.dependency(archive)
        body = archive.read_bytes()

        class Response:
            headers = {"Content-Length": str(len(body))}

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def geturl(self):
                return dependency["url"]

            def read(self, _size):
                nonlocal body
                result, body = body, b""
                return result

        attempts = []
        failures = []

        def opener(_request, timeout):
            attempts.append(timeout)
            if len(attempts) == 1:
                failure = urllib.error.HTTPError(
                    str(dependency["url"]), 503, "unavailable", {"Retry-After": "0"}, None
                )
                failures.append(failure)
                raise failure
            return Response()

        sleeps = []
        downloaded = dependencies._download(
            dependency, self.root / "cache", opener=opener,
            sleeper=sleeps.append, jitter=lambda _low, _high: 1.0,
        )
        self.assertEqual(downloaded.read_bytes(), archive.read_bytes())
        self.assertEqual(attempts, [60, 60])
        self.assertEqual(sleeps, [0.0])
        self.assertEqual(list((self.root / "cache").glob("*.part-*")), [])
        for failure in failures:
            failure.close()

    def test_retries_connection_reset_until_the_limit(self) -> None:
        archive = self.make_archive([("sound-v1.0.0/test", b"ok", "file")])
        dependency = self.dependency(archive)
        attempts = []

        def opener(_request, timeout):
            attempts.append(timeout)
            raise ConnectionResetError("reset")

        with self.assertRaisesRegex(dependencies.DependencyError, "retry limit exhausted"):
            dependencies._download(
                dependency, self.root / "cache", opener=opener,
                sleeper=lambda _delay: None, jitter=lambda _low, _high: 1.0,
            )
        self.assertEqual(attempts, [60] * dependencies.DOWNLOAD_ATTEMPTS)
        self.assertEqual(list((self.root / "cache").glob("*.part-*")), [])

    def test_replaces_corrupt_cached_archive(self) -> None:
        archive = self.make_archive([("sound-v1.0.0/test", b"ok", "file")])
        dependency = self.dependency(archive)
        cache = self.root / "cache"
        cache.mkdir()
        cached = cache / f"sound-{dependency['sha256']}.tar.gz"
        cached.write_bytes(b"corrupt")
        downloaded = dependencies._download(dependency, cache)
        self.assertEqual(downloaded.read_bytes(), archive.read_bytes())

    def test_does_not_retry_digest_failure_or_keep_partial(self) -> None:
        archive = self.make_archive([("sound-v1.0.0/test", b"ok", "file")])
        dependency = self.dependency(archive)
        dependency["sha256"] = "0" * 64
        attempts = []

        class Response(io.BytesIO):
            headers = {"Content-Length": str(archive.stat().st_size)}

            def geturl(self):
                return dependency["url"]

        def opener(_request, timeout):
            attempts.append(True)
            self.assertEqual(timeout, 60)
            return Response(archive.read_bytes())

        with self.assertRaisesRegex(dependencies.DependencyError, "terminal policy or integrity"):
            dependencies._download(
                dependency, self.root / "cache", opener=opener, sleeper=lambda _delay: None
            )
        self.assertEqual(attempts, [True])
        self.assertEqual(list((self.root / "cache").glob("*.part-*")), [])

    def test_source_cache_verifies_tree_before_reuse(self) -> None:
        archive = self.make_archive([("source/CMakeLists.txt", b"project(source)\n", "file")])
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        tree = hashlib.sha256(
            f"{hashlib.sha256(b'project(source)\n').hexdigest()}  CMakeLists.txt\n".encode()
        ).hexdigest()
        source = dependencies.fetch_source(
            name="fixture", url=archive.as_uri(), sha256=digest,
            tree_sha256=tree, cache_dir=self.root / "cache",
        )
        self.assertEqual((source / "CMakeLists.txt").read_text(encoding="utf-8"), "project(source)\n")
        (source / "CMakeLists.txt").write_text("changed\n", encoding="utf-8")
        with self.assertRaisesRegex(dependencies.DependencyError, "mismatched shared source content"):
            dependencies.fetch_source(
                name="fixture", url=archive.as_uri(), sha256=digest,
                tree_sha256=tree, cache_dir=self.root / "cache",
            )


if __name__ == "__main__":
    unittest.main()
