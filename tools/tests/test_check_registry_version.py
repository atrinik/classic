from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "check_registry_version.py"
SPEC = importlib.util.spec_from_file_location("check_registry_version", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
check_registry_version = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_registry_version)


class CheckRegistryVersionTests(unittest.TestCase):
    def test_finds_existing_tag_digest(self) -> None:
        value = [
            {
                "name": "sha256:" + "a" * 64,
                "metadata": {"container": {"tags": ["5.6.0"]}},
            }
        ]
        self.assertEqual(
            check_registry_version.find_version(value, "5.6.0"),
            "sha256:" + "a" * 64,
        )

    def test_missing_tag_returns_none(self) -> None:
        self.assertIsNone(check_registry_version.find_version([], "5.6.0"))

    def test_finds_latest_alias_digest(self) -> None:
        value = [
            {
                "name": "sha256:" + "b" * 64,
                "metadata": {"container": {"tags": ["5.6.0", "latest"]}},
            }
        ]
        self.assertEqual(
            check_registry_version.find_version(value, "latest"),
            "sha256:" + "b" * 64,
        )

    def test_finds_material_digest_tag(self) -> None:
        tag = "materials-" + "c" * 64
        value = [
            {
                "name": "sha256:" + "d" * 64,
                "metadata": {"container": {"tags": [tag]}},
            }
        ]
        self.assertEqual(
            check_registry_version.find_version(value, tag),
            "sha256:" + "d" * 64,
        )

    def test_absent_material_package_writes_a_missing_result(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "output"
            arguments = [
                "check_registry_version.py",
                "--organization",
                "atrinik",
                "--package",
                "classic-dependencies",
                "--tag",
                "materials-" + "e" * 64,
                "--github-output",
                str(output),
            ]
            response = (1, {"message": "Package not found.", "status": "404"}, "")
            with mock.patch.object(check_registry_version, "request", return_value=response):
                with mock.patch.object(sys, "argv", arguments):
                    self.assertEqual(check_registry_version.main(), 0)
            self.assertEqual(
                output.read_text(),
                "package_exists=false\nexists=false\ndigest=\n",
            )

    def test_absent_material_tag_preserves_package_existence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "output"
            arguments = [
                "check_registry_version.py",
                "--organization",
                "atrinik",
                "--package",
                "classic-dependencies",
                "--tag",
                "materials-" + "a" * 64,
                "--github-output",
                str(output),
            ]
            with mock.patch.object(
                check_registry_version, "request", return_value=(0, [], "")
            ):
                with mock.patch.object(sys, "argv", arguments):
                    self.assertEqual(check_registry_version.main(), 0)
            self.assertEqual(
                output.read_text(),
                "package_exists=true\nexists=false\ndigest=\n",
            )

    def test_other_material_package_errors_remain_terminal(self) -> None:
        arguments = [
            "check_registry_version.py",
            "--organization",
            "atrinik",
            "--package",
            "classic-dependencies",
            "--tag",
            "materials-" + "f" * 64,
        ]
        response = (1, {"message": "Forbidden", "status": "403"}, "forbidden")
        with mock.patch.object(check_registry_version, "request", return_value=response):
            with mock.patch.object(sys, "argv", arguments):
                with self.assertRaisesRegex(RuntimeError, "cannot audit GHCR package"):
                    check_registry_version.main()

    def test_malformed_version_metadata_fails_closed(self) -> None:
        malformed = (
            {"name": "sha256:" + "a" * 64},
            {"name": "sha256:" + "a" * 64, "metadata": None},
            {"name": "sha256:" + "a" * 64, "metadata": {}},
            {
                "name": "sha256:" + "a" * 64,
                "metadata": {"container": None},
            },
            {
                "name": "sha256:" + "a" * 64,
                "metadata": {"container": {}},
            },
            {
                "name": "sha256:" + "a" * 64,
                "metadata": {"container": {"tags": [1]}},
            },
        )
        tag = "materials-" + "b" * 64
        for value in malformed:
            with self.subTest(value=value):
                with self.assertRaisesRegex(RuntimeError, "invalid .*metadata"):
                    check_registry_version.find_version([value], tag)


if __name__ == "__main__":
    unittest.main()
