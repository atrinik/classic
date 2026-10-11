"""Exercise the release/source contract with the real AppImage inspector."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "release"))
import finalize_artifacts
import locked_inputs
import release_artifacts
import test_appimage as package_fixtures


@unittest.skipIf(getattr(package_fixtures.AppImageTests, "__unittest_skip__", False),
                 "real AppImage fixtures require trusted SquashFS and ELF inspectors")
class ReleaseAppImageIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.package = package_fixtures.AppImageTests("runTest")
        self.package.setUp()
        self.addCleanup(self.package.doCleanups)
        self.source = self.package.source
        self.assets = self.package.root / "assets"
        self.assets.mkdir()
        self.version = self.package.version
        self.package.lock["tools"] = []
        self.package.lock["runtime"].update({
            "url": "https://example.invalid/immutable-runtime",
            "license": "MIT",
        })
        self.package.lock_path.write_text(json.dumps(self.package.lock, sort_keys=True) + "\n")
        for relative in ("client/dependencies.lock.json", "server/dependencies.lock.json",
                         "dependencies.bundle.json", "tools/release/finalize_artifacts.py"):
            destination = self.source / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(release_artifacts.ROOT / relative, destination)
        self.git("init", "--quiet")
        self.git("add", ".")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "trusted source")
        self.revision = self.git("rev-parse", "HEAD")
        self.git("tag", "v" + self.version)
        self.epoch = int(self.git("show", "-s", "--format=%ct", self.revision))
        self.package.metadata["revision"] = self.revision
        self.package.metadata["lock_sha256"] = hashlib.sha256(self.package.lock_path.read_bytes()).hexdigest()
        self.package.metadata["native_inputs"] = {
            key: self.package.lock[key] for key in ("sources", "tools", "runtime")
        }
        self.inputs = locked_inputs.load_locked_inputs(self.version, self.source, 3)
        self.bundle = json.loads((self.source / "dependencies.bundle.json").read_text())

    def git(self, *arguments):
        return subprocess.run(["git", "-C", str(self.source), *arguments], check=True,
                              capture_output=True, text=True).stdout.strip()

    def candidate(self, *, invalid_spdx=False):
        names = release_artifacts.expected_names(self.version, 3)
        sbom_name = f"atrinik-classic-{self.version}.spdx.json"
        for name in names - {"SHA256SUMS", "release-manifest.json", sbom_name}:
            (self.assets / name).write_bytes(b"fixture release artifact")
        shutil.copyfile(self.package.image(),
                        self.assets / f"atrinik-classic-client-{self.version}-linux-x86_64.AppImage")
        inventory = json.loads(json.dumps(self.package.metadata))
        paths = sorted(path for path in self.assets.iterdir()
                       if path.name not in ("SHA256SUMS", "release-manifest.json", sbom_name))
        sbom = {} if invalid_spdx else finalize_artifacts.build_spdx(
            paths, self.version, self.revision, self.epoch, self.inputs, inventory, self.package.lock,
        )
        sbom_path = self.assets / sbom_name
        sbom_path.write_text(json.dumps(sbom))
        manifest = finalize_artifacts.build_release_manifest(
            sorted([*paths, sbom_path]), self.version, self.revision, self.epoch,
            self.bundle, self.inputs, self.package.lock, inventory,
        )
        (self.assets / "release-manifest.json").write_text(json.dumps(manifest))
        (self.assets / "SHA256SUMS").write_text("".join(
            f"{finalize_artifacts.sha256(path)}  {path.name}\n"
            for path in sorted(self.assets.iterdir()) if path.name != "SHA256SUMS"
        ))

    def validate(self):
        return release_artifacts.validate_candidate(self.assets, "v" + self.version,
                                                     self.revision, self.source)

    def test_complete_candidate_uses_immutable_tls_resources_with_real_inspector(self):
        self.candidate()
        self.assertEqual(len(self.validate()), 13)
        # Current checkout bytes cannot replace this candidate's committed trust.
        (self.source / "client/ca-bundle.crt").write_text("uncommitted trust change\n")
        (self.source / "tools/ci/appimage/openssl.cnf").write_text("uncommitted config change\n")
        self.assertEqual(len(self.validate()), 13)

    def test_resealed_certificate_tampering_is_rejected_before_spdx(self):
        self.package.write("usr/share/atrinik/ca-bundle.crt", "forged certificate trust\n")
        self.candidate(invalid_spdx=True)
        with self.assertRaisesRegex(ValueError, "certificate trust differs from trusted source"):
            self.validate()

    def test_resealed_openssl_config_tampering_is_rejected_before_spdx(self):
        path = self.package.payload / "usr/share/atrinik/openssl.cnf"
        path.write_text(path.read_text() + "# forged but syntactically valid config\n")
        self.candidate(invalid_spdx=True)
        with self.assertRaisesRegex(ValueError, "OpenSSL configuration differs from trusted source"):
            self.validate()


if __name__ == "__main__":
    unittest.main()
