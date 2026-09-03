from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.verify_gpu_fixture_provenance import (
    ProvenanceError,
    load_json,
    load_provenance,
    verify,
)


ROOT = Path(__file__).resolve().parents[2]
PROVENANCE = ROOT / "src/tests/fixtures/player_view/content-provenance.json"
LOCK = ROOT.parent / "server/dependencies.lock.json"


class GpuFixtureProvenanceTests(unittest.TestCase):
    def test_bound_fixture_and_content_coordinate_pass(self) -> None:
        result = verify(ROOT.parent)

        self.assertEqual("v1.0.0", result["content_coordinate"]["tag"])
        self.assertEqual(
            "63eb9bb5f02fb9104c2385d5e01c28c3df20b735",
            result["content_coordinate"]["commit"],
        )
        self.assertEqual(
            "977ce63e38f4795545f42bd2f4a9c62bd38fcdb4cfde7cb7fe3c2cd9fe73983e",
            result["archdef"]["sha256"],
        )
        self.assertGreater(result["gpu_fixture_count"], 0)
        self.assertFalse(result["runtime_verified"])

    def test_archdef_drift_is_rejected(self) -> None:
        contract = load_json(PROVENANCE)
        contract["archdef"]["sha256"] = "0" * 64
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "provenance.json"
            path.write_text(json.dumps(contract), encoding="utf-8")

            with self.assertRaisesRegex(ProvenanceError, "archdef digest mismatch"):
                verify(ROOT.parent, provenance_path=path)

    def test_content_lock_drift_is_rejected(self) -> None:
        lock = load_json(LOCK)
        lock["dependencies"][0]["tag"] = "v1.6.0"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "dependencies.lock.json"
            path.write_text(json.dumps(lock), encoding="utf-8")

            with self.assertRaisesRegex(ProvenanceError, "lock tag disagrees"):
                verify(ROOT.parent, lock_path=path)

    def test_artifact_coordinate_drift_is_rejected(self) -> None:
        contract = load_json(PROVENANCE)
        contract["content"]["selected"]["artifact"]["url"] = (
            "https://github.com/atrinik/content/releases/download/v9.9.9/"
            "atrinik-content-1.0.0-classic-runtime.tar.gz"
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "provenance.json"
            path.write_text(json.dumps(contract), encoding="utf-8")

            with self.assertRaisesRegex(ProvenanceError, "URL"):
                load_provenance(path)

    def test_duplicate_json_keys_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "duplicate.json"
            path.write_text('{"schema_version": 1, "schema_version": 2}', encoding="utf-8")

            with self.assertRaisesRegex(ProvenanceError, "duplicate JSON key"):
                load_json(path)

    def test_worldmaker_boundary_is_explicit(self) -> None:
        contract = load_provenance(PROVENANCE)
        self.assertFalse(contract["worldmaker"]["archdef_generated"])
        self.assertEqual(
            "classic/client/data/archdef.dat",
            contract["worldmaker"]["archdef_source"],
        )
        self.assertEqual(
            ["client-maps", "data/*.zz"],
            contract["worldmaker"]["generated_outputs"],
        )

    def test_content_runtime_root_symlink_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "target"
            target.mkdir()
            link = root / "runtime"
            link.symlink_to(target, target_is_directory=True)
            with self.assertRaises(ProvenanceError):
                verify(ROOT.parent, content_runtime=link)


if __name__ == "__main__":
    unittest.main()
