from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


CLIENT_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = CLIENT_ROOT / "tools/benchmark_shadow_prototypes.py"
FIXTURE = CLIENT_ROOT / "src/tests/fixtures/player_view/shadow-prototypes.json"
SPEC = importlib.util.spec_from_file_location("benchmark_shadow_prototypes", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
prototype = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = prototype
SPEC.loader.exec_module(prototype)


class ShadowPrototypeTests(unittest.TestCase):
    def test_matrix_is_bounded_and_covers_both_scenes(self) -> None:
        evidence = prototype.measure(FIXTURE)
        self.assertTrue(evidence["matrix"]["identical_scene_inputs"])
        self.assertFalse(evidence["matrix"]["production_direction_input_available"])
        self.assertEqual(len(evidence["results"]), 60)
        self.assertEqual(len(evidence["matrix"]["light_cases"]), 10)
        for result in evidence["results"]:
            metrics = result["metrics"]
            self.assertEqual(metrics["idle_nonzero_frames"], 0)
            self.assertLessEqual(metrics["frame_p95_work_units"], 12000)
            self.assertEqual(len(metrics["checkpoints_sha256"]), 5)
            self.assertIn("rgb", result["light_case_input"])
            if result["light_case"] == "negative-light":
                self.assertEqual(result["light_case_input"]["effective_scalar"], 0)

    def test_hidden_casters_and_closed_boundaries_do_not_leak(self) -> None:
        _, scenes = prototype._parse_fixture(FIXTURE)
        scene = scenes[0]
        positions = prototype._frame_positions(scene, 0, [{"x": 0, "y": 0}])
        mask, _ = prototype._contact(scene, positions, {(28, 28), (24, 20)}, 192)
        self.assertEqual(mask[prototype._idx(scene, 28, 28)], 0)
        self.assertEqual(mask[prototype._idx(scene, 24, 20)], 0)

    def test_fixture_rejects_wrong_movement_length(self) -> None:
        data = FIXTURE.read_text(encoding="utf-8").replace('"movement_frames": 480', '"movement_frames": 479')
        with tempfile.TemporaryDirectory() as directory:
            with tempfile.NamedTemporaryFile(
                mode="w",
                suffix=".json",
                encoding="utf-8",
                dir=directory,
                delete=False,
            ) as handle:
                handle.write(data)
                handle.flush()
                path = Path(handle.name)
            with self.assertRaisesRegex(prototype.PrototypeError, "480-frame"):
                prototype.measure(path)
        self.assertFalse(path.exists())
        self.assertFalse(Path(directory).exists())


if __name__ == "__main__":
    unittest.main()
