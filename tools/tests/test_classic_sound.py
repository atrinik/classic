"""Regression coverage for the pinned Classic runtime-sound tree."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "classic_sound.py"
SPEC = importlib.util.spec_from_file_location("atrinik_classic_sound", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
classic_sound = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(classic_sound)


def _digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _ogg(first_packet: bytes) -> bytes:
    """Return one valid-enough first Ogg page for header-level verification."""
    if len(first_packet) > 255:
        raise AssertionError("fixture packet needs multiple lacing values")
    return (
        b"OggS\x00\x02" + b"\x00" * 8 + b"\x01\x00\x00\x00"
        + b"\x00" * 8 + b"\x01" + bytes((len(first_packet),)) + first_packet
    )


def make_tree(root: Path, commit: str = "a" * 40, tag: str = "v1.0.0") -> dict[str, object]:
    """Build a tiny, closed runtime-sound fixture and return its lock fields."""
    if root.exists():
        shutil.rmtree(root)
    payloads = {
        "background/theme.ogg": _ogg(b"\x01vorbis\x00\x00\x00\x00"),
        "effects/strike.mid": _ogg(b"OpusHead\x01\x02\x00\x00\x00\x00\x00\x00"),
        "effects/wind.ogg": _ogg(b"OpusHead\x01\x02\x00\x00\x00\x00\x00\x00"),
    }
    sources = {
        "background/theme.ogg": {"codec": "vorbis", "container": "ogg", "sha256": _digest(payloads["background/theme.ogg"])},
        "effects/strike.mid": {"codec": "midi", "container": "standard-midi-file", "sha256": "1" * 64},
        "effects/wind.ogg": {"codec": "flac", "container": "flac", "sha256": "2" * 64},
    }
    assets = []
    for logical_path, payload in payloads.items():
        path = root / logical_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        source = sources[logical_path]
        assets.append({
            "logical_path": logical_path,
            "source_path": "effects/replacements/wind.flac" if logical_path == "effects/wind.ogg" else logical_path,
            "mapping": "copy" if source["codec"] == "vorbis" else "render-opus",
            "source": source,
            "output": {
                "channels": 2, "codec": "vorbis" if source["codec"] == "vorbis" else "opus",
                "container": "ogg", "duration_seconds": 1.0, "sample_rate": 48000,
                "sha256": _digest(payload), "size_bytes": len(payload),
            },
        })
    schema_path = root / "schemas/classic-runtime-manifest-v1.schema.json"
    schema_path.parent.mkdir(parents=True)
    schema_path.write_text(json.dumps({
        "$id": "https://atrinik.org/schemas/sound/classic-runtime-manifest-v1.schema.json",
    }) + "\n", encoding="utf-8")
    source_manifest_path = root / "manifests/source-assets.json"
    source_manifest_path.parent.mkdir(parents=True)
    source_manifest = {
        "$schema": "../schemas/source-assets-v1.schema.json", "schema_version": 1,
        "assets": [{
            "logical_path": asset["logical_path"], "source_path": asset["source_path"],
            "source": {**asset["source"], "channels": 2, "sample_rate": 48000,
                       "duration_seconds": 1.0},
        } for asset in assets],
        "audio_source_count": len(assets), "source_corpus_sha256": _digest((
            json.dumps([
                {"source_path": asset["source_path"], "sha256": asset["source"]["sha256"]}
                for asset in assets
            ], sort_keys=True, indent=2) + "\n"
        ).encode()),
        "source_size_bytes": 123,
    }
    source_manifest_path.write_text(json.dumps(source_manifest, sort_keys=True) + "\n", encoding="utf-8")
    toolchain_path = root / "manifests/classic-audio-toolchain.json"
    license_path = root / "licenses/FIXTURE.txt"
    license_path.parent.mkdir()
    license_path.write_text("fixture license\n", encoding="utf-8")
    toolchain_path.write_text(json.dumps({
        "$schema": "../schemas/classic-audio-toolchain-v1.schema.json", "schema_version": 1,
        "license_texts": {"fixture": {
            "archive_path": "licenses/FIXTURE.txt", "sha256": _digest(license_path.read_bytes()),
        }},
    }, sort_keys=True) + "\n", encoding="utf-8")
    remediation_path = root / "classic-runtime-remediation.json"
    remediation_path.write_text(json.dumps({
        "$schema": "schemas/classic-remediation-v1.schema.json", "schema_version": 1,
        "source_commit": commit, "source_tree": "b" * 40,
        "source_manifest_sha256": _digest(source_manifest_path.read_bytes()),
        "source_count": len(assets), "classification": "nonblocking-modernization",
        "count": 0, "category_counts": {}, "findings": [],
        "release_boundary": "fixture modernization finding boundary",
    }, sort_keys=True) + "\n", encoding="utf-8")
    output_tree = _digest("".join(
        f"{asset['output']['sha256']}  {asset['logical_path']}\n"
        for asset in sorted(assets, key=lambda item: item["logical_path"])
    ).encode())
    manifest = {
        "$schema": "schemas/classic-runtime-manifest-v1.schema.json", "schema_version": 1,
        "source_commit": commit, "source_tree": "b" * 40, "release_tag": tag,
        "publishable": True, "playtest_only": False, "assets": assets,
        "source_manifest_sha256": _digest(source_manifest_path.read_bytes()),
        "toolchain_sha256": _digest(toolchain_path.read_bytes()),
        "schema_sha256": _digest(schema_path.read_bytes()),
        "remediation_report_sha256": _digest(remediation_path.read_bytes()),
        "remediation_finding_count": 0,
        "tool_versions": {name: "fixture 1" for name in (
            "ffmpeg", "wildmidi", "openmpt123", "opusenc", "opusinfo", "sdl3_mixer_probe",
        )},
        "logical_path_count": len(assets), "copied_vorbis_count": 1,
        "converted_opus_count": 2, "output_tree_sha256": output_tree,
    }
    manifest_path = root / "classic-runtime-manifest.json"
    manifest_path.write_text(json.dumps(manifest, sort_keys=True) + "\n", encoding="utf-8")
    checksum_path = root / "SHA256SUMS"
    checksum_path.write_text("".join(
        f"{_digest((root / path).read_bytes())}  {path}\n"
        for path in sorted(
            [*payloads, manifest_path.name, schema_path.relative_to(root).as_posix(),
             source_manifest_path.relative_to(root).as_posix(),
             toolchain_path.relative_to(root).as_posix(), remediation_path.name,
             license_path.relative_to(root).as_posix()]
        )
    ), encoding="utf-8")
    return {"name": "sound", "commit": commit, "tag": tag,
            "runtime_manifest_sha256": _digest(manifest_path.read_bytes()),
            "runtime_checksums_sha256": _digest(checksum_path.read_bytes())}


class ClassicSoundTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "sound"
        self.dependency = {
            "name": "sound",
            "commit": "a" * 40,
            "tag": "v1.2.3",
            "runtime_manifest_sha256": "",
            "runtime_checksums_sha256": "",
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def make_tree(self) -> dict[str, object]:
        self.dependency = make_tree(
            self.root, commit=str(self.dependency["commit"]), tag=str(self.dependency["tag"])
        )
        return json.loads((self.root / "classic-runtime-manifest.json").read_text())

    def assert_rejected(self, pattern: str = "") -> None:
        with self.assertRaisesRegex(ValueError, pattern):
            classic_sound.verify_tree(self.root, self.dependency)

    def reseal_metadata(self) -> None:
        """Simulate an attacker who can rewrite both mutable metadata files."""
        paths = sorted(
            path.relative_to(self.root).as_posix()
            for path in self.root.rglob("*")
            if path.is_file() and path.name not in {"SHA256SUMS", ".atrinik-dependency.json"}
        )
        checksums = "".join(
            f"{_digest((self.root / path).read_bytes())}  {path}\n" for path in paths
        )
        checksum_path = self.root / "SHA256SUMS"
        checksum_path.write_text(checksums, encoding="utf-8")
        self.dependency["runtime_manifest_sha256"] = _digest(
            (self.root / "classic-runtime-manifest.json").read_bytes()
        )
        self.dependency["runtime_checksums_sha256"] = _digest(checksum_path.read_bytes())

    @staticmethod
    def refresh_output_tree(manifest: dict[str, object]) -> None:
        assets = manifest["assets"]
        assert isinstance(assets, list)
        manifest["output_tree_sha256"] = _digest("".join(
            f"{asset['output']['sha256']}  {asset['logical_path']}\n"
            for asset in sorted(assets, key=lambda item: item["logical_path"])
        ).encode())

    def test_accepts_closed_two_codec_tree_and_returns_counts(self) -> None:
        self.make_tree()
        self.assertEqual(classic_sound.verify_tree(self.root, self.dependency), {
            "logical_path_count": 3,
            "copied_vorbis_count": 1,
            "converted_opus_count": 2,
            "verified_file_count": 9,
        })

    def test_malformed_codec_types_and_huge_duration_are_validation_errors(self) -> None:
        for field, value in (("codec", []), ("duration_seconds", 10**1000)):
            with self.subTest(field=field):
                manifest = self.make_tree()
                manifest["assets"][0]["output"][field] = value
                (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest))
                self.reseal_metadata()
                self.assert_rejected("identity or format")
        manifest = self.make_tree()
        source_path = self.root / "manifests/source-assets.json"
        source = json.loads(source_path.read_text())
        source["assets"][0]["source"]["codec"] = []
        source_path.write_text(json.dumps(source))
        manifest["source_manifest_sha256"] = _digest(source_path.read_bytes())
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest))
        self.reseal_metadata()
        self.assert_rejected("source asset identity or codec")

    def test_rejects_wrong_or_forged_pinned_manifest_and_checksums(self) -> None:
        self.make_tree()
        self.dependency["runtime_manifest_sha256"] = "0" * 64
        self.assert_rejected("manifest")
        self.dependency["runtime_manifest_sha256"] = _digest(
            (self.root / "classic-runtime-manifest.json").read_bytes()
        )
        self.dependency["runtime_checksums_sha256"] = "f" * 64
        self.assert_rejected("SHA256SUMS")

    def test_rejects_mutated_payload_even_after_manifest_and_checksums_reseal(self) -> None:
        manifest = self.make_tree()
        payload = self.root / "effects/strike.mid"
        payload.write_bytes(_ogg(b"\x01vorbis\x00\x00\x00\x00"))
        manifest["assets"][1]["output"]["sha256"] = _digest(payload.read_bytes())
        manifest["assets"][1]["output"]["size_bytes"] = payload.stat().st_size
        self.refresh_output_tree(manifest)
        manifest_path = self.root / "classic-runtime-manifest.json"
        manifest_path.write_text(json.dumps(manifest, sort_keys=True) + "\n", encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("manifest")

    def test_rejects_missing_and_unlisted_payloads(self) -> None:
        self.make_tree()
        (self.root / "effects/wind.ogg").unlink()
        self.assert_rejected("closed file set")
        self.make_tree()
        (self.root / "effects/extra.ogg").write_bytes(_ogg(b"\x01vorbis\x00"))
        self.assert_rejected("pinned inventory")

    def test_rejects_raw_midi_and_flac_disguised_as_retained_logical_paths(self) -> None:
        self.make_tree()
        (self.root / "effects/strike.mid").write_bytes(b"MThd\x00\x00\x00\x06")
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["assets"][1]["output"]["sha256"] = _digest((self.root / "effects/strike.mid").read_bytes())
        manifest["assets"][1]["output"]["size_bytes"] = 8
        self.refresh_output_tree(manifest)
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("Ogg|codec|header")
        self.make_tree()
        (self.root / "effects/wind.ogg").write_bytes(b"fLaC\x00\x00\x00\x22")
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["assets"][2]["output"]["sha256"] = _digest((self.root / "effects/wind.ogg").read_bytes())
        manifest["assets"][2]["output"]["size_bytes"] = 8
        self.refresh_output_tree(manifest)
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("Ogg|codec|header")

    def test_rejects_unsafe_paths_symlinks_and_case_collisions(self) -> None:
        self.make_tree()
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["assets"][0]["logical_path"] = "../escape.ogg"
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("safe|path|traversal")
        self.make_tree()
        (self.root / "effects/link.ogg").symlink_to(self.root / "background/theme.ogg")
        self.assert_rejected("link|regular")
        self.make_tree()
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        duplicate = dict(manifest["assets"][0])
        duplicate["logical_path"] = "BACKGROUND/theme.ogg"
        manifest["assets"].append(duplicate)
        manifest["logical_path_count"] += 1
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("identity or format")

    def test_rejects_manifest_policy_count_and_codec_contract_violations(self) -> None:
        manifest = self.make_tree()
        manifest["publishable"] = False
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("publication/source identity")
        self.make_tree()
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["converted_opus_count"] = 0
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("count")

    def test_rejects_forged_publication_identity_and_wrong_or_malformed_codec(self) -> None:
        manifest = self.make_tree()
        manifest["release_tag"] = "v9.9.9"
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("publication|identity")
        self.make_tree()
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["assets"][0]["output"]["codec"] = "opus"
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("Vorbis copy|codec")
        self.make_tree()
        payload = self.root / "effects/strike.mid"
        payload.write_bytes(b"OggS\x00")
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["assets"][1]["output"]["sha256"] = _digest(payload.read_bytes())
        manifest["assets"][1]["output"]["size_bytes"] = payload.stat().st_size
        self.refresh_output_tree(manifest)
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("Ogg|codec|header")

    def test_rejects_schema_links_output_tree_mapping_and_source_corpus_reseals(self) -> None:
        manifest = self.make_tree()
        manifest["schema_version"] = 999
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("schema")
        self.make_tree()
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["source_manifest_sha256"] = "0" * 64
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("source|linked")
        self.make_tree()
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["output_tree_sha256"] = "0" * 64
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("logical-tree")
        self.make_tree()
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["assets"][1]["mapping"] = "copy"
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("identity or format")
        self.make_tree()
        source_path = self.root / "manifests/source-assets.json"
        source_manifest = json.loads(source_path.read_text())
        source_manifest["assets"].pop()
        source_manifest["audio_source_count"] -= 1
        source_path.write_text(json.dumps(source_manifest), encoding="utf-8")
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["source_manifest_sha256"] = _digest(source_path.read_bytes())
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("source|closure|count")


if __name__ == "__main__":
    unittest.main()
