from __future__ import annotations

import io
import hashlib
from contextlib import redirect_stdout
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "release"))
import release_artifacts as artifacts
import locked_inputs


class ReleaseArtifactsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.directory = self.root / "assets"
        self.directory.mkdir()
        self.version = "5.99.0"
        self.revision = "c" * 40

    def tearDown(self):
        self.tmp.cleanup()

    def candidate(self, schema):
        names = artifacts.expected_names(self.version, schema)
        for name in names - {"SHA256SUMS", "release-manifest.json"}:
            (self.directory / name).write_bytes(b"artifact")
        manifest = {"schema_version": schema, "tag": "v" + self.version,
                    "version": self.version, "revision": self.revision,
                    "artifacts": [{"name": name, "sha256": artifacts.sha256(self.directory / name),
                                   "size": (self.directory / name).stat().st_size}
                                  for name in sorted(names - {"SHA256SUMS", "release-manifest.json"})]}
        (self.directory / "release-manifest.json").write_text(json.dumps(manifest))
        self.checksums()

    def checksums(self):
        (self.directory / "SHA256SUMS").write_text("".join(
            f"{artifacts.sha256(path)}  {path.name}\n" for path in sorted(self.directory.iterdir())
            if path.name != "SHA256SUMS"))

    def validate(self, schema):
        with mock.patch.object(artifacts, "source_schema", return_value=schema), mock.patch.object(artifacts, "validate_source_metadata"), mock.patch.object(artifacts, "validate_deb"), mock.patch.object(artifacts, "validate_appimage_metadata"), mock.patch.object(artifacts, "git_value", return_value=self.revision):
            return artifacts.validate_candidate(self.directory, "v" + self.version, self.revision)

    def test_historical_schema_one_retains_exact_twelve_without_deb(self):
        self.candidate(1)
        self.assertEqual(len(self.validate(1)), 12)
        (self.directory / f"atrinik-classic-client-{self.version}-linux-amd64.deb").write_bytes(b"extra")
        with self.assertRaisesRegex(RuntimeError, "artifact set"):
            self.validate(1)

    def test_historical_schema_two_requires_deb_and_rejects_tamper(self):
        self.candidate(2)
        self.assertEqual(len(self.validate(2)), 13)
        deb = self.directory / f"atrinik-classic-client-{self.version}-linux-amd64.deb"
        deb.write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "hash/size mismatch"):
            self.validate(2)
        deb.unlink()
        with self.assertRaisesRegex(RuntimeError, "artifact set"):
            self.validate(2)

    def test_schema_three_requires_appimage_and_rejects_deb_or_tampering(self):
        self.candidate(3)
        self.assertEqual(len(self.validate(3)), 13)
        image = self.directory / f"atrinik-classic-client-{self.version}-linux-x86_64.AppImage"
        unexpected = self.directory / f"atrinik-classic-client-{self.version}-linux-amd64.deb"
        unexpected.write_bytes(b"historical package")
        with self.assertRaisesRegex(RuntimeError, "artifact set"):
            self.validate(3)
        unexpected.unlink()
        image.write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "hash/size mismatch"):
            self.validate(3)
        image.unlink()
        with self.assertRaisesRegex(RuntimeError, "artifact set"):
            self.validate(3)

    def test_all_schema_pairs_reject_wrong_source_and_symlink(self):
        for schema in (1, 2, 3):
            for path in self.directory.iterdir():
                path.unlink()
            self.candidate(schema)
            for wrong in {1, 2, 3} - {schema}:
                with self.assertRaisesRegex(RuntimeError, "source contract"):
                    self.validate(wrong)
            payload = self.directory / f"atrinik-classic-{self.version}.tar.gz"
            moved = self.root / "payload"
            payload.rename(moved)
            payload.symlink_to(moved)
            with self.assertRaisesRegex(RuntimeError, "artifact set"):
                self.validate(schema)
            moved.unlink()

    def test_schema_three_invokes_package_metadata_validator_and_propagates_failure(self):
        self.candidate(3)
        validator = mock.Mock(side_effect=ValueError("unsafe runtime path"))
        with mock.patch.object(artifacts, "source_schema", return_value=3), mock.patch.object(artifacts, "validate_source_metadata"), mock.patch.object(artifacts, "git_value", return_value=self.revision), mock.patch.object(artifacts, "validate_appimage_metadata", validator):
            with self.assertRaisesRegex(ValueError, "unsafe runtime path"):
                artifacts.validate_candidate(self.directory, "v" + self.version, self.revision)
        self.assertEqual(validator.call_args.args[0], self.directory)
        self.assertEqual(validator.call_args.args[3:], (self.revision, self.version))

    def test_manifest_cannot_select_different_source_contract(self):
        self.candidate(1)
        with self.assertRaisesRegex(RuntimeError, "source contract"):
            self.validate(2)

    def test_checksum_set_rejects_omitted_or_extra_record(self):
        self.candidate(1)
        sums = self.directory / "SHA256SUMS"
        sums.write_text(sums.read_text().splitlines()[0] + "\n")
        with self.assertRaisesRegex(RuntimeError, "checksums differ"):
            self.validate(1)

    def test_contract_is_read_from_revision_generator(self):
        historical = 'def build_release_manifest():\n    return {"schema_version": 1}\n'
        with mock.patch.object(artifacts, "git_value", return_value=historical):
            self.assertEqual(artifacts.source_schema(self.root, self.revision), 1)
        with mock.patch.object(artifacts, "git_value", return_value="RELEASE_ARTIFACT_SCHEMA = 2\n"):
            self.assertEqual(artifacts.source_schema(self.root, self.revision), 2)
        with mock.patch.object(artifacts, "git_value", return_value="RELEASE_ARTIFACT_SCHEMA = 3\n"):
            self.assertEqual(artifacts.source_schema(self.root, self.revision), 3)
        with mock.patch.object(artifacts, "git_value", return_value="RELEASE_ARTIFACT_SCHEMA = 4\n"):
            with self.assertRaises(RuntimeError):
                artifacts.source_schema(self.root, self.revision)

    def test_source_schema_cli_requires_no_candidate_directory(self):
        output = io.StringIO()
        arguments = ["release_artifacts.py", "--print-source-schema", "--revision",
                     self.revision, "--source-root", str(self.root)]
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(artifacts, "source_schema", return_value=2) as schema, redirect_stdout(output):
            self.assertEqual(artifacts.main(), 0)
        schema.assert_called_once_with(self.root, self.revision)
        self.assertEqual(output.getvalue(), "2\n")

    def test_source_metadata_binds_epoch_bundle_and_historical_affects(self):
        inputs = locked_inputs.load_locked_inputs(self.version, schema=1)
        descriptor = json.loads((artifacts.ROOT / "dependencies.bundle.json").read_text())
        manifest = {"source_epoch": 123, "dependency_bundle": descriptor, "locked_inputs": inputs}
        def source(_root, *args):
            if args[0] == "show" and args[1] == "-s":
                return "123"
            path = args[1].split(":", 1)[1]
            return (artifacts.ROOT / path).read_text()
        with mock.patch.object(artifacts, "git_value", side_effect=source):
            artifacts.validate_source_metadata(manifest, self.root, self.revision, self.version, 1)
            with self.assertRaisesRegex(RuntimeError, "locked inputs"):
                artifacts.validate_source_metadata(manifest, self.root, self.revision, self.version, 2)

    def test_source_schema_and_metadata_use_real_immutable_history(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        def git(*arguments):
            return subprocess.run(["git", "-C", str(self.root), *arguments], check=True,
                                  capture_output=True, text=True).stdout.strip()
        git("config", "user.name", "Release fixture")
        git("config", "user.email", "release@example.invalid")
        for relative in ("client/dependencies.lock.json", "server/dependencies.lock.json", "dependencies.bundle.json"):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((artifacts.ROOT / relative).read_bytes())
        generator = self.root / "tools/release/finalize_artifacts.py"
        generator.parent.mkdir(parents=True)
        lock = {"schema_version": 1, "sources": [], "tools": [], "runtime": {}}
        revisions = []
        for schema in (1, 2, 3):
            generator.write_text(f"RELEASE_ARTIFACT_SCHEMA = {schema}\n")
            if schema == 3:
                native = self.root / "tools/ci/appimage/packaging.lock.json"
                native.parent.mkdir(parents=True)
                native.write_text(json.dumps(lock) + "\n")
                self.assertEqual(artifacts.git_value(self.root, "show", f"HEAD:dependencies.bundle.json", preserve_whitespace=True), (self.root / "dependencies.bundle.json").read_text())
            git("add", ".")
            git("commit", "-qm", f"fixture schema {schema}")
            revisions.append(git("rev-parse", "HEAD"))
        # The current checkout advertises schema 3. Older metadata must still
        # bind the source commit's dependency lock and original affected names.
        for schema, revision in enumerate(revisions, start=1):
            self.assertEqual(artifacts.source_schema(self.root, revision), schema)
            manifest = {"source_epoch": int(git("show", "-s", "--format=%ct", revision)),
                        "dependency_bundle": json.loads((self.root / "dependencies.bundle.json").read_text()),
                        "locked_inputs": locked_inputs.load_locked_inputs(self.version, self.root, schema)}
            if schema == 3:
                manifest["appimage_packaging_lock"] = lock
            artifacts.validate_source_metadata(manifest, self.root, revision, self.version, schema)
            manifest["locked_inputs"] = locked_inputs.load_locked_inputs(self.version, self.root, 1 if schema == 3 else 3)
            with self.assertRaisesRegex(RuntimeError, "locked inputs"):
                artifacts.validate_source_metadata(manifest, self.root, revision, self.version, schema)
        manifest["locked_inputs"] = locked_inputs.load_locked_inputs(self.version, self.root, 3)
        manifest["appimage_packaging_lock"] = {**lock, "runtime": {"sha256": "a" * 64}}
        with self.assertRaisesRegex(RuntimeError, "packaging lock"):
            artifacts.validate_source_metadata(manifest, self.root, revisions[2], self.version, 3)

    def test_appimage_inventory_and_spdx_are_reverified_against_exact_source(self):
        image = self.directory / f"atrinik-classic-client-{self.version}-linux-x86_64.AppImage"
        image.write_bytes(b"image")
        lock_text = '{"schema_version": 1}'
        launcher_text = "#!/bin/sh\nexec usr/bin/atrinik\n"
        trusted_files = {"tools/ci/appimage/packaging.lock.json": lock_text,
                         "tools/ci/appimage/AppRun": launcher_text,
                         "tools/ci/appimage/openssl.cnf": "trusted configuration\n",
                         "client/ca-bundle.crt": "trusted certificate bundle\n"}
        inventory = {"schema": 1, "version": self.version, "revision": self.revision,
                     "lock_sha256": hashlib.sha256(lock_text.encode()).hexdigest(),
                     "native_inputs": {"sources": [], "tools": [], "runtime": {
                         "version": "1", "url": "https://example.invalid/runtime", "sha256": "a" * 64,
                         "license": "MIT"}}, "bundled_libraries": [], "bundled_library_records": [], "files": {}}
        manifest = {"source_epoch": 123, "locked_inputs": [], "appimage_inventory": inventory,
                    "appimage_packaging_lock": json.loads(lock_text)}
        sbom = artifacts.build_spdx([image], self.version, self.revision, 123, [], inventory)
        sbom_path = self.directory / f"atrinik-classic-{self.version}.spdx.json"
        sbom_path.write_text(json.dumps(sbom))
        def reader(path, version, *, revision, source_root):
            self.assertEqual(path, image)
            self.assertEqual((version, revision), (self.version, self.revision))
            for relative, content in trusted_files.items():
                self.assertEqual((source_root / relative).read_text(), content)
            return inventory
        def source(root, *args, **kwargs):
            self.assertTrue(kwargs.get("preserve_whitespace"))
            self.assertEqual(root, self.root)
            self.assertTrue(args[1].startswith(self.revision + ":"))
            return trusted_files[args[1].split(":", 1)[1]]
        with mock.patch.object(artifacts, "git_value", side_effect=source), mock.patch.dict(sys.modules, {"appimage": mock.Mock(read_appimage_inventory=reader, TRUSTED_SOURCE_FILES=tuple(trusted_files))}):
            artifacts.validate_appimage_metadata(self.directory, manifest, self.root, self.revision, self.version)
            manifest["appimage_inventory"] = {**inventory, "revision": "d" * 40}
            with self.assertRaisesRegex(RuntimeError, "inventory"):
                artifacts.validate_appimage_metadata(self.directory, manifest, self.root, self.revision, self.version)
            manifest["appimage_inventory"] = inventory
            sbom["packages"][1]["checksums"][0]["checksumValue"] = "e" * 64
            sbom_path.write_text(json.dumps(sbom))
            with self.assertRaisesRegex(RuntimeError, "SPDX"):
                artifacts.validate_appimage_metadata(self.directory, manifest, self.root, self.revision, self.version)

    @unittest.skipUnless(shutil.which("dpkg-deb"), "dpkg-deb required")
    def test_real_deb_metadata_and_payload_validation(self):
        package = self.root / "package"
        control = package / "DEBIAN/control"
        control.parent.mkdir(parents=True)
        control.parent.chmod(0o755)
        valid = ("Package: atrinik\nVersion: 5.99.0\nArchitecture: amd64\nMaintainer: Test <test@example.invalid>\nDescription: Test\n"
                 "Depends: libsdl3-0 (>= 3.4), libsdl3-image0 (>= 3.2), libsdl3-mixer0 (>= 3.2.4), libsdl3-ttf0 (>= 3.2), libssl3t64 (>= 3.5), ca-certificates, libvulkan1\n")
        control.write_text(valid)
        for name in ("usr/games/atrinik", "usr/share/applications/atrinik.desktop",
                     "usr/share/games/atrinik/sound/a", "usr/share/games/atrinik/fonts/a",
                     "usr/share/games/atrinik/textures/a", "usr/share/games/atrinik/client.cfg", "usr/share/doc/atrinik/LICENSE.md", "usr/share/doc/atrinik/ATTRIBUTIONS.md"):
            path = package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"payload")
            path.chmod(0o644)
            for parent in path.parents:
                if parent == package:
                    break
                parent.chmod(0o755)
        (package / "usr/games/atrinik").write_bytes(b"\x7fELF\x02\x01" + b"\0" * 12 + b"\x3e\x00")
        (package / "usr/share/applications/atrinik.desktop").write_text("[Desktop Entry]\nExec=/usr/games/atrinik\n")
        (package / "usr/games/atrinik").chmod(0o755)
        deb = self.root / "client.deb"
        def build():
            result = subprocess.run(["dpkg-deb", "--root-owner-group", "--build", str(package), str(deb)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        build()
        artifacts.validate_deb(deb, self.version)
        for path, mode in ((package / "usr/games", 0o700),
                           (package / "usr/share/games/atrinik/client.cfg", 0o600),
                           (package / "usr/games/atrinik", 0o744)):
            original_mode = path.stat().st_mode & 0o777
            path.chmod(mode)
            build()
            with self.assertRaisesRegex(RuntimeError, "inaccessible|not executable"):
                artifacts.validate_deb(deb, self.version)
            path.chmod(original_mode)
        runtime_config = package / "usr/share/games/atrinik/client.cfg"
        runtime_config.unlink()
        build()
        with self.assertRaisesRegex(RuntimeError, "client.cfg"):
            artifacts.validate_deb(deb, self.version)
        runtime_config.write_text("payload")
        runtime_config.chmod(0o644)
        duplicate = valid.replace(
            "Depends: ",
            "Depends: libsdl3-0 (>= 3.4.0), libssl3t64 (>= 3.5.0), "
            "libsdl3-image0 (>= 3.0), libsdl3-ttf0 (>= 3.0), "
            "libsdl3-mixer0 (>= 3.0), ",
        )
        control.write_text(duplicate)
        build()
        artifacts.validate_deb(deb, self.version)
        for text, error in ((valid.replace("Architecture: amd64", "Architecture: arm64"), "Architecture"),
                            (valid.replace("(>= 3.4)", "(>= 3.2)"), "libsdl3-0"),
                            (duplicate.replace("(>= 3.4.0)", "(>= 3.3.0)").replace("(>= 3.4)", "(>= 3.2)"), "libsdl3-0"),
                            (valid.replace("libssl3t64 (>= 3.5)", "libssl3t64 (>= 3.4)"), "libssl3t64"),
                            (valid.replace("libsdl3-image0 (>= 3.2)", "libsdl3-image0 (>= 3.1)"), "libsdl3-image0"),
                            (valid.replace("libsdl3-ttf0 (>= 3.2)", "libsdl3-ttf0 (>= 3.1)"), "libsdl3-ttf0"),
                            (valid.replace("libsdl3-mixer0 (>= 3.2.4)", "libsdl3-mixer0 (>= 3.2.3)"), "libsdl3-mixer0")):
            control.write_text(text)
            build()
            with self.assertRaisesRegex(RuntimeError, error):
                artifacts.validate_deb(deb, self.version)
        control.write_text(valid)
        bad = package / "usr/share/games/atrinik/private.so"
        bad.write_bytes(b"library")
        bad.chmod(0o644)
        build()
        with self.assertRaisesRegex(RuntimeError, "private library"):
            artifacts.validate_deb(deb, self.version)
        bad.unlink()
        (package / "DEBIAN/postinst").write_text("#!/bin/sh\nexit 0\n")
        (package / "DEBIAN/postinst").chmod(0o755)
        build()
        with self.assertRaisesRegex(RuntimeError, "control files"):
            artifacts.validate_deb(deb, self.version)

    def test_unsafe_tar_members_are_rejected_without_extraction(self):
        for name, kind, mode in (("../escape", tarfile.REGTYPE, 0o644),
                                 ("usr/games/link", tarfile.SYMTYPE, 0o644),
                                 ("usr/games/suid", tarfile.REGTYPE, 0o4755)):
            data = io.BytesIO()
            with tarfile.open(fileobj=data, mode="w") as archive:
                member = tarfile.TarInfo(name)
                member.type, member.mode = kind, mode
                archive.addfile(member)
            data.seek(0)
            with tarfile.open(fileobj=data, mode="r:") as archive, self.assertRaisesRegex(RuntimeError, "unsafe"):
                artifacts.safe_deb_members(archive)


if __name__ == "__main__":
    unittest.main()
