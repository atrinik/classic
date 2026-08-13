from __future__ import annotations

import importlib.util
import io
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "dependency_bundle.py"
SPEC = importlib.util.spec_from_file_location("dependency_bundle", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
dependency_bundle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dependency_bundle)


class DependencyBundleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "root"
        self.cache = Path(self.temporary.name) / "cache"
        self.root.mkdir()
        (self.root / "server/tools").mkdir(parents=True)
        shutil.copyfile(
            Path(__file__).resolve().parents[2] / "server/tools/dependencies.py",
            self.root / "server/tools/dependencies.py",
        )
        records = {}
        for name in ("content", "libpcpnatpmp", "resources", "sound"):
            archive = Path(self.temporary.name) / f"{name}.tar.gz"
            with tarfile.open(archive, "w:gz") as output:
                payload = f"{name}\n".encode()
                info = tarfile.TarInfo(f"{name}-v1/file")
                info.size = len(payload)
                output.addfile(info, io.BytesIO(payload))
            digest = dependency_bundle.sha256_file(archive)
            cached = self.cache / "downloads" / f"{name}-{digest}.tar.gz"
            cached.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(archive, cached)
            records[name] = {
                "name": name,
                "repository": f"atrinik/{name}",
                "tag": "v1.0.0",
                "commit": "a" * 40,
                "url": f"https://example.invalid/{name}.tar.gz",
                "sha256": digest,
                "destination": name,
                "strip_components": 1,
            }
        self.write_lock("client", [records["sound"]])
        self.write_lock("server", [records["content"], records["resources"]])
        source_lock = self.root / "server/cmake/immutable_sources.lock.json"
        source_lock.parent.mkdir(parents=True)
        source_lock.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "sources": {
                        "libpcpnatpmp": {
                            "url": records["libpcpnatpmp"]["url"],
                            "sha256": records["libpcpnatpmp"]["sha256"],
                            "tree_sha256": "b" * 64,
                            "mingw_tree_sha256": "c" * 64,
                        }
                    },
                }
            )
            + "\n",
            encoding="utf-8",
        )
        self.layout = Path(self.temporary.name) / "layout"
        self.descriptor = dependency_bundle.build_layout(
            self.root, self.cache, self.layout
        )
        self.descriptor_path = self.root / "dependencies.bundle.json"
        self.descriptor_path.write_text(
            json.dumps(self.descriptor, indent=2) + "\n", encoding="utf-8"
        )
        self.bundle = Path(self.temporary.name) / "bundle"
        manifest_digest = str(self.descriptor["digest"]).removeprefix("sha256:")
        manifest = json.loads(
            (self.layout / "blobs/sha256" / manifest_digest).read_text()
        )
        layer_digest = manifest["layers"][0]["digest"].removeprefix("sha256:")
        with tarfile.open(self.layout / "blobs/sha256" / layer_digest, "r:gz") as layer:
            layer.extractall(Path(self.temporary.name) / "extracted", filter="data")
        self.bundle = Path(self.temporary.name) / "extracted/bundle"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_lock(self, component: str, dependencies: list[dict[str, object]]) -> None:
        path = self.root / component / "dependencies.lock.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps({"schema_version": 1, "dependencies": dependencies}) + "\n",
            encoding="utf-8",
        )

    def rewrite_manifest(self, manifest: dict[str, object]) -> None:
        manifest_path = self.bundle / "manifest.json"
        manifest_path.write_bytes(dependency_bundle.canonical_json(manifest))
        descriptor = json.loads(self.descriptor_path.read_text())
        descriptor["bundle_manifest_sha256"] = (
            "sha256:" + dependency_bundle.sha256_file(manifest_path)
        )
        self.descriptor_path.write_text(
            json.dumps(descriptor, indent=2) + "\n", encoding="utf-8"
        )

    def test_layout_is_deterministic_and_descriptor_pins_manifest(self) -> None:
        second = Path(self.temporary.name) / "layout-second"
        actual = dependency_bundle.build_layout(self.root, self.cache, second)
        self.assertEqual(actual, self.descriptor)
        self.assertTrue(str(actual["digest"]).startswith("sha256:"))
        self.assertEqual(
            actual["tag"],
            "materials-" + str(actual["material_digest"]).removeprefix("sha256:"),
        )

    def test_verifies_and_installs_every_archive_in_consumer_caches(self) -> None:
        manifest = dependency_bundle.verify_bundle(
            self.root, self.descriptor_path, self.bundle
        )
        self.assertEqual(len(manifest["artifacts"]), 4)
        dependency_bundle.install_bundle(
            self.root, self.descriptor_path, self.bundle
        )
        for path in (
            self.root / "client/build/dependencies/downloads",
            self.root / "server/build/dependencies/downloads",
            self.root / "server/build/dependency-cache/downloads",
        ):
            self.assertTrue(any(path.iterdir()))

    def test_corrupt_bundle_fails_closed(self) -> None:
        sound = next((self.bundle / "archives").glob("sound-*.tar.gz"))
        sound.write_bytes(b"corrupt")
        with self.assertRaisesRegex(dependency_bundle.BundleError, "failed verification"):
            dependency_bundle.verify_bundle(
                self.root, self.descriptor_path, self.bundle
            )

    def test_extra_bundle_file_fails_closed(self) -> None:
        (self.bundle / "unexpected").write_text("unexpected", encoding="utf-8")
        with self.assertRaisesRegex(dependency_bundle.BundleError, "unexpected files"):
            dependency_bundle.verify_bundle(
                self.root, self.descriptor_path, self.bundle
            )

    def test_artifact_digest_must_match_its_locked_input(self) -> None:
        manifest_path = self.bundle / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        artifacts = {artifact["name"]: artifact for artifact in manifest["artifacts"]}
        content = artifacts["content"]
        sound = artifacts["sound"]
        old_path = self.bundle / content["path"]
        replacement_path = self.bundle / "archives" / (
            f"content-{sound['sha256']}.tar.gz"
        )
        shutil.copyfile(self.bundle / sound["path"], replacement_path)
        old_path.unlink()
        content.update(
            {
                "path": f"archives/content-{sound['sha256']}.tar.gz",
                "sha256": sound["sha256"],
                "size": sound["size"],
            }
        )
        self.rewrite_manifest(manifest)

        with self.assertRaisesRegex(
            dependency_bundle.BundleError, "does not match its locked input"
        ):
            dependency_bundle.verify_bundle(
                self.root, self.descriptor_path, self.bundle
            )

    def test_symlinked_bundle_root_fails_closed(self) -> None:
        linked = Path(self.temporary.name) / "linked-bundle"
        linked.symlink_to(self.bundle, target_is_directory=True)
        with self.assertRaisesRegex(dependency_bundle.BundleError, "bundle root"):
            dependency_bundle.verify_bundle(
                self.root, self.descriptor_path, linked
            )

    def test_stale_source_lock_fails_closed(self) -> None:
        lock = self.root / "client/dependencies.lock.json"
        lock.write_text(lock.read_text().replace("v1.0.0", "v1.0.1"), encoding="utf-8")
        with self.assertRaisesRegex(dependency_bundle.BundleError, "stale"):
            dependency_bundle.verify_bundle(
                self.root, self.descriptor_path, self.bundle
            )


if __name__ == "__main__":
    unittest.main()
