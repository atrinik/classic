from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CLIENT_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = CLIENT_ROOT / "src/tests/fixtures/player_view"


class MovementFixtureTests(unittest.TestCase):
    def test_generated_five_depth_snapshot_and_delta_are_pinned(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            generated_snapshot = temporary_path / "snapshot.hex"
            generated_delta = temporary_path / "delta.hex"
            subprocess.run(
                [
                    sys.executable,
                    str(CLIENT_ROOT / "tools/generate_movement_five_depth.py"),
                    str(FIXTURES / "colored-scene.map2.hex"),
                    str(generated_snapshot),
                ],
                check=True,
            )
            subprocess.run(
                [
                    sys.executable,
                    str(CLIENT_ROOT / "tools/generate_movement_delta.py"),
                    str(generated_delta),
                ],
                check=True,
            )
            for generated, pinned in (
                (generated_snapshot, FIXTURES / "movement-colored-five-depth.map2.hex"),
                (generated_delta, FIXTURES / "movement-colored-delta.map2.hex"),
            ):
                self.assertEqual(
                    hashlib.sha256(generated.read_bytes()).digest(),
                    hashlib.sha256(pinned.read_bytes()).digest(),
                )


if __name__ == "__main__":
    unittest.main()
