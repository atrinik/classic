from __future__ import annotations

import io
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
        with mock.patch.object(artifacts, "source_schema", return_value=schema), mock.patch.object(artifacts, "validate_source_metadata"), mock.patch.object(artifacts, "validate_deb"), mock.patch.object(artifacts, "git_value", return_value=self.revision):
            return artifacts.validate_candidate(self.directory, "v" + self.version, self.revision)

    def test_historical_schema_one_retains_exact_twelve_without_deb(self):
        self.candidate(1)
        self.assertEqual(len(self.validate(1)), 12)
        (self.directory / f"atrinik-classic-client-{self.version}-linux-amd64.deb").write_bytes(b"extra")
        with self.assertRaisesRegex(RuntimeError, "artifact set"):
            self.validate(1)

    def test_new_schema_requires_deb_and_rejects_tamper(self):
        self.candidate(2)
        self.assertEqual(len(self.validate(2)), 13)
        deb = self.directory / f"atrinik-classic-client-{self.version}-linux-amd64.deb"
        deb.write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "hash/size mismatch"):
            self.validate(2)
        deb.unlink()
        with self.assertRaisesRegex(RuntimeError, "artifact set"):
            self.validate(2)

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
        inputs = locked_inputs.load_locked_inputs(self.version)
        for record in inputs:
            record["affects"] = [n for n in record["affects"] if not n.endswith(".deb")]
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
        (package / "usr/games/atrinik").write_bytes(b"\x7fELF\x02\x01" + b"\0" * 12 + b"\x3e\x00")
        (package / "usr/share/applications/atrinik.desktop").write_text("[Desktop Entry]\nExec=/usr/games/atrinik\n")
        (package / "usr/games/atrinik").chmod(0o755)
        deb = self.root / "client.deb"
        def build():
            result = subprocess.run(["dpkg-deb", "--root-owner-group", "--build", str(package), str(deb)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        build()
        artifacts.validate_deb(deb, self.version)
        runtime_config = package / "usr/share/games/atrinik/client.cfg"
        runtime_config.unlink()
        build()
        with self.assertRaisesRegex(RuntimeError, "client.cfg"):
            artifacts.validate_deb(deb, self.version)
        runtime_config.write_text("payload")
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
