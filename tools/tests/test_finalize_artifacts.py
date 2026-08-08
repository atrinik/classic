from __future__ import annotations

import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
import zipfile
import sys


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

        empty = self.root / "empty.zip"
        with zipfile.ZipFile(empty, "w") as archive:
            archive.writestr("expected/atrinik.exe", b"")
        with self.assertRaisesRegex(RuntimeError, "only empty"):
            finalize_artifacts.validate_zip(empty, "expected", ("atrinik.exe",))

    def test_windows_server_zip_requires_runtime_payload(self) -> None:
        path = self.root / "server.zip"
        package = "atrinik-classic-server-5.6.0-windows-x86_64"
        required = (
            "server/atrinik-server.exe",
            "server/LICENSE.md",
            "server/ATTRIBUTIONS.md",
            "server/server.cfg",
            "server/permissions.cfg",
            "server/ca-bundle.crt",
            "server/*plugin_arena*.dll",
            "server/*plugin_python*.dll",
            "server/python*.dll",
            "server/Lib/*",
            "maps/*",
            "server/lib/*",
            "server/resources/*",
            "server/install_data/*",
            "server/install_data/http/client-maps/*",
        )
        files = (
            "server/atrinik-server.exe",
            "server/LICENSE.md",
            "server/ATTRIBUTIONS.md",
            "server/server.cfg",
            "server/permissions.cfg",
            "server/ca-bundle.crt",
            "server/plugin_arena.dll",
            "server/plugin_python.dll",
            "server/python313.dll",
            "server/Lib/os.py",
            "maps/world.map",
            "server/lib/helper.dll",
            "server/resources/archetypes",
            "server/install_data/accounts",
            "server/install_data/http/client-maps/map.json",
        )
        with zipfile.ZipFile(path, "w") as archive:
            for name in files:
                archive.writestr(f"{package}/{name}", b"fixture")
        finalize_artifacts.validate_zip(path, package, required)

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
