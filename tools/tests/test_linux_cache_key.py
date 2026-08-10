from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from tools.ci import linux_cache_key


COMMIT = "a" * 40
DIGEST = "sha256:" + "b" * 64


class LinuxCacheKeyTests(unittest.TestCase):
    def test_trust_scopes_are_isolated(self) -> None:
        self.assertEqual(
            linux_cache_key.cache_scope("push", "refs/heads/main", "", COMMIT),
            "trusted-main",
        )
        self.assertEqual(
            linux_cache_key.cache_scope(
                "pull_request", "refs/pull/85/merge", "85", COMMIT
            ),
            "pr-85",
        )
        self.assertEqual(
            linux_cache_key.cache_scope("merge_group", "refs/heads/queue", "", COMMIT),
            f"merge-{COMMIT}",
        )
        with self.assertRaises(linux_cache_key.CacheKeyError):
            linux_cache_key.cache_scope(
                "push", "refs/heads/untrusted", "", COMMIT
            )

    def test_every_material_input_invalidates_the_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            material = root / "contract.txt"
            material.write_text("flags=-O2\n", encoding="utf-8")
            arguments = {
                "component": "server",
                "event": "pull_request",
                "ref": "refs/pull/85/merge",
                "pr_number": "85",
                "commit": COMMIT,
                "image_digest": DIGEST,
                "compiler": "gcc-15.2.0",
                "epoch": "1",
                "materials": ["contract.txt"],
                "root": root,
            }
            original = linux_cache_key.build_prefix(**arguments)
            variants = (
                {"image_digest": "sha256:" + "c" * 64},
                {"compiler": "gcc-15.2.1"},
                {"epoch": "2"},
                {"component": "client"},
            )
            for change in variants:
                with self.subTest(change=change):
                    changed = dict(arguments)
                    changed.update(change)
                    self.assertNotEqual(
                        original, linux_cache_key.build_prefix(**changed)
                    )
            material.write_text("flags=-O3\n", encoding="utf-8")
            self.assertNotEqual(
                original, linux_cache_key.build_prefix(**arguments)
            )

    def test_unsafe_or_missing_materials_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(linux_cache_key.CacheKeyError):
                linux_cache_key.material_digest(["../escape"], root)
            with self.assertRaises(linux_cache_key.CacheKeyError):
                linux_cache_key.material_digest(["missing"], root)

    def test_material_symlinks_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            root = temporary / "root"
            outside = temporary / "outside"
            root.mkdir()
            outside.mkdir()
            material = outside / "material.txt"
            material.write_text("flags=-O2\n", encoding="utf-8")

            (root / "final-link").symlink_to(material)
            (root / "ancestor-link").symlink_to(outside, target_is_directory=True)
            for value in ("final-link", "ancestor-link/material.txt"):
                with self.subTest(value=value):
                    with self.assertRaises(linux_cache_key.CacheKeyError):
                        linux_cache_key.material_digest([value], root)


if __name__ == "__main__":
    unittest.main()
