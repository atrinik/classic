import copy
import hashlib
import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from tools.verify_gpu_qualification import (
    ArtifactError,
    LIFECYCLE_EVENTS,
    UI_CLOSURE_STATES,
    WORKLOAD_CONTRACTS,
    WORKLOADS,
    _lifecycle_fullscreen_golden,
    _require_ui_screenshot_crop,
    _review_artifact,
    validate,
    validate_lifecycle_record,
    validate_production_record,
    validate_record,
)


def png(width: int, height: int) -> bytes:
    def chunk(name: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + name + data +
                struct.pack(">I", zlib.crc32(name + data)))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    pixels = b"".join(b"\0" + b"\0\0\0\xff" * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def record(name="dense-17x17-five-depth-1080p"):
    stages = {
        stage: [1000] * 40
        for stage in (
            "command_build", "albedo_owner", "light_tone", "ui",
            "cpu_submission", "gpu_completion_wait", "present_wait"
        )
    }
    logical, width, height, depths, animation, actors, *_ = WORKLOAD_CONTRACTS[name]
    frames = [10_000] * 40
    depth_mask = (1 << depths) - 1
    return {
        "schema_version": 3,
        "benchmark": "gpu-interop-stress-qualification",
        "revision": "1" * 40,
        "dirty": False,
        "gpu": {"backend": "vulkan", "device": "device", "driver_name": "driver",
                "driver_version": "1", "hardware_tier": "reference",
                "qualified_hardware": True},
        "build": {"type": "Release", "compiler": "GNU 15", "system": "Linux x86_64"},
        "shader_cohort": "2" * 64,
        "workload": {"name": name, "logical_view": logical,
                     "window_logical_width": width, "window_logical_height": height,
                     "render_output_width": width, "render_output_height": height,
                     "active_depths": depths, "animation_only": animation,
                     "actors": actors, "production_path": True, "stretch_exercised": True,
                     "animation_path_verified": animation,
                     "redraw_reason_frames": 40 if animation else 0,
                     "animated_command_transitions": 1 if animation else 0,
                     "command_depth_mask": depth_mask, "living_depth_mask": 1,
                     "door_depth_mask": 1 if animation else 0,
                     "roof_depth_mask": depth_mask if animation else 0,
                     "frames_with_stretch": 40, "frames_with_living": 40,
                     "frames_with_door": 40 if animation else 0,
                     "frames_with_roof": 40 if animation else 0},
        "cold": {"scope": "fresh-process", "uploads": 2, "upload_bytes": 20,
                 "source_uploads": 1, "source_upload_bytes": 4,
                 "instance_uploads": 0, "instance_upload_bytes": 0,
                 "light_uploads": 0, "light_upload_bytes": 0,
                 "slot_uniform_uploads": 1, "slot_uniform_upload_bytes": 16,
                 "resource_creations": 1, "peak_retained_bytes": 4},
        "frame_windows_ns": frames,
        "frame_ns": {"p95": 10_000, "p99": 10_000, "max": 10_000},
        "stretch_frame_windows_ns": frames,
        "stage_windows_ns": stages,
        "checkpoint": {"algorithm": "sha256-rgba32-with-dimensions", "timed": False,
                       "pixels_sha256": "3" * 64,
                       "animation_pixels_sha256":
                           ["4" * 64, "5" * 64, "4" * 64] if animation else ["", "", ""]},
        "steady_state": {"uploads": 36,
                         "upload_bytes": 576,
                         "source_uploads": 0, "source_upload_bytes": 0,
                         "instance_uploads": 0,
                         "instance_upload_bytes": 0,
                         "light_uploads": 0, "light_upload_bytes": 0,
                         "slot_uniform_uploads": 36, "slot_uniform_upload_bytes": 576,
                         "resource_creations": 0,
                         "resource_destructions": 0, "readbacks": 0, "commands": 100,
                         "batches": 80, "draws": 80, "retained_bytes": 4,
                         "peak_retained_bytes": 4, "fallbacks": 0},
    }


class VerifyGpuQualificationTests(unittest.TestCase):
    def test_fullscreen_golden_is_keyed_by_tier_and_display_mode(self):
        contract = {"backends": {"vulkan": {"brynknot-movement": {
            "fullscreen": {
                "reference": {"1920x1080": "a" * 64},
                "minimum": {"1280x720": "b" * 64},
            }
        }}}}
        lifecycle = {"gpu": {"backend": "vulkan", "hardware_tier": "reference"}}
        event = {"display_mode_size": [1920, 1080]}
        self.assertEqual(_lifecycle_fullscreen_golden(contract, lifecycle, event), "a" * 64)
        lifecycle["gpu"]["hardware_tier"] = "minimum"
        self.assertIsNone(_lifecycle_fullscreen_golden(contract, lifecycle, event))
        event["display_mode_size"] = [1280, 720]
        self.assertEqual(_lifecycle_fullscreen_golden(contract, lifecycle, event), "b" * 64)

    def test_valid_production_records(self):
        value = {
            "schema_version": 2,
            "renderer": "gpu-production-player-view",
            "fixture": "brynknot-movement",
            "revision": "1" * 40,
            "dirty": False,
            "manifest_sha256": "2" * 64,
            "snapshot_sha256": "3" * 64,
            "pixels_sha256": "4" * 64,
            "golden_verified": True,
            "movement_lifecycle": True,
            "logical_view": [17, 17],
            "viewport": [1024, 780],
            "ui_pixels_sha256": "",
            "assertions": {"ui_names_targets": False, "visibility_fade": False,
                           "map_interaction": False, "damage_animation": False,
                           "kill_animation": False, "elevated_animation": False,
                           "layer_content_animation": False},
            "timed_light_lifecycle": False,
            "timed_endpoint_pixels_sha256": "",
            "borrowed_temporal_light_samples": 0,
            "initial_pixels_sha256": "4" * 64,
            "initial_artifact": "review/initial.png",
            "initial_artifact_sha256": "8" * 64,
            "final_artifact": "review/final.png",
            "final_artifact_sha256": "9" * 64,
            "transition_pixels_sha256": "5" * 64,
            "archived_software_pixels_sha256": "",
            "archived_software_ui_pixels_sha256": "",
            "archived_software_lifecycle_sha256": "6" * 64,
            "backend": "vulkan",
            "device": "device",
            "driver_name": "driver",
            "driver_version": "1",
            "hardware_tier": "reference",
            "build": {"type": "Release", "compiler": "GNU 15", "system": "Linux x86_64"},
            "qualified_hardware": True,
            "ui_closure": None,
        }
        self.assertTrue(validate_production_record(value))
        value["transition_pixels_sha256"] = value["initial_pixels_sha256"]
        with self.assertRaisesRegex(ArtifactError, "distinct"):
            validate_production_record(value)
        value["transition_pixels_sha256"] = "5" * 64
        value["movement_lifecycle"] = False
        value["archived_software_lifecycle_sha256"] = ""
        value["fixture"] = "gpu-qualification-town-25x25"
        value["golden_verified"] = False
        value["logical_view"] = [25, 25]
        value["viewport"] = [2560, 1440]
        self.assertFalse(validate_production_record(value))
        value["fixture"] = "gpu-ui-closure"
        value["logical_view"] = [17, 17]
        value["viewport"] = [1024, 780]
        value["ui_pixels_sha256"] = "7" * 64
        value["assertions"]["ui_names_targets"] = True
        states = []
        for index, name in enumerate(UI_CLOSURE_STATES):
            screenshot = name.startswith("screenshot_")
            digest = "a" * 64 if name == "popup_region_map_minimap" else (
                "b" * 64 if name in {"region_map_fow_transition", "region_map_fow_retained"}
                else f"{(index % 8) + 1:x}" * 64)
            states.append({
                "name": name,
                "pixels_sha256": digest,
                "output_size": [768, 585] if name == "screenshot_map" else [1024, 780],
                "command": "/screenshot map" if name == "screenshot_map" else (
                    "/screenshot" if screenshot else ""),
                "asynchronous": screenshot,
                "artifact": f"review/{name}.png",
                "artifact_sha256": "f" * 64,
                "steady_state": {"uploads": 0, "upload_bytes": 0,
                                 "slot_uniform_uploads": 0,
                                 "slot_uniform_upload_bytes": 0,
                                 "resource_creations": 0, "resource_destructions": 0,
                                 "readbacks": 1 if screenshot else 0, "fallbacks": 0},
            })
        value["ui_closure"] = states
        self.assertFalse(validate_production_record(value))
        for dimension in range(2):
            with self.subTest(dimension=dimension):
                invalid = copy.deepcopy(value)
                invalid["ui_closure"][0]["output_size"][dimension] -= 1
                with self.assertRaisesRegex(ArtifactError, "checkpoint size"):
                    validate_production_record(invalid)
        invalid = copy.deepcopy(value)
        by_name = {state["name"]: state for state in invalid["ui_closure"]}
        by_name["popup_character_selection"]["pixels_sha256"] = \
            by_name["gameplay_widgets_text_windows"]["pixels_sha256"]
        with self.assertRaisesRegex(ArtifactError, "character-selection"):
            validate_production_record(invalid)
        invalid = copy.deepcopy(value)
        by_name = {state["name"]: state for state in invalid["ui_closure"]}
        by_name["screenshot_map"]["output_size"] = invalid["viewport"]
        with self.assertRaisesRegex(ArtifactError, "screenshot_map.*size"):
            validate_production_record(invalid)
        invalid = copy.deepcopy(value)
        by_name = {state["name"]: state for state in invalid["ui_closure"]}
        by_name["screenshot_map"]["pixels_sha256"] = \
            by_name["screenshot_window"]["pixels_sha256"]
        with self.assertRaisesRegex(ArtifactError, "map screenshot crop"):
            validate_production_record(invalid)

    def test_valid_lifecycle_record(self):
        events = []
        for name in LIFECYCLE_EVENTS:
            frames = [10_000] * 40
            events.append({
                "name": name,
                "recovery_attempts": 0 if name in {
                    "cold_asset_upload", "teleport", "reconnect", "screenshot_readback"
                } else 1,
                "fullscreen": name == "fullscreen_enter",
                "action": {"asynchronous": name == "screenshot_readback",
                           "duration_ns": 1000,
                           "uploads": 1 if name == "cold_asset_upload" else 0,
                           "upload_bytes": 4 if name == "cold_asset_upload" else 0,
                           "source_uploads": 1 if name == "cold_asset_upload" else 0,
                           "source_upload_bytes": 4 if name == "cold_asset_upload" else 0,
                           "instance_uploads": 0,
                           "instance_upload_bytes": 0,
                           "light_uploads": 0,
                           "light_upload_bytes": 0,
                           "slot_uniform_uploads": 0,
                           "slot_uniform_upload_bytes": 0,
                           "resource_creations": 1 if name == "cold_asset_upload" else 0,
                           "resource_destructions": 0,
                           "device_recoveries": 0 if name in {
                               "cold_asset_upload", "teleport", "reconnect",
                               "screenshot_readback"
                           } else 1,
                           "recovery_failures": 0,
                           "readbacks": 1 if name == "screenshot_readback" else 0,
                           "fallbacks": 0},
                "output_size": [1056, 804] if name == "resize_grow" else [1024, 780],
                "display_mode_size": [1024, 780] if name == "fullscreen_enter" else [0, 0],
                "pixels_sha256": "4" * 64 if name == "resize_grow" else (
                    "5" * 64 if name == "teleport" else "3" * 64),
                "artifact": f"review/{name}.png",
                "artifact_sha256": "6" * 64,
                "frame_windows_ns": frames,
                "frame_ns": {"p95": 10_000, "p99": 10_000, "max": 10_000},
                "steady_state": {"uploads": 0, "upload_bytes": 0,
                                 "slot_uniform_uploads": 0,
                                 "slot_uniform_upload_bytes": 0,
                                 "resource_creations": 0, "resource_destructions": 0,
                                 "readbacks": 0, "commands": 1, "batches": 1,
                                 "draws": 1, "retained_bytes": 4, "fallbacks": 0},
            })
        value = {
            "schema_version": 2,
            "benchmark": "gpu-production-recovery-lifecycle",
            "fixture": "brynknot-movement",
            "manifest_sha256": "7" * 64,
            "viewport": [1024, 780],
            "resize_delta": [32, 24],
            "revision": "1" * 40,
            "dirty": False,
            "gpu": {"backend": "vulkan", "device": "device", "driver_name": "driver",
                    "driver_version": "1", "hardware_tier": "reference",
                    "qualified_hardware": True},
            "build": {"type": "Release", "compiler": "GNU 15", "system": "Linux"},
            "shader_cohort": "2" * 64,
            "production_path": True,
            "sustained_frames_per_event": 40,
            "events": events,
            "final_checkpoint": {"algorithm": "sha256-rgba32-with-dimensions",
                                 "pixels_sha256": "3" * 64},
        }
        validate_lifecycle_record(value)
        screenshot = next(event for event in value["events"]
                          if event["name"] == "screenshot_readback")
        screenshot["action"]["readbacks"] = 0
        with self.assertRaisesRegex(ArtifactError, "action readback"):
            validate_lifecycle_record(value)
        screenshot["action"]["readbacks"] = 1
        value["events"] = value["events"][:-1]
        with self.assertRaisesRegex(ArtifactError, "incomplete"):
            validate_lifecycle_record(value)

    def test_lifecycle_action_schema_is_strict_and_reconciled(self):
        events = []
        for name in LIFECYCLE_EVENTS:
            events.append({
                "name": name,
                "recovery_attempts": 0 if name in {
                    "cold_asset_upload", "teleport", "reconnect", "screenshot_readback"
                } else 1,
                "fullscreen": name == "fullscreen_enter",
                "action": {
                    "asynchronous": name == "screenshot_readback", "duration_ns": 1,
                    "uploads": 1 if name == "cold_asset_upload" else 0,
                    "upload_bytes": 4 if name == "cold_asset_upload" else 0,
                    "source_uploads": 1 if name == "cold_asset_upload" else 0,
                    "source_upload_bytes": 4 if name == "cold_asset_upload" else 0,
                    "instance_uploads": 0, "instance_upload_bytes": 0,
                    "light_uploads": 0, "light_upload_bytes": 0,
                    "slot_uniform_uploads": 0, "slot_uniform_upload_bytes": 0,
                    "resource_creations": 1 if name == "cold_asset_upload" else 0,
                    "resource_destructions": 0,
                    "device_recoveries": 0 if name in {
                        "cold_asset_upload", "teleport", "reconnect", "screenshot_readback"
                    } else 1,
                    "recovery_failures": 0,
                    "readbacks": 1 if name == "screenshot_readback" else 0,
                    "fallbacks": 0,
                },
                "output_size": [1056, 804] if name == "resize_grow" else [1024, 780],
                "display_mode_size": [1024, 780] if name == "fullscreen_enter" else [0, 0],
                "pixels_sha256": "4" * 64 if name == "resize_grow" else (
                    "5" * 64 if name == "teleport" else "3" * 64),
                "artifact": f"review/{name}.png", "artifact_sha256": "6" * 64,
                "frame_windows_ns": [10_000] * 40,
                "frame_ns": {"p95": 10_000, "p99": 10_000, "max": 10_000},
                "steady_state": {
                    "uploads": 0, "upload_bytes": 0, "slot_uniform_uploads": 0,
                    "slot_uniform_upload_bytes": 0, "resource_creations": 0,
                    "resource_destructions": 0, "readbacks": 0, "commands": 1,
                    "batches": 1, "draws": 1, "retained_bytes": 4, "fallbacks": 0,
                },
            })
        value = {
            "schema_version": 2, "benchmark": "gpu-production-recovery-lifecycle",
            "fixture": "brynknot-movement", "manifest_sha256": "7" * 64,
            "viewport": [1024, 780], "resize_delta": [32, 24], "revision": "1" * 40,
            "dirty": False,
            "gpu": {"backend": "vulkan", "device": "device", "driver_name": "driver",
                    "driver_version": "1", "hardware_tier": "reference",
                    "qualified_hardware": True},
            "build": {"type": "Release", "compiler": "GNU 15", "system": "Linux"},
            "shader_cohort": "2" * 64, "production_path": True,
            "sustained_frames_per_event": 40, "events": events,
            "final_checkpoint": {"algorithm": "sha256-rgba32-with-dimensions",
                                 "pixels_sha256": "3" * 64},
        }
        invalid = copy.deepcopy(value)
        invalid["events"][0]["action"]["duration_ns"] = 0
        with self.assertRaisesRegex(ArtifactError, "duration"):
            validate_lifecycle_record(invalid)
        invalid = copy.deepcopy(value)
        invalid["events"][0]["action"]["uploads"] = 2
        with self.assertRaisesRegex(ArtifactError, "accounting"):
            validate_lifecycle_record(invalid)
        invalid = copy.deepcopy(value)
        del invalid["events"][0]["action"]["light_uploads"]
        with self.assertRaisesRegex(ArtifactError, "schema"):
            validate_lifecycle_record(invalid)

    def test_review_png_identity_and_dimensions_are_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            review = root / "review"
            review.mkdir()
            contents = png(7, 5)
            artifact = review / "frame.png"
            artifact.write_bytes(contents)
            digest = hashlib.sha256(contents).hexdigest()
            pixels = _review_artifact(root / "records.jsonl",
                                      "review/frame.png",
                                      digest,
                                      [7, 5])
            with self.assertRaisesRegex(ArtifactError, "dimensions"):
                _review_artifact(root / "records.jsonl", "review/frame.png", digest, [5, 7])
            with self.assertRaisesRegex(ArtifactError, "digest mismatch"):
                _review_artifact(root / "records.jsonl", "review/frame.png", "0" * 64, [7, 5])
            with self.assertRaisesRegex(ArtifactError, "not closed"):
                _review_artifact(root / "records.jsonl", "review/../frame.png", digest, [7, 5])
            with self.assertRaisesRegex(ArtifactError, "pixels mismatch"):
                _review_artifact(root / "records.jsonl",
                                 "review/frame.png",
                                 digest,
                                 [7, 5],
                                 hashlib.sha256(pixels).hexdigest())

    def test_map_screenshot_requires_exact_centered_pixel_crop(self):
        viewport = [8, 8]
        map_size = [6, 6]
        window = bytes((index % 251 for index in range(viewport[0] * viewport[1] * 4)))
        exact = b"".join(
            window[((row + 1) * viewport[0] + 1) * 4:
                   ((row + 1) * viewport[0] + 7) * 4]
            for row in range(6)
        )
        record = {
            "viewport": viewport,
            "ui_closure": [
                {"name": "screenshot_window", "output_size": viewport},
                {"name": "screenshot_map", "output_size": map_size},
            ],
        }
        _require_ui_screenshot_crop(record, {
            "screenshot_window": window,
            "screenshot_map": exact,
        })
        top_left = b"".join(
            window[(row * viewport[0]) * 4:(row * viewport[0] + 6) * 4]
            for row in range(6)
        )
        with self.assertRaisesRegex(ArtifactError, "exact centered crop"):
            _require_ui_screenshot_crop(record, {
                "screenshot_window": window,
                "screenshot_map": top_left,
            })

    def test_valid_record(self):
        self.assertEqual(validate_record(record()), "dense-17x17-five-depth-1080p")

    def test_rejects_environment_only_qualification(self):
        value = record()
        value["gpu"]["qualified_hardware"] = False
        with self.assertRaisesRegex(ArtifactError, "attestation"):
            validate_record(value)

    def test_rejects_timed_readback_and_churn(self):
        value = record()
        value["checkpoint"]["timed"] = True
        with self.assertRaisesRegex(ArtifactError, "timed"):
            validate_record(value)
        value = record()
        value["steady_state"]["uploads"] = 1
        with self.assertRaisesRegex(ArtifactError, "uploads"):
            validate_record(value)

    def test_rejects_missing_or_unbounded_slot_uniform_uploads(self):
        value = record()
        del value["steady_state"]["slot_uniform_upload_bytes"]
        with self.assertRaisesRegex(ArtifactError, "schema"):
            validate_record(value)
        value = record()
        value["steady_state"]["slot_uniform_upload_bytes"] = 36 * 1024 + 1
        value["steady_state"]["upload_bytes"] = 36 * 1024 + 1
        with self.assertRaisesRegex(ArtifactError, "bounded"):
            validate_record(value)

    def test_rejects_fenced_gpu_budget(self):
        value = record()
        value["stage_windows_ns"]["gpu_completion_wait"] = [5_000_000] * 40
        with self.assertRaisesRegex(ArtifactError, "5 ms"):
            validate_record(value)

    def test_rejects_frame_and_workload_contracts(self):
        value = record()
        value["frame_windows_ns"] = [1_000_000_000] * 40
        with self.assertRaisesRegex(ArtifactError, "frame p95"):
            validate_record(value)
        value = record()
        value["workload"]["logical_view"] = 1
        with self.assertRaisesRegex(ArtifactError, "geometry"):
            validate_record(value)

    def test_complete_matrix_requires_three_fresh_runs(self):
        values = [record(name) for name in WORKLOADS for _ in range(3)]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "matrix.jsonl"
            path.write_text("".join(json.dumps(value) + "\n" for value in values),
                            encoding="utf-8")
            validate([path], True)
            incomplete = copy.deepcopy(values[:-1])
            path.write_text("".join(json.dumps(value) + "\n" for value in incomplete),
                            encoding="utf-8")
            with self.assertRaisesRegex(ArtifactError, "three"):
                validate([path], True)


if __name__ == "__main__":
    unittest.main()
