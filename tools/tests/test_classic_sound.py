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
        "background/theme.ogg": _ogg(b"OpusHead\x01\x02\x00\x00\x00\x00\x00\x00"),
        "effects/strike.mid": _ogg(b"\x01vorbis\x00\x00\x00\x00"),
        "effects/wind.flac": _ogg(b"\x01vorbis\x00\x00\x00\x00"),
    }
    assets = []
    for logical_path, payload in payloads.items():
        path = root / logical_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        assets.append({"logical_path": logical_path, "output": {
            "codec": "opus" if logical_path.startswith("background/") else "vorbis",
            "container": "ogg", "sha256": _digest(payload), "size_bytes": len(payload),
        }})
    manifest = {
        "source_commit": commit, "release_tag": tag, "publishable": True,
        "playtest_only": False, "assets": assets, "logical_path_count": len(assets),
        "copied_vorbis_count": 2, "converted_opus_count": 1,
    }
    manifest_path = root / "classic-runtime-manifest.json"
    manifest_path.write_text(json.dumps(manifest, sort_keys=True) + "\n", encoding="utf-8")
    checksum_path = root / "SHA256SUMS"
    checksum_path.write_text("".join(
        f"{_digest((root / path).read_bytes())}  {path}\n"
        for path in sorted([*payloads, manifest_path.name])
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
        if self.root.exists():
            shutil.rmtree(self.root)
        payloads = {
            "background/theme.ogg": _ogg(b"OpusHead\x01\x02\x00\x00\x00\x00\x00\x00"),
            "effects/strike.mid": _ogg(b"\x01vorbis\x00\x00\x00\x00"),
            "effects/wind.flac": _ogg(b"\x01vorbis\x00\x00\x00\x00"),
        }
        assets = []
        for logical_path, payload in payloads.items():
            path = self.root / logical_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
            codec = "opus" if logical_path.startswith("background/") else "vorbis"
            assets.append({
                "logical_path": logical_path,
                "output": {
                    "codec": codec,
                    "container": "ogg",
                    "sha256": _digest(payload),
                    "size_bytes": len(payload),
                },
            })
        manifest = {
            "source_commit": self.dependency["commit"],
            "release_tag": self.dependency["tag"],
            "publishable": True,
            "playtest_only": False,
            "assets": assets,
            "logical_path_count": len(assets),
            "copied_vorbis_count": 2,
            "converted_opus_count": 1,
        }
        manifest_path = self.root / "classic-runtime-manifest.json"
        manifest_path.write_text(json.dumps(manifest, sort_keys=True) + "\n", encoding="utf-8")
        checksum_paths = sorted([*payloads, manifest_path.name])
        checksums = "".join(
            f"{_digest((self.root / path).read_bytes())}  {path}\n" for path in checksum_paths
        )
        checksum_path = self.root / "SHA256SUMS"
        checksum_path.write_text(checksums, encoding="utf-8")
        self.dependency["runtime_manifest_sha256"] = _digest(manifest_path.read_bytes())
        self.dependency["runtime_checksums_sha256"] = _digest(checksum_path.read_bytes())
        return manifest

    def assert_rejected(self, pattern: str = "") -> None:
        with self.assertRaisesRegex(Exception, pattern):
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

    def test_accepts_closed_two_codec_tree_and_returns_counts(self) -> None:
        self.make_tree()
        self.assertEqual(classic_sound.verify_tree(self.root, self.dependency), {
            "logical_path_count": 3,
            "copied_vorbis_count": 2,
            "converted_opus_count": 1,
            "verified_file_count": 4,
        })

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
        payload.write_bytes(_ogg(b"OpusHead\x01\x02\x00\x00\x00\x00\x00\x00"))
        manifest["assets"][1]["output"]["sha256"] = _digest(payload.read_bytes())
        manifest["assets"][1]["output"]["size_bytes"] = payload.stat().st_size
        manifest_path = self.root / "classic-runtime-manifest.json"
        manifest_path.write_text(json.dumps(manifest, sort_keys=True) + "\n", encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("manifest")

    def test_rejects_missing_and_unlisted_payloads(self) -> None:
        self.make_tree()
        (self.root / "effects/wind.flac").unlink()
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
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("Ogg|codec|header")
        self.make_tree()
        (self.root / "effects/wind.flac").write_bytes(b"fLaC\x00\x00\x00\x22")
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["assets"][2]["output"]["sha256"] = _digest((self.root / "effects/wind.flac").read_bytes())
        manifest["assets"][2]["output"]["size_bytes"] = 8
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
        manifest["assets"][0]["output"]["codec"] = "vorbis"
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("codec")
        self.make_tree()
        payload = self.root / "background/theme.ogg"
        payload.write_bytes(b"OggS\x00")
        manifest = json.loads((self.root / "classic-runtime-manifest.json").read_text())
        manifest["assets"][0]["output"]["sha256"] = _digest(payload.read_bytes())
        manifest["assets"][0]["output"]["size_bytes"] = payload.stat().st_size
        (self.root / "classic-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reseal_metadata()
        self.assert_rejected("Ogg|codec|header")


if __name__ == "__main__":
    unittest.main()
