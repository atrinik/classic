from __future__ import annotations

import hashlib
import io
import re
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class CMakeIncrementalContractTests(unittest.TestCase):
    def test_libatrinik_header_manifest_is_complete(self) -> None:
        cmake = (ROOT / "libatrinik/CMakeLists.txt").read_text(encoding="utf-8")
        match = re.search(
            r"set\(LIBATRINIK_PUBLIC_HEADER_NAMES\n(?P<public>.*?)\)\n"
            r"set\(LIBATRINIK_PRIVATE_HEADER_NAMES (?P<private>.*?)\)",
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        assert match is not None
        declared = set(match.group("public").split()) | set(match.group("private").split())
        actual = {path.name for path in (ROOT / "libatrinik").glob("*.h")}
        self.assertEqual(declared, actual)
        self.assertEqual(set(match.group("private").split()), {"socket_private.h"})

    def test_immutable_source_cache_reuses_verified_extraction(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            archive = temporary / "fixture.tar.gz"
            payload = b"cmake_minimum_required(VERSION 3.21)\n"
            with tarfile.open(archive, "w:gz") as bundle:
                info = tarfile.TarInfo("fixture/CMakeLists.txt")
                info.size = len(payload)
                bundle.addfile(info, io.BytesIO(payload))
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            file_digest = hashlib.sha256(payload).hexdigest()
            tree_digest = hashlib.sha256(
                f"{file_digest}  CMakeLists.txt\n".encode()
            ).hexdigest()
            source = temporary / "source"
            cache = temporary / "cache"
            source.mkdir()
            helper = (ROOT / "server/cmake/immutable_source_cache.cmake").as_posix()
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.21)\n"
                "project(cache-test LANGUAGES NONE)\n"
                f'include("{helper}")\n'
                "atrinik_extract_immutable_source(\n"
                "  NAME fixture\n"
                f'  URL "file://{archive.as_posix()}"\n'
                f'  SHA256 "{digest}"\n'
                f'  TREE_SHA256 "{tree_digest}"\n'
                f'  CACHE_DIR "{cache.as_posix()}"\n'
                "  OUTPUT fixture_source)\n"
                "file(WRITE \"${CMAKE_BINARY_DIR}/source.txt\" \"${fixture_source}\")\n",
                encoding="utf-8",
            )

            for name in ("first", "second"):
                configured = subprocess.run(
                    ["cmake", "-S", str(source), "-B", str(temporary / name)],
                    check=False,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
                self.assertEqual(configured.returncode, 0, configured.stdout)
            first = (temporary / "first/source.txt").read_text(encoding="utf-8")
            second = (temporary / "second/source.txt").read_text(encoding="utf-8")
            self.assertEqual(first, second)
            self.assertEqual(Path(first).name, f"fixture-{digest}")

            cached_cmake = Path(first) / "CMakeLists.txt"
            cached_cmake.chmod(0o600)
            cached_cmake.write_bytes(payload + b"# changed\n")
            rejected = subprocess.run(
                ["cmake", "-S", str(source), "-B", str(temporary / "third")],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("Mismatched shared fixture source content", rejected.stdout)
            cached_cmake.write_bytes(payload)

            marker = Path(first) / ".atrinik-source-sha256"
            marker.chmod(0o600)
            marker.write_text("0" * 64 + "\n", encoding="utf-8")
            rejected = subprocess.run(
                ["cmake", "-S", str(source), "-B", str(temporary / "fourth")],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("Mismatched shared fixture source cache", rejected.stdout)


if __name__ == "__main__":
    unittest.main()
