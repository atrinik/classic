from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


CLIENT_ROOT = Path(__file__).resolve().parents[2]
VERIFIER_PATH = CLIENT_ROOT / "tools/verify_gpu_fixture_bytes.py"
SPEC = importlib.util.spec_from_file_location("verify_gpu_fixture_bytes", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)


EXPECTED = {
    "data/interface.cfg": "cb3bccde141390f8e016a988f1c3b8c13fcf72f54d0e1ef4305cd7716f0a2302",
    "src/tests/fixtures/player_view/gpu-interface.cfg": "8f2fc77fead14655d039c9cd7e52c6767e942603eb06646f427f87d25cb8a3d2",
    "src/tests/fixtures/player_view/gpu-benchmark-17x17-five-depth.map2.hex": "eef02084f57ef9bf8e511a3019a3ae911efa0c2808cd93bf3e4ba0919c17b0ef",
    "src/tests/fixtures/player_view/gpu-benchmark-28x28-thirteen-depth.map2.hex": "ee0d4de99741ec050ecb46a609f747726d22f371f584fa536ba20a26dc8476c8",
}


class GPUFixtureByteTests(unittest.TestCase):
    def test_source_inputs_match_the_issue_contract(self) -> None:
        inputs = VERIFIER.validate_source(CLIENT_ROOT)
        self.assertGreater(len(inputs), len(EXPECTED))
        for relative, expected in EXPECTED.items():
            self.assertEqual(inputs[relative], expected, relative)

    def test_staged_package_inputs_match_source_bytes(self) -> None:
        source_inputs = VERIFIER.validate_source(CLIENT_ROOT)
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            data = package / "data"
            data.mkdir()
            for name in ("archdef.dat", "interface.cfg"):
                source = CLIENT_ROOT / "data" / name
                (data / name).write_bytes(source.read_bytes())
            VERIFIER.validate_package(package, source_inputs)
            interface = data / "interface.cfg"
            interface.write_bytes(interface.read_bytes() + b"\r")
            with self.assertRaisesRegex(VERIFIER.FixtureError, "carriage-return"):
                VERIFIER.validate_package(package, source_inputs)

    def test_source_verifier_rejects_carriage_returns_in_pinned_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            client = Path(temporary)
            fixture_root = client / "src" / "tests" / "fixtures" / "player_view"
            fixture_root.mkdir(parents=True)
            data = client / "data"
            data.mkdir()
            payload = b"portable\n"
            (data / "interface.cfg").write_bytes(payload.replace(b"\n", b"\r\n"))
            digest = hashlib.sha256(payload).hexdigest()
            manifest = (
                f'<fixture renderer="gpu" input-root="../../../.." '
                f'interface="data/interface.cfg" interface-sha256="{digest}" />\n'
            ).encode("ascii")
            (fixture_root / "minimal.xml").write_bytes(manifest)
            with self.assertRaisesRegex(VERIFIER.FixtureError, "carriage-return"):
                VERIFIER.validate_source(client)


if __name__ == "__main__":
    unittest.main()
