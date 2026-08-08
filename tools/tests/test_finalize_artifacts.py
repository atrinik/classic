from __future__ import annotations

import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
import zipfile


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "finalize_artifacts.py"
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
        path = self.root / "atrinik-classic-client-6.0.0.tar.gz"
        package = "atrinik-classic-client-6.0.0"
        with tarfile.open(path, "w:gz") as archive:
            for name, content in (
                (f"{package}/VERSION", b"6.0.0\n"),
                (f"{package}/LICENSE.md", b"GPL\n"),
            ):
                member = tarfile.TarInfo(name)
                member.size = len(content)
                archive.addfile(member, io.BytesIO(content))
        finalize_artifacts.validate_source_archive(
            path, "atrinik-classic-client", "6.0.0"
        )

    def test_wheel_metadata_must_use_unified_name_and_version(self) -> None:
        path = self.root / "atrinik_classic_protocol-6.0.0-py3-none-any.whl"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(
                "atrinik_classic_protocol-6.0.0.dist-info/METADATA",
                "Name: atrinik-classic-protocol\nVersion: 6.0.0\n",
            )
        finalize_artifacts.validate_wheel(path, "6.0.0")
        with self.assertRaisesRegex(RuntimeError, "wrong distribution version"):
            finalize_artifacts.validate_wheel(path, "6.0.1")

    def test_expected_set_uses_unambiguous_classic_names(self) -> None:
        names = finalize_artifacts.expected_names("6.0.0")
        self.assertIn("atrinik-classic-6.0.0.tar.gz", names)
        self.assertIn("atrinik-classic-editor-6.0.0.tar.gz", names)
        self.assertIn(
            "atrinik-classic-server-6.0.0-windows-x86_64.zip", names
        )
        self.assertNotIn("atrinik-server-6.0.0.tar.gz", names)


if __name__ == "__main__":
    unittest.main()
