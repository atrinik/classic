from __future__ import annotations

import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
import zipfile
import sys
import warnings


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "finalize_artifacts.py"
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("finalize_artifacts", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
finalize_artifacts = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(finalize_artifacts)


class FinalizeArtifactsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def embedded_python_standard_library() -> bytes:
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as archive:
            archive.writestr("encodings/__init__.pyc", b"compiled encodings")
            archive.writestr("os.pyc", b"compiled os")
        return output.getvalue()

    def test_source_archive_requires_version_and_license(self) -> None:
        path = self.root / "atrinik-classic-client-5.6.0.tar.gz"
        package = "atrinik-classic-client-5.6.0"
        with tarfile.open(path, "w:gz") as archive:
            for name, content in (
                (f"{package}/VERSION", b"5.6.0\n"),
                (f"{package}/LICENSE.md", b"GPL\n"),
                (f"{package}/ATTRIBUTIONS.md", b"notices\n"),
                (f"{package}/PROVENANCE/history/imports.json", b"{}\n"),
                (f"{package}/PROVENANCE/history/release-tags.json", b"{}\n"),
                (
                    f"{package}/PROVENANCE/history/component-release-map.json",
                    b"{}\n",
                ),
                (f"{package}/dependencies/protocol/CMakeLists.txt", b"# protocol\n"),
                (f"{package}/dependencies/protocol/VERSION", b"5.6.0\n"),
                (
                    f"{package}/dependencies/libatrinik/CMakeLists.txt",
                    b"# library\n",
                ),
                (f"{package}/dependencies/libatrinik/VERSION", b"5.6.0\n"),
            ):
                member = tarfile.TarInfo(name)
                member.size = len(content)
                archive.addfile(member, io.BytesIO(content))
        finalize_artifacts.validate_source_archive(
            path, "atrinik-classic-client", "5.6.0"
        )

    def test_wheel_metadata_must_use_unified_name_and_version(self) -> None:
        path = self.root / "atrinik_classic_protocol-5.6.0-py3-none-any.whl"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(
                "atrinik_classic_protocol-5.6.0.dist-info/METADATA",
                "Name: atrinik-classic-protocol\n"
                "Version: 5.6.0\n"
                "License-Expression: GPL-2.0-or-later\n",
            )
            archive.writestr(
                "atrinik_classic_protocol-5.6.0.dist-info/licenses/LICENSE.md",
                "GPL\n",
            )
            archive.writestr("atrinik_protocol/__init__.py", "from .game import VALUE\n")
            archive.writestr("atrinik_protocol/game.py", "VALUE = 1\n")
            archive.writestr(
                "atrinik_classic_protocol-5.6.0.dist-info/WHEEL",
                "Wheel-Version: 1.0\n",
            )
            archive.writestr(
                "atrinik_classic_protocol-5.6.0.dist-info/RECORD",
                "atrinik_protocol/__init__.py,,\n",
            )
        finalize_artifacts.validate_wheel(path, "5.6.0")
        with self.assertRaisesRegex(RuntimeError, "wrong distribution version"):
            finalize_artifacts.validate_wheel(path, "5.6.1")

    def test_windows_zip_requires_portable_payload(self) -> None:
        path = self.root / "client.zip"
        package = "atrinik-classic-client-5.6.0-windows-x86_64"
        with zipfile.ZipFile(path, "w") as archive:
            for name in (
                "atrinik.exe",
                "LICENSE.md",
                "ATTRIBUTIONS.md",
                "client.cfg",
                "ca-bundle.crt",
                "SDL3.dll",
                "SDL3_image.dll",
                "SDL3_mixer.dll",
                "SDL3_ttf.dll",
                "fonts/font.ttf",
                "textures/icon.png",
                "data/items.dat",
                "settings/default.xml",
                "sound/click.ogg",
            ):
                archive.writestr(f"{package}/{name}", b"fixture")
        finalize_artifacts.validate_zip(
            path,
            package,
            (
                "atrinik.exe",
                "client.cfg",
                "ca-bundle.crt",
                "LICENSE.md",
                "ATTRIBUTIONS.md",
                "SDL3.dll",
                "SDL3_image.dll",
                "SDL3_mixer.dll",
                "SDL3_ttf.dll",
                "fonts/*",
                "textures/*",
                "data/*",
                "settings/*",
                "sound/*",
            ),
        )
        with self.assertRaisesRegex(RuntimeError, "missing packaged server.cfg"):
            finalize_artifacts.validate_zip(path, package, ("server.cfg",))

    def test_windows_zip_rejects_wrong_root_or_empty_required_payload(self) -> None:
        wrong_root = self.root / "wrong-root.zip"
        with zipfile.ZipFile(wrong_root, "w") as archive:
            archive.writestr("other/atrinik.exe", b"fixture")
        with self.assertRaisesRegex(RuntimeError, "unexpected root"):
            finalize_artifacts.validate_zip(wrong_root, "expected", ("atrinik.exe",))

        root_file = self.root / "root-file.zip"
        with zipfile.ZipFile(root_file, "w") as archive:
            archive.writestr("expected", b"fixture")
        with self.assertRaisesRegex(RuntimeError, "unexpected root"):
            finalize_artifacts.validate_zip(root_file, "expected", ())

        empty = self.root / "empty.zip"
        with zipfile.ZipFile(empty, "w") as archive:
            archive.writestr("expected/atrinik.exe", b"")
        with self.assertRaisesRegex(RuntimeError, "only empty"):
            finalize_artifacts.validate_zip(empty, "expected", ("atrinik.exe",))

    def test_windows_zip_rejects_corrupt_unsafe_or_duplicate_members(self) -> None:
        corrupt = self.root / "corrupt.zip"
        with zipfile.ZipFile(corrupt, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("expected/atrinik.exe", b"unique payload")
        contents = bytearray(corrupt.read_bytes())
        payload_offset = contents.index(b"unique payload")
        contents[payload_offset] ^= 1
        corrupt.write_bytes(contents)
        with self.assertRaisesRegex(RuntimeError, "corrupt ZIP member"):
            finalize_artifacts.validate_zip(
                corrupt, "expected", ("atrinik.exe",)
            )

        unsafe = self.root / "unsafe.zip"
        with zipfile.ZipFile(unsafe, "w") as archive:
            archive.writestr("expected\\..\\other\\payload", b"payload")
        with self.assertRaisesRegex(RuntimeError, "unsafe packaged path"):
            finalize_artifacts.validate_zip(unsafe, "expected", ("payload",))

        duplicate = self.root / "duplicate.zip"
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(duplicate, "w") as archive:
                archive.writestr("expected/payload", b"first")
                archive.writestr("expected/payload", b"second")
        with self.assertRaisesRegex(RuntimeError, "duplicate member"):
            finalize_artifacts.validate_zip(duplicate, "expected", ("payload",))

        case_collision = self.root / "case-collision.zip"
        with zipfile.ZipFile(case_collision, "w") as archive:
            archive.writestr("expected/server/server.cfg", b"first")
            archive.writestr("expected/server/SERVER.CFG", b"second")
        with self.assertRaisesRegex(RuntimeError, "duplicate packaged output"):
            finalize_artifacts.validate_zip(case_collision, "expected", ())

        for index, alias in enumerate(
            ("expected/server//server.cfg", "expected/server/./server.cfg")
        ):
            with self.subTest(alias=alias):
                unsafe_alias = self.root / f"unsafe-alias-{index}.zip"
                with zipfile.ZipFile(unsafe_alias, "w") as archive:
                    archive.writestr(alias, b"fixture")
                with self.assertRaisesRegex(RuntimeError, "unsafe packaged path"):
                    finalize_artifacts.validate_zip(unsafe_alias, "expected", ())

    def test_windows_server_zip_requires_runtime_payload(self) -> None:
        path = self.root / "server.zip"
        package = "atrinik-classic-server-5.6.0-windows-x86_64"
        files = (
            "server/atrinik-server.exe",
            "server/LICENSE.md",
            "server/ATTRIBUTIONS.md",
            "server/server.cfg",
            "server/permissions.cfg",
            "server/ca-bundle.crt",
            "server/server.bat",
            "server/LICENSE.txt",
            "server/plugin_arena.dll",
            "server/plugin_python.dll",
            "server/python3.dll",
            "server/python313.dll",
            "server/_socket.pyd",
            "server/maps/regions.reg",
            "server/lib/helper.dll",
            "server/resources/archetypes",
            "server/install_data/accounts",
            "server/assets/client-maps/map.json",
        )
        with zipfile.ZipFile(path, "w") as archive:
            for name in files:
                archive.writestr(f"{package}/{name}", b"fixture")
            archive.writestr(
                f"{package}/server/python313.zip",
                self.embedded_python_standard_library(),
            )
            archive.writestr(
                f"{package}/server/python313._pth",
                "python313.zip\n.\n",
            )
        finalize_artifacts.validate_zip(
            path,
            package,
            finalize_artifacts.SERVER_WINDOWS_REQUIRED_PATTERNS,
            finalize_artifacts.SERVER_WINDOWS_FORBIDDEN_PATTERNS,
        )
        finalize_artifacts.validate_embedded_python_runtime(path, package)

    def test_windows_server_zip_rejects_split_maps_layout(self) -> None:
        package = "atrinik-classic-server-5.6.0-windows-x86_64"
        for name in ("maps", "maps/", "maps/regions.reg", "Maps/", "MAPS/regions.reg"):
            with self.subTest(name=name):
                path = self.root / f"server-{name.replace('/', '-')}.zip"
                with zipfile.ZipFile(path, "w") as archive:
                    archive.writestr(f"{package}/{name}", b"fixture")
                with self.assertRaisesRegex(RuntimeError, "forbidden packaged maps"):
                    finalize_artifacts.validate_zip(
                        path,
                        package,
                        (),
                        finalize_artifacts.SERVER_WINDOWS_FORBIDDEN_PATTERNS,
                    )

    def test_embedded_python_requires_nonempty_standard_library_zip(self) -> None:
        path = self.root / "server.zip"
        package = "atrinik-classic-server-5.6.0-windows-x86_64"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(f"{package}/server/python313.dll", b"runtime")
            archive.writestr(
                f"{package}/server/python313._pth",
                "python313.zip\n.\n",
            )
        with self.assertRaisesRegex(RuntimeError, "exactly one embedded Python"):
            finalize_artifacts.validate_embedded_python_runtime(path, package)

        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(f"{package}/server/python313.dll", b"runtime")
            archive.writestr(
                f"{package}/server/python313._pth",
                "python313.zip\n.\n",
            )
            archive.writestr(f"{package}/server/python313.zip", b"")
        with self.assertRaisesRegex(
            RuntimeError, "empty embedded Python standard-library ZIP"
        ):
            finalize_artifacts.validate_embedded_python_runtime(path, package)

    def test_embedded_python_requires_matching_pth_reference(self) -> None:
        path = self.root / "server.zip"
        package = "atrinik-classic-server-5.6.0-windows-x86_64"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(f"{package}/server/python313.dll", b"runtime")
            archive.writestr(
                f"{package}/server/python312._pth",
                "python313.zip\n.\n",
            )
            archive.writestr(
                f"{package}/server/python313.zip",
                self.embedded_python_standard_library(),
            )
        with self.assertRaisesRegex(RuntimeError, "server/python313._pth"):
            finalize_artifacts.validate_embedded_python_runtime(path, package)

        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(f"{package}/server/python313.dll", b"runtime")
            archive.writestr(
                f"{package}/server/python313._pth",
                "python312.zip\n.\n",
            )
            archive.writestr(
                f"{package}/server/python313.zip",
                self.embedded_python_standard_library(),
            )
        with self.assertRaisesRegex(RuntimeError, "python313.zip followed by"):
            finalize_artifacts.validate_embedded_python_runtime(path, package)

        for entries in (
            "python313.zip\n",
            "python313.zip\n.\n..\\shared\n",
            "python313.zip\n.\nimport site\n",
        ):
            with self.subTest(entries=entries):
                with zipfile.ZipFile(path, "w") as archive:
                    archive.writestr(
                        f"{package}/server/python313.dll",
                        b"runtime",
                    )
                    archive.writestr(
                        f"{package}/server/python313._pth",
                        entries,
                    )
                    archive.writestr(
                        f"{package}/server/python313.zip",
                        self.embedded_python_standard_library(),
                    )
                with self.assertRaisesRegex(RuntimeError, "must contain only"):
                    finalize_artifacts.validate_embedded_python_runtime(
                        path, package
                    )

    def test_embedded_python_validates_matching_abi_and_nested_zip(self) -> None:
        path = self.root / "server.zip"
        package = "atrinik-classic-server-5.6.0-windows-x86_64"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(f"{package}/server/python312.dll", b"runtime")
            archive.writestr(
                f"{package}/server/python313._pth",
                "python313.zip\n.\n",
            )
            archive.writestr(
                f"{package}/server/python313.zip",
                self.embedded_python_standard_library(),
            )
        with self.assertRaisesRegex(RuntimeError, "server/python313.dll"):
            finalize_artifacts.validate_embedded_python_runtime(path, package)

        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(f"{package}/server/python313.dll", b"runtime")
            archive.writestr(
                f"{package}/server/python313._pth",
                "python313.zip\n.\n",
            )
            archive.writestr(f"{package}/server/python313.zip", b"not a ZIP")
        with self.assertRaisesRegex(
            RuntimeError, "invalid embedded Python standard-library ZIP"
        ):
            finalize_artifacts.validate_embedded_python_runtime(path, package)

    def test_embedded_python_requires_standard_library_members(self) -> None:
        path = self.root / "server.zip"
        package = "atrinik-classic-server-5.6.0-windows-x86_64"
        standard_library = io.BytesIO()
        with zipfile.ZipFile(standard_library, "w") as archive:
            archive.writestr("os.pyc", b"compiled os")
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(f"{package}/server/python313.dll", b"runtime")
            archive.writestr(
                f"{package}/server/python313._pth",
                "python313.zip\n.\n",
            )
            archive.writestr(
                f"{package}/server/python313.zip",
                standard_library.getvalue(),
            )
        with self.assertRaisesRegex(RuntimeError, "encodings/__init__.pyc"):
            finalize_artifacts.validate_embedded_python_runtime(path, package)

    def test_embedded_python_rejects_unversioned_runtime_stem(self) -> None:
        path = self.root / "server.zip"
        package = "atrinik-classic-server-5.6.0-windows-x86_64"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(f"{package}/server/python3.dll", b"runtime")
            archive.writestr(
                f"{package}/server/python3._pth",
                "python3.zip\n.\n",
            )
            archive.writestr(
                f"{package}/server/python3.zip",
                self.embedded_python_standard_library(),
            )
        with self.assertRaisesRegex(RuntimeError, "exactly one embedded Python"):
            finalize_artifacts.validate_embedded_python_runtime(path, package)

    def test_embedded_python_requires_root_extension_module(self) -> None:
        path = self.root / "server.zip"
        package = "atrinik-classic-server-5.6.0-windows-x86_64"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(f"{package}/server/python313.dll", b"runtime")
            archive.writestr(
                f"{package}/server/python313._pth",
                "python313.zip\n.\n",
            )
            archive.writestr(
                f"{package}/server/python313.zip",
                self.embedded_python_standard_library(),
            )
            archive.writestr(
                f"{package}/server/extensions/_socket.pyd",
                b"extension",
            )
        with self.assertRaisesRegex(RuntimeError, "no embedded Python extension"):
            finalize_artifacts.validate_embedded_python_runtime(path, package)

    def test_spdx_maps_locked_inputs_to_affected_artifacts(self) -> None:
        artifact = self.root / "atrinik-classic-server-5.6.0-windows-x86_64.zip"
        artifact.write_bytes(b"server")
        locked_input = {
            "name": "content",
            "repository": "atrinik/content",
            "tag": "v1.2.0",
            "commit": "a" * 40,
            "url": "https://github.com/atrinik/content/release.tar.gz",
            "sha256": "b" * 64,
            "lock": "server/dependencies.lock.json",
            "affects": [artifact.name, "ghcr.io/atrinik/classic-server:5.6.0"],
        }
        spdx = finalize_artifacts.build_spdx(
            [artifact], "5.6.0", "c" * 40, 0, [locked_input]
        )
        self.assertTrue(
            any(
                relationship.get("relationshipType") == "DEPENDS_ON"
                for relationship in spdx["relationships"]
            )
        )

    def test_expected_set_uses_unambiguous_classic_names(self) -> None:
        names = finalize_artifacts.expected_names("5.6.0")
        self.assertIn("atrinik-classic-5.6.0.tar.gz", names)
        self.assertIn("atrinik-classic-editor-5.6.0.tar.gz", names)
        self.assertIn(
            "atrinik-classic-server-5.6.0-windows-x86_64.zip", names
        )
        self.assertNotIn("atrinik-server-5.6.0.tar.gz", names)


if __name__ == "__main__":
    unittest.main()
