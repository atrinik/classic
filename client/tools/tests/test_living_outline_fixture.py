from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest


CLIENT_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = CLIENT_ROOT / "src/tests/fixtures/player_view"
GENERATOR_PATH = CLIENT_ROOT / "tools/generate_living_outline_fixtures.py"
SPEC = importlib.util.spec_from_file_location("generate_living_outline_fixtures", GENERATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class LivingOutlineFixtureTests(unittest.TestCase):
    def test_visibility_retention_has_unchanged_static_neighbors(self) -> None:
        original = GENERATOR.centered_visibility_fade()
        retained = GENERATOR.centered_visibility_fade(retained_background=True)
        header_size = len(GENERATOR.packet(((0, b""),)))
        payload = retained[header_size:]
        center_payload = original[header_size:]
        self.assertEqual(payload[:len(center_payload)], center_payload)
        cursor = 0
        records = {}
        while cursor < len(payload):
            mask, radiance, count = struct.unpack_from(">HHB", payload, cursor)
            cursor += 5
            self.assertEqual(mask & 63, 4)
            self.assertEqual(radiance, 2048)
            coordinates = (mask >> 11, (mask >> 6) & 31)
            self.assertNotIn(coordinates, records)
            layers = []
            for _ in range(count):
                layer, face, flags, extra = struct.unpack_from(">BHBB", payload, cursor)
                cursor += 5
                self.assertEqual((flags, extra), (0, 0))
                layers.append((layer, face))
            self.assertEqual(payload[cursor], 0)
            cursor += 1
            records[coordinates] = layers
        self.assertEqual(cursor, len(payload))
        self.assertEqual(set(records), {(x, y) for x in range(13) for y in range(13)})
        self.assertEqual(records.pop((6, 6)), [(0, 1), (2, 2), (5, 4), (6, 3)])
        self.assertEqual(len(records), 168)
        self.assertTrue(all(layers == [(0, 1)] for layers in records.values()))

    def test_generated_scenes_are_pinned(self) -> None:
        for name, expected in GENERATOR.scenes().items():
            pinned = FIXTURES / f"{name}.map2.hex"
            self.assertEqual(pinned.read_bytes(), expected.hex().encode("ascii") + b"\n", name)

    def test_generator_writes_only_named_scenes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            subprocess.run(
                [sys.executable, str(GENERATOR_PATH), str(output)],
                check=True,
            )
            self.assertEqual(
                {path.name for path in output.iterdir()},
                {f"{name}.map2.hex" for name in GENERATOR.scenes()},
            )
            for name, expected in GENERATOR.scenes().items():
                self.assertEqual(
                    (output / f"{name}.map2.hex").read_bytes(),
                    expected.hex().encode("ascii") + b"\n",
                    name,
                )


if __name__ == "__main__":
    unittest.main()
