from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


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


if __name__ == "__main__":
    unittest.main()
