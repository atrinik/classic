from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "verify_import_history.py"
SPEC = importlib.util.spec_from_file_location("verify_import_history", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
verify_import_history = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify_import_history)


class ReleaseTagPolicyTests(unittest.TestCase):
    def test_semantic_versions_sort_numerically(self) -> None:
        tags = ["v6.10.0", "v6.2.0", "v7.0.0", "v6.0.0"]
        self.assertEqual(
            sorted(tags, key=verify_import_history.semantic_version),
            ["v6.0.0", "v6.2.0", "v6.10.0", "v7.0.0"],
        )

    def test_prefixed_or_partial_versions_are_rejected(self) -> None:
        for tag in ("client-v6.0.0", "6.0.0", "v6.0", "v6.0.0-rc.1"):
            with self.subTest(tag=tag):
                with self.assertRaisesRegex(RuntimeError, "unprefixed semantic version"):
                    verify_import_history.semantic_version(tag)


if __name__ == "__main__":
    unittest.main()
