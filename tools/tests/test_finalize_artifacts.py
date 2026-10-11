from __future__ import annotations

import importlib.util
import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile
import sys
import stat
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

        symlink = self.root / "symlink.zip"
        with zipfile.ZipFile(symlink, "w") as archive:
            member = zipfile.ZipInfo("expected/server/maps/regions.reg")
            member.create_system = 3
            member.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(member, b"../../outside")
        with self.assertRaisesRegex(RuntimeError, "unsupported ZIP member type"):
            finalize_artifacts.validate_zip(
                symlink, "expected", ("server/maps/regions.reg",)
            )

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

        for index, alias in enumerate(
            (
                "expected/maps./regions.reg",
                "expected/server/server.cfg.",
                "expected/server/server.cfg ",
                "expected/server/ server.cfg",
                "expected/server/CON.txt",
                "expected/server/COM¹.txt",
                "expected/server/LPT²",
                "expected/server/bad:name",
                "expected/server/control\x1f.txt",
            )
        ):
            with self.subTest(alias=alias):
                windows_alias = self.root / f"windows-alias-{index}.zip"
                with zipfile.ZipFile(windows_alias, "w") as archive:
                    archive.writestr(alias, b"fixture")
                with self.assertRaisesRegex(RuntimeError, "unsafe packaged path"):
                    finalize_artifacts.validate_zip(windows_alias, "expected", ())

        finalize_artifacts.validate_windows_member("a" * 255)
        with self.assertRaisesRegex(RuntimeError, "unsafe packaged path"):
            finalize_artifacts.validate_windows_member("a" * 256)
        with self.assertRaisesRegex(RuntimeError, "unsafe packaged path"):
            finalize_artifacts.validate_windows_member("\U0001f600" * 128)

        collisions = (
            ("expected/server", "expected/server/maps/regions.reg"),
            ("expected/server/maps/regions.reg", "expected/server"),
            ("expected/SERVER", "expected/server/maps/regions.reg"),
        )
        for index, members in enumerate(collisions):
            with self.subTest(members=members):
                collision = self.root / f"file-descendant-{index}.zip"
                with zipfile.ZipFile(collision, "w") as archive:
                    for member in members:
                        archive.writestr(member, b"fixture")
                with self.assertRaisesRegex(RuntimeError, "file/descendant collision"):
                    finalize_artifacts.validate_zip(collision, "expected", ())

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
            finalize_artifacts.SERVER_WINDOWS_UNIQUE_FILES,
        )
        finalize_artifacts.validate_embedded_python_runtime(path, package)

        duplicate_plugins = self.root / "duplicate-plugins.zip"
        with zipfile.ZipFile(duplicate_plugins, "w") as archive:
            archive.writestr(f"{package}/server/plugin_arena.dll", b"first")
            archive.writestr(f"{package}/server/OTHER_PLUGIN_ARENA.DLL", b"second")
            archive.writestr(f"{package}/server/plugin_python.dll", b"python")
        with self.assertRaisesRegex(RuntimeError, "exactly one packaged"):
            finalize_artifacts.validate_zip(
                duplicate_plugins,
                package,
                (),
                (),
                finalize_artifacts.SERVER_WINDOWS_UNIQUE_FILES,
            )

        overlapping_plugin = self.root / "overlapping-plugin.zip"
        with zipfile.ZipFile(overlapping_plugin, "w") as archive:
            archive.writestr(
                f"{package}/server/plugin_arena_plugin_python.dll", b"both"
            )
        with self.assertRaisesRegex(RuntimeError, "multiple unique roles"):
            finalize_artifacts.validate_zip(
                overlapping_plugin,
                package,
                (),
                (),
                finalize_artifacts.SERVER_WINDOWS_UNIQUE_FILES,
            )

        nested_plugins = self.root / "nested-plugins.zip"
        with zipfile.ZipFile(nested_plugins, "w") as archive:
            archive.writestr(f"{package}/server/nested/plugin_arena.dll", b"arena")
            archive.writestr(f"{package}/server/plugin_python.dll", b"python")
        with self.assertRaisesRegex(RuntimeError, "exactly one packaged"):
            finalize_artifacts.validate_zip(
                nested_plugins,
                package,
                (),
                (),
                finalize_artifacts.SERVER_WINDOWS_UNIQUE_FILES,
            )

        empty_root_plugin = self.root / "empty-root-plugin.zip"
        with zipfile.ZipFile(empty_root_plugin, "w") as archive:
            archive.writestr(f"{package}/server/plugin_arena.dll", b"")
            archive.writestr(
                f"{package}/server/nested/plugin_arena_payload.dll", b"nested"
            )
            archive.writestr(f"{package}/server/plugin_python.dll", b"python")
        with self.assertRaisesRegex(RuntimeError, "empty packaged"):
            finalize_artifacts.validate_zip(
                empty_root_plugin,
                package,
                ("server/*plugin_arena*.dll", "server/*plugin_python*.dll"),
                (),
                finalize_artifacts.SERVER_WINDOWS_UNIQUE_FILES,
            )

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

    def test_spdx_records_runtime_and_bundled_library_provenance(self) -> None:
        image = self.root / "atrinik-classic-client-5.6.0-linux-x86_64.AppImage"
        image.write_bytes(b"image")
        source = {"name": "SDL3", "version": "3.4.0", "url": "https://example.invalid/SDL3.tar.gz",
                  "sha256": "a" * 64, "license": "Zlib"}
        runtime = {"version": "2026-01", "url": "https://example.invalid/runtime",
                   "sha256": "b" * 64, "license": "MIT"}
        library = {"name": "libSDL3.so.0", "path": "usr/lib/libSDL3.so.0.4.0",
                   "version": "3.4.0", "sha256": "d" * 64, "license": "Zlib", "source": "SDL3"}
        inventory = {"lock_sha256": "e" * 64,
                     "native_inputs": {"sources": [source], "tools": [], "runtime": runtime},
                     "bundled_library_records": [library]}
        spdx = finalize_artifacts.build_spdx([image], "5.6.0", "c" * 40, 123, [], inventory)
        packages = {record["name"]: record for record in spdx["packages"]}
        for name, version, digest, license in (("SDL3", "3.4.0", "a" * 64, "Zlib"),
                                              ("appimage-runtime", "2026-01", "b" * 64, "MIT"),
                                              ("libSDL3.so.0", "3.4.0", "d" * 64, "Zlib")):
            self.assertEqual(packages[name]["versionInfo"], version)
            self.assertEqual(packages[name]["checksums"], [{"algorithm": "SHA256", "checksumValue": digest}])
            self.assertEqual(packages[name]["licenseDeclared"], license)
        self.assertIn({"spdxElementId": "SPDXRef-Artifact-1", "relationshipType": "CONTAINS",
                       "relatedSpdxElement": "SPDXRef-AppImageLibrary-1"}, spdx["relationships"])
        self.assertIn({"spdxElementId": "SPDXRef-AppImageLibrary-1", "relationshipType": "GENERATED_FROM",
                       "relatedSpdxElement": "SPDXRef-AppImageInput-SDL3"}, spdx["relationships"])

    def test_spdx_preserves_exact_package_scoped_notices_without_binary_license_inference(self) -> None:
        import copy
        image = self.root / "atrinik-classic-client-5.6.0-linux-x86_64.AppImage"
        image.write_bytes(b"image")
        notice_path = "usr/share/doc/atrinik/licenses/system/libexample/copyright"
        common_path = "usr/share/doc/atrinik/licenses/system/common-licenses/MIT"
        text = f"===== {notice_path} =====\nFull package notice with per-file scopes.\n\n===== {common_path} =====\nFull referenced license text.\n\n"
        notice = {"package": "libexample", "license_id": "LicenseRef-System-libexample",
                  "text": text, "sha256": hashlib.sha256(text.encode()).hexdigest(),
                  "files": [{"path": notice_path, "sha256": "a" * 64},
                            {"path": common_path, "sha256": "b" * 64}]}
        inventory = {"lock_sha256": "c" * 64, "native_inputs": {"sources": [], "tools": [],
                     "runtime": {"version": "1", "url": "https://example.invalid/runtime",
                                 "sha256": "d" * 64, "license": "MIT"}},
                     "bundled_library_records": [{"name": "libexample.so.1", "path": "usr/lib/libexample.so.1.0",
                         "version": "1.0", "sha256": "e" * 64, "license": notice["license_id"],
                         "source": "deb:libexample"}], "system_license_notices": [notice],
                     "files": {notice_path: "a" * 64, common_path: "b" * 64}}
        lock = {"system_package_licenses": {"libexample": notice["license_id"]},
                "system_package_copyright_sha256": {"libexample": "a" * 64},
                "system_package_notice_files": {"libexample": ["MIT"]},
                "system_common_license_sha256": {"MIT": "b" * 64}}
        document = finalize_artifacts.build_spdx([image], "5.6.0", "f" * 40, 123, [], inventory, lock)
        self.assertEqual(len(document["hasExtractedLicensingInfos"]), 1)
        extracted = document["hasExtractedLicensingInfos"][0]
        self.assertEqual(extracted["licenseId"], notice["license_id"])
        self.assertEqual(extracted["extractedText"], text)
        self.assertIn(notice["sha256"], extracted["comment"])
        binary = next(package for package in document["packages"] if package["name"] == "libexample.so.1")
        self.assertEqual(binary["licenseDeclared"], "NOASSERTION")
        self.assertEqual(binary["licenseConcluded"], "NOASSERTION")
        self.assertFalse(binary["filesAnalyzed"])
        self.assertNotIn("licenseInfoFromFiles", binary)
        self.assertIn(notice["license_id"], binary["comment"])
        self.assertIn("beyond this compiled library", binary["licenseComments"])
        for mutation, message in (("missing", "no exact source package notice"),
                                  ("duplicate", "duplicate"), ("text", "text hash"),
                                  ("file", "file hash"), ("source_file", "immutable lock"),
                                  ("identifier", "locked license identifier")):
            damaged = copy.deepcopy(inventory)
            if mutation == "missing":
                damaged["system_license_notices"] = []
            elif mutation == "duplicate":
                damaged["system_license_notices"].append(copy.deepcopy(notice))
            elif mutation == "text":
                damaged["system_license_notices"][0]["text"] += "forged"
            elif mutation == "file":
                damaged["files"][notice_path] = "0" * 64
            elif mutation == "source_file":
                damaged["system_license_notices"][0]["files"][0]["sha256"] = "0" * 64
                damaged["files"][notice_path] = "0" * 64
            else:
                damaged["system_license_notices"][0]["license_id"] = "LicenseRef-Forged"
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, message):
                finalize_artifacts.build_spdx([image], "5.6.0", "f" * 40, 123, [], damaged, lock)

    def test_spdx_records_each_immutable_static_runtime_source(self) -> None:
        image = self.root / "atrinik-classic-client-5.6.0-linux-x86_64.AppImage"
        image.write_bytes(b"image")
        runtime = {"version": "20251108", "url": "https://example.invalid/runtime",
                   "sha256": "a" * 64, "license": "MIT AND LGPL-2.1-or-later"}
        inventory = {"lock_sha256": "b" * 64, "native_inputs": {
            "sources": [], "tools": [], "runtime": runtime}, "bundled_library_records": []}
        source = {"name": "libfuse", "version": "3.15.0", "url": "https://example.invalid/libfuse.tar.gz",
                  "sha256": "c" * 64, "license": "LGPL-2.1-or-later"}
        spdx = finalize_artifacts.build_spdx([image], "5.6.0", "d" * 40, 123, [], inventory,
                                            {"runtime_sources": [source]})
        package = next(package for package in spdx["packages"] if package["name"] == "libfuse")
        self.assertEqual(package["versionInfo"], "3.15.0")
        self.assertEqual(package["checksums"], [{"algorithm": "SHA256", "checksumValue": "c" * 64}])
        self.assertEqual(package["licenseDeclared"], "LGPL-2.1-or-later")
        for relation in ("DEPENDS_ON", "GENERATED_FROM"):
            self.assertIn({"spdxElementId": "SPDXRef-AppImageInput-appimage-runtime",
                           "relationshipType": relation,
                           "relatedSpdxElement": "SPDXRef-AppImageRuntimeSource-libfuse"}, spdx["relationships"])

    def test_finalizer_rejects_wrong_checkout_schema_or_epoch_before_writes(self) -> None:
        arguments = ["finalize_artifacts.py", "--version", "5.6.0", "--revision", "c" * 40,
                     "--source-epoch", "123", "--directory", str(self.root)]
        for head, schema, epoch, error in (("d" * 40, 3, "123", "checkout"),
                                           ("c" * 40, 2, "123", "schema"),
                                           ("c" * 40, 3, "124", "epoch")):
            with self.subTest(error=error), mock.patch.object(sys, "argv", arguments), mock.patch("release_artifacts.git_value", side_effect=[head, epoch]), mock.patch("release_artifacts.source_schema", return_value=schema):
                with self.assertRaisesRegex(RuntimeError, error):
                    finalize_artifacts.main()
                self.assertEqual(list(self.root.iterdir()), [])

    def test_release_manifest_records_exact_dependency_bundle(self) -> None:
        artifact = self.root / "artifact.zip"
        artifact.write_bytes(b"artifact")
        descriptor = {
            "image": "ghcr.io/atrinik/classic-dependencies",
            "digest": "sha256:" + "a" * 64,
            "material_digest": "sha256:" + "b" * 64,
        }
        manifest = finalize_artifacts.build_release_manifest(
            [artifact], "5.6.0", "c" * 40, 123, descriptor, [], {"schema_version": 1}, {"schema": 1}
        )
        self.assertEqual(manifest["schema_version"], 3)
        self.assertIs(manifest["dependency_bundle"], descriptor)
        self.assertEqual(manifest["appimage_packaging_lock"], {"schema_version": 1})
        self.assertEqual(manifest["appimage_inventory"], {"schema": 1})
        self.assertEqual(manifest["artifacts"][0]["name"], artifact.name)
        self.assertEqual(manifest["artifacts"][0]["size"], len(b"artifact"))

    def test_expected_set_uses_unambiguous_classic_names(self) -> None:
        names = finalize_artifacts.expected_names("5.6.0")
        self.assertIn("atrinik-classic-5.6.0.tar.gz", names)
        self.assertIn("atrinik-classic-editor-5.6.0.tar.gz", names)
        self.assertIn("atrinik-classic-client-5.6.0-linux-x86_64.AppImage", names)
        self.assertIn(
            "atrinik-classic-server-5.6.0-windows-x86_64.zip", names
        )
        self.assertNotIn("atrinik-server-5.6.0.tar.gz", names)


if __name__ == "__main__":
    unittest.main()
