from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
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
