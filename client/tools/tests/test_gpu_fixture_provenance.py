from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

from tools.verify_gpu_fixture_provenance import (
    ProvenanceError,
    _runtime_file_paths,
    _validate_source_profile,
    _validate_runtime_manifest,
    _manifest_files_digest,
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

        selected = next(entry for entry in load_json(LOCK)["dependencies"]
                        if entry["name"] == "content")
        self.assertEqual(selected["tag"], result["content_coordinate"]["tag"])
        self.assertEqual(selected["commit"], result["content_coordinate"]["commit"])
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

    def test_dependency_manager_metadata_is_not_content(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "manifest.json").write_text("{}", encoding="utf-8")
            (root / ".atrinik-dependency.json").write_text("{}", encoding="utf-8")
            content = root / "maps/example.map"
            content.parent.mkdir()
            content.write_text("fixture", encoding="utf-8")

            self.assertEqual(
                {"maps/example.map"},
                _runtime_file_paths(root, "manifest.json"),
            )

    def test_non_regular_runtime_entry_is_rejected(self) -> None:
        if not hasattr(os, "mkfifo"):
            self.skipTest("FIFO creation is unavailable on this platform")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            os.mkfifo(root / "unexpected.fifo")
            with self.assertRaisesRegex(ProvenanceError, "non-regular entry"):
                _runtime_file_paths(root, "manifest.json")


class SourceProfileQualificationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.runtime = self.root / "content"
        self.resources = self.root / "resources"
        self.runtime.mkdir()
        self.resources.mkdir()
        (self.resources / "resource").write_text("resource")
        self.provenance = load_provenance(PROVENANCE)
        entries = []
        for name, data in (("lib/archetypes", b"archetypes"),
                           ("maps/celestial-migration-index.json", b"{}")):
            path = self.runtime / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            entries.append({"path": name, "sha256": hashlib.sha256(data).hexdigest(),
                            "size": len(data)})
        self.provenance["content"]["selected"]["artifact"]["archetypes"] = entries[0]
        self.manifest = {
            "schema_version": 2, "target": "classic", "release_version": "unreleased",
            "source": {"repository": "atrinik/content", "branch": "main", "commit": "a" * 40},
            "content_format": "classic-ads-v1",
            "artifact_format": "atrinik-classic-runtime-content-v1",
            "compatible_classic_releases": ">=5.10.1 <6.0.0",
            "consumers": ["classic/client", "classic/editor", "classic/server"],
            "replacement_ready": False, "replacement_toolkit_package": False,
            "license_files": [], "files": entries,
            "celestial_schema_version": 1, "celestial_runtime_factory_version": 1,
            "celestial_migration_index": entries[1]["path"],
            "celestial_migration_index_sha256": entries[1]["sha256"],
            "celestial_manifest_files_sha256": _manifest_files_digest(entries),
        }
        self.qualification = {
            "schema_version": 1, "kind": "classic-source-profile-qualification",
            "package_version": "0.0.0",
            "source": {name: {"commit": "a" * 40, "dirty": False}
                       for name in ("classic", "content", "resources")},
            "content_manifest_sha256": "0" * 64,
        }
        self.path = self.root / "qualification.json"
        self.write_manifest()

    def write_manifest(self) -> None:
        data = json.dumps(self.manifest).encode()
        (self.runtime / "manifest.json").write_bytes(data)
        self.qualification["content_manifest_sha256"] = hashlib.sha256(data).hexdigest()

    def write_qualification(self) -> str:
        data = json.dumps(self.qualification).encode()
        self.path.write_bytes(data)
        return hashlib.sha256(data).hexdigest()

    def check(self, version: str = "0.0.0") -> tuple:
        return _validate_source_profile(self.path, self.write_qualification(), version,
                                        self.runtime, self.resources, self.provenance)

    def test_bound_source_profile_is_explicitly_unreleased(self) -> None:
        runtime, qualification = self.check()
        self.assertEqual("unreleased", runtime["release_version"])
        self.assertEqual(self.qualification, qualification)
        self.assertEqual(2, runtime["files"])

    def test_source_mode_cannot_admit_release_package(self) -> None:
        for version in ("5.78.0", "0.0.1", "", "00.0.0"):
            with self.subTest(version=version), self.assertRaisesRegex(ProvenanceError, "0.0.0"):
                self.check(version)

    def test_default_release_contract_still_rejects_unreleased(self) -> None:
        with self.assertRaisesRegex(ProvenanceError, "not approved"):
            _validate_runtime_manifest(self.runtime, self.provenance)

    def test_release_contract_remains_valid(self) -> None:
        selected = self.provenance["content"]["selected"]
        self.manifest["source"]["commit"] = selected["commit"]
        self.manifest["release_version"] = selected["release_version"]
        self.write_manifest()
        data = (self.runtime / "manifest.json").read_bytes()
        selected["runtime_manifests"] = [{"release_version": selected["release_version"],
                                         "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}]
        self.assertEqual(selected["release_version"],
                         _validate_runtime_manifest(self.runtime, self.provenance)["release_version"])
        with self.assertRaisesRegex(ProvenanceError, "unreleased"):
            self.check()

    def test_qualification_closed_schema_and_types(self) -> None:
        baseline = copy.deepcopy(self.qualification)
        variants = [dict(baseline, schema_version=True), dict(baseline, extra=1),
                    dict(baseline, kind="release"), dict(baseline, package_version="5.78.0")]
        for field, value in (("dirty", 0), ("commit", "A" * 40), ("extra", True)):
            changed = copy.deepcopy(baseline)
            changed["source"]["classic"][field] = value
            variants.append(changed)
        for variant in variants:
            with self.subTest(variant=variant), self.assertRaises(ProvenanceError):
                self.qualification = variant
                self.check()

    def test_digest_and_coordinate_bindings(self) -> None:
        sha = self.write_qualification()
        with self.assertRaisesRegex(ProvenanceError, "qualification digest"):
            _validate_source_profile(self.path, "0" * 64, "0.0.0", self.runtime,
                                     self.resources, self.provenance)
        self.qualification["content_manifest_sha256"] = "0" * 64
        with self.assertRaisesRegex(ProvenanceError, "manifest digest"):
            self.check()
        self.write_manifest()
        self.qualification["source"]["content"]["commit"] = "b" * 40
        with self.assertRaisesRegex(ProvenanceError, "source coordinate"):
            self.check()

    def test_content_integrity_and_inventory(self) -> None:
        archetypes = self.runtime / "lib/archetypes"
        original = archetypes.read_bytes()
        archetypes.write_bytes(b"tampered")
        with self.assertRaisesRegex(ProvenanceError, "digest mismatch"):
            self.check()
        archetypes.write_bytes(original)
        extra = self.runtime / ".atrinik-workspace-managed.json"
        extra.write_text("{}")
        with self.assertRaisesRegex(ProvenanceError, "files differ"):
            self.check()
        extra.unlink()
        archetypes.unlink()
        with self.assertRaisesRegex(ProvenanceError, "files differ"):
            self.check()
        archetypes.symlink_to(self.resources / "resource")
        with self.assertRaisesRegex(ProvenanceError, "symbolic link"):
            self.check()

    def test_archetype_and_celestial_compatibility(self) -> None:
        baseline = copy.deepcopy(self.manifest)
        for key, value in (("celestial_schema_version", 2),
                           ("celestial_manifest_files_sha256", "0" * 64),
                           ("celestial_migration_index_sha256", "0" * 64)):
            self.manifest = dict(baseline, **{key: value})
            self.write_manifest()
            with self.subTest(key=key), self.assertRaises(ProvenanceError):
                self.check()
        self.manifest = copy.deepcopy(baseline)
        self.manifest["files"][0]["sha256"] = "0" * 64
        self.manifest["celestial_manifest_files_sha256"] = _manifest_files_digest(self.manifest["files"])
        self.write_manifest()
        with self.assertRaisesRegex(ProvenanceError, "archetype artifact"):
            self.check()

    def test_resources_and_qualification_symlinks_rejected(self) -> None:
        sha = self.write_qualification()
        target = self.root / "attestation.json"
        self.path.rename(target)
        self.path.symlink_to(target)
        with self.assertRaisesRegex(ProvenanceError, "regular file"):
            _validate_source_profile(self.path, sha, "0.0.0", self.runtime,
                                     self.resources, self.provenance)
        self.path.unlink()
        (self.resources / "link").symlink_to(self.runtime / "lib/archetypes")
        with self.assertRaisesRegex(ProvenanceError, "symbolic link"):
            self.check()

    def test_options_cannot_bypass_static_fixture_checks(self) -> None:
        contract = load_json(PROVENANCE)
        contract["archdef"]["sha256"] = "0" * 64
        path = self.root / "provenance.json"
        path.write_text(json.dumps(contract))
        with self.assertRaisesRegex(ProvenanceError, "archdef digest mismatch"):
            verify(ROOT.parent, provenance_path=path, content_runtime=self.runtime,
                   source_profile_qualification=self.path,
                   source_profile_qualification_sha256=self.write_qualification(),
                   package_version="0.0.0", profile_resources=self.resources)

    def test_partial_source_options_rejected(self) -> None:
        with self.assertRaisesRegex(ProvenanceError, "all options and paired trees"):
            verify(ROOT.parent, content_runtime=self.runtime, package_version="0.0.0")


if __name__ == "__main__":
    unittest.main()
