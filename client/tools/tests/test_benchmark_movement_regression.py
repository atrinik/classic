from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "benchmark_movement_regression.py"
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("benchmark_movement_regression", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)

VERIFY_MODULE_PATH = MODULE_PATH.parent / "verify_movement_benchmark.py"
VERIFY_SPEC = importlib.util.spec_from_file_location(
    "verify_movement_benchmark", VERIFY_MODULE_PATH
)
assert VERIFY_SPEC is not None and VERIFY_SPEC.loader is not None
movement_verifier = importlib.util.module_from_spec(VERIFY_SPEC)
VERIFY_SPEC.loader.exec_module(movement_verifier)


def timing(samples: int, *, p50_ns: int = 1_000_000, p95_ns: int = 2_000_000,
           first_ns: int = 2_000_000, last_ns: int = 2_000_000) -> dict[str, object]:
    if samples == 0:
        return {
            "unit": "ns", "samples": 0, "p50": 0, "p95": 0, "p99": 0,
            "max": 0, "windows": [],
        }
    if p50_ns == p95_ns == first_ns == last_ns == 0:
        return {
            "unit": "ns",
            "samples": samples,
            "p50": 0,
            "p95": 0,
            "p99": 0,
            "max": 0,
            "windows": [
                {"start_tick": start, "samples": min(32, samples - start), "p95_ns": 0}
                for start in range(0, samples, 32)
            ],
        }
    windows = []
    for start in range(0, samples, 32):
        window_p95 = first_ns if start == 0 else last_ns if start + 32 >= samples else p95_ns
        windows.append(
            {"start_tick": start, "samples": min(32, samples - start), "p95_ns": window_p95}
        )
    return {
        "unit": "ns",
        "samples": samples,
        "p50": p50_ns,
        "p95": p95_ns,
        "p99": max(p95_ns, 3_000_000),
        "max": max(p95_ns, first_ns, last_ns, 4_000_000),
        "windows": windows,
    }


def empty_lighting_counters() -> dict[str, int]:
    return {
        field: 0
        for field in benchmark.validate_record.__globals__["LIGHTING_COUNTER_FIELDS"]
    }


def lighting_timings(counters: dict[str, int]) -> dict[str, dict[str, object]]:
    calls = {
        "translation": counters["field_translations"]
        + counters["field_translation_fallback_bounds"]
        + counters["field_translation_fallback_control"],
        "dirty_clear": counters["field_dirty_marks"],
        "rasterization": counters["field_rasterized_quads"],
        "extrapolation": counters["field_rebuilds"],
        "tone_map_multiply": counters["render_calls"],
        "sprite_lookup": counters["lit_sprite_lookups"],
        "sprite_construction": counters["lit_sprite_misses"],
        "sprite_invalidation": counters["lit_sprite_clears"],
    }
    return {
        name: {"unit": "ns", "calls": count, "elapsed": count * 100}
        for name, count in calls.items()
    }


def lighting_state(mode: str) -> dict[str, object]:
    active = mode == "smooth"
    return {
        "allocated_levels": 5 if active else 0,
        "active_levels": 0,
        "cache_valid_levels": 5 if active else 0,
        "dirty_levels": 0,
        "lit_sprite_entries": 5 if active else 0,
        "lit_sprite_bytes": 1280 if active else 0,
        "retained_field_bytes": 2560 if active else 0,
        "state_digest": "a" * 16,
    }


def lighting_level(
    depth: int, draws: int, name: str, mode: str, reconstruction: str
) -> dict[str, object]:
    allocated = mode == "smooth" and -2 <= depth <= 2
    state = {
        "cache_valid": allocated,
        "dirty": False,
        "entries": 1 if allocated else 0,
        "bytes": 256 if allocated else 0,
        "retained_field_bytes": 512 if allocated else 0,
        "state_digest": "a" * 16,
    }
    counters = empty_lighting_counters()
    if allocated:
        reuses = draws if name == "idle" else 0
        rebuilds = draws - reuses
        translated = name == "sustained" and reconstruction == "translated"
        full_control = name == "sustained" and reconstruction == "full"
        counters.update(
            {
                "field_begins": draws,
                "field_dirty_marks": rebuilds,
                "field_dirty_pixels": rebuilds * (320 * 240 if full_control else 1024),
                "field_rasterized_quads": rebuilds,
                "field_translations": rebuilds if translated else 0,
                "field_translated_pixels": rebuilds * 512 if translated else 0,
                "field_translated_bytes": rebuilds * 5120 if translated else 0,
                "field_scroll_x_pixels": rebuilds * 24 if name == "sustained" else 0,
                "field_scroll_y_pixels": rebuilds * 12 if name == "sustained" else 0,
                "field_translation_fallback_control": rebuilds if full_control else 0,
                "field_partial_rebuilds": rebuilds if translated else 0,
                "field_full_rebuilds": rebuilds if not translated else 0,
                "field_full_rebuild_cache": rebuilds
                if not translated and not full_control
                else 0,
                "field_full_rebuild_control": rebuilds if full_control else 0,
                "field_rebuilds": rebuilds,
                "field_reuses": reuses,
                "render_calls": draws,
                "lit_sprite_draws": 2,
                "lit_sprite_lookups": 2,
                "lit_sprite_hits": 2,
            }
        )
    timings = lighting_timings(counters)
    if allocated and depth != 0:
        timings["tone_map_multiply"] = {"unit": "ns", "calls": 0, "elapsed": 0}
    return {
        "depth": depth,
        "width": 320 if allocated else 0,
        "height": 240 if allocated else 0,
        "start": {"allocated": allocated, **copy.deepcopy(state)},
        "end": {"allocated": allocated, **copy.deepcopy(state)},
        "peak": {
            "entries": 1 if allocated else 0,
            "bytes": 256 if allocated else 0,
            "retained_field_bytes": 512 if allocated else 0,
        },
        "counters": counters,
        "timings": timings,
    }


def checkpoint(
    name: str, index: int, viewport_width: int = 320, viewport_height: int = 240
) -> dict[str, object]:
    width, height = (
        (viewport_width + 32, viewport_height + 24)
        if name == "resized"
        else (viewport_width, viewport_height)
    )
    x, y = (11, 10) if name == "step_b" else (10, 11) if name == "step_c" else (10, 10)
    return {
        "name": name,
        "pixels_sha256": f"{index + 1:064x}",
        "state_digest": f"{index + 1:016x}",
        "map_x": x,
        "map_y": y,
        "viewport_width": width,
        "viewport_height": height,
    }


def visual_lifecycle_digest(checkpoints: list[dict[str, object]]) -> str:
    digest = hashlib.sha256(b"pvm-checkpoints-v1\n")
    for item in checkpoints:
        digest.update(
            (
                f"{item['name']}\t{item['pixels_sha256']}\t{item['map_x']}\t{item['map_y']}\t"
                f"{item['viewport_width']}\t{item['viewport_height']}\n"
            ).encode("ascii")
        )
    return digest.hexdigest()


def native_record(
    *,
    sustained_p95_ns: int = 2_000_000,
    first_window_ns: int = 2_000_000,
    last_window_ns: int = 2_000_000,
    mode: str = "smooth",
    viewport: str = "standard",
    reconstruction: str = "translated",
    workload_variant: str = "production",
) -> dict[str, object]:
    phases = []
    packet_counts = {"cold": 1, "sustained": 480, "idle": 8, "resumed": 80}
    changed_counts = {"cold": 1, "sustained": 480, "idle": 0, "resumed": 80}
    draw_counts = {"cold": 1, "sustained": 480, "idle": 0, "resumed": 80}
    animation_draw_counts = {"cold": 0, "sustained": 0, "idle": 16, "resumed": 0}
    minimap_draw_counts = {"cold": 1, "sustained": 240, "idle": 0, "resumed": 40}
    for name, samples in benchmark.REQUIRED_PHASES.items():
        p95_ns = sustained_p95_ns if name == "sustained" else 2_000_000
        first_ns = first_window_ns if name == "sustained" else 2_000_000
        last_ns = last_window_ns if name == "sustained" else 2_000_000
        packets = packet_counts[name]
        changed = changed_counts[name]
        draws = draw_counts[name]
        animation_draws = animation_draw_counts[name]
        minimap_draws = (
            0 if workload_variant == "isolated-lighting" else minimap_draw_counts[name]
        )
        renderer_draws = draws + minimap_draws
        render_passes = renderer_draws + animation_draws
        reasons = {
            "external": 0,
            "packet": changed,
            "scroll": 0,
            "animation": samples,
            "lighting": 0,
            "resize": 0,
            "ui": 0,
        }
        work = timing(samples, p95_ns=p95_ns, first_ns=first_ns, last_ns=last_ns)
        loop = timing(samples, p50_ns=125_000_000, p95_ns=125_000_000,
                      first_ns=125_000_000, last_ns=125_000_000)
        wait = timing(samples, p50_ns=123_000_000, p95_ns=123_000_000,
                      first_ns=123_000_000, last_ns=123_000_000)
        budget_yields = 8 if name == "resumed" else 0
        queue_digest = f"{list(benchmark.REQUIRED_PHASES).index(name) + 1:016x}"
        lighting_levels = [
            lighting_level(depth, draws, name, mode, reconstruction)
            for depth in range(-6, 7)
        ]
        lighting_phase_counters = empty_lighting_counters()
        for field in lighting_phase_counters:
            lighting_phase_counters[field] = sum(
                level["counters"][field] for level in lighting_levels
            )
        lighting_phase_timings = {
            timing_name: {
                "unit": "ns",
                "calls": sum(
                    level["timings"][timing_name]["calls"] for level in lighting_levels
                ),
                "elapsed": sum(
                    level["timings"][timing_name]["elapsed"] for level in lighting_levels
                ),
            }
            for timing_name in benchmark.validate_record.__globals__["LIGHTING_TIMING_FIELDS"]
        }
        lighting_active = mode == "smooth"
        phases.append(
            {
                "name": name,
                "samples": samples,
                "map_packets": packets,
                "changed_map_packets": changed,
                "noop_map_packets": packets - changed,
                "full_map_draws": draws,
                "animation_draws": animation_draws,
                "animation_ticks": samples,
                "draw_reasons": reasons,
                "frame_time": copy.deepcopy(work),
                "main_loop": {
                    "update_cadence_hz": 8,
                    "update_interval_ns": 125_000_000,
                    "work_time": copy.deepcopy(work),
                    "simulated_wait_time": wait,
                    "simulated_update_loop_time": loop,
                    "work_capacity_fps": {
                        "p50": 1_000_000_000 / work["p50"],
                        "p95": 1_000_000_000 / work["p95"],
                    },
                },
                "map_time": timing(draws),
                "animation_time": timing(animation_draws),
                "lighting_work_time": timing(
                    draws,
                    p50_ns=500_000 if lighting_active else 0,
                    p95_ns=600_000 if lighting_active else 0,
                    first_ns=600_000 if lighting_active else 0,
                    last_ns=600_000 if lighting_active else 0,
                ),
                "local_minimap": {
                    "enabled": workload_variant == "production",
                    "update_interval_ms": 250 if workload_variant == "production" else 0,
                    "surface_width": 1700 if workload_variant == "production" else 0,
                    "surface_height": 1200 if workload_variant == "production" else 0,
                    "map_draws": minimap_draws,
                    "map_time": timing(minimap_draws),
                },
                "queue": {
                    "enqueued": packets,
                    "dequeued": packets,
                    "budget_yields": budget_yields,
                    "recoveries": 1 if name == "resumed" else 0,
                    "start_depth": 0,
                    "end_depth": 0,
                    "peak_depth": 8 if name == "resumed" else 1,
                    "start_bytes": 0,
                    "end_bytes": 0,
                    "peak_bytes": 8192 if name == "resumed" else 1024,
                    "oldest_age_us": 125_000 if name == "resumed" else 0,
                    "current_oldest_age_us": 0,
                    "processing_us": packets * 5_000,
                    "service_clock": "simulated",
                    "simulated_command_us": 5_000,
                    "drain_time": timing(samples),
                    "due": False,
                    "budget_due": False,
                    "order_digests_comparable": True,
                    "enqueued_order_digest": queue_digest,
                    "dequeued_order_digest": queue_digest,
                },
                "map": {
                    "map_draws": renderer_draws,
                    "primary_map_draws": draws,
                    "auxiliary_map_draws": minimap_draws,
                    "animation_draws": animation_draws,
                    "animation_level_draws": animation_draws * 5,
                    "presents": 0,
                    "present_failures": 0,
                    "render_failures": 0,
                    "fault_injections": 0,
                    "fault_detections": 0,
                    "level_draws": renderer_draws * 5,
                    "render_commands": renderer_draws * 10,
                    "annotations": 0,
                    "ui_tiles": 0,
                    "peak_render_commands": 10,
                    "peak_active_levels": 5,
                    "renderer_allocation_statistics_available": False,
                    "renderer_allocations": 0,
                    "renderer_allocation_bytes": 0,
                },
                "render_stages": {
                    stage: {
                        "unit": "us",
                        "elapsed": (
                            0
                            if stage == "lighting" and not lighting_active
                            else draws if stage == "lighting" else render_passes
                        ),
                        "calls": (
                            0
                            if stage == "lighting" and not lighting_active
                            else draws * 5
                            if stage == "lighting"
                            else render_passes
                            if stage in ("map", "paint", "ui", "command_sort", "sprite_effects")
                            else draws + animation_draws
                            if stage in ("living_occlusion", "hint_replay")
                            else renderer_draws
                            if stage in ("map_scratch_clear", "ground_composite")
                            else renderer_draws * 5 + animation_draws * 5
                            if stage == "objects"
                            else renderer_draws * 5
                        ),
                        "scope": scope,
                    }
                    for stage, scope in benchmark.validate_record.__globals__["RENDER_STAGES"].items()
                },
                "lighting": {
                    "available": True,
                    "start": lighting_state(mode),
                    "end": lighting_state(mode),
                    "peak": {
                        "allocated_levels": 5 if lighting_active else 0,
                        "active_levels": 5 if lighting_active else 0,
                        "cache_valid_levels": 5 if lighting_active else 0,
                        "dirty_levels": 0,
                        "lit_sprite_entries": 5 if lighting_active else 0,
                        "lit_sprite_bytes": 1280 if lighting_active else 0,
                        "retained_field_bytes": 2560 if lighting_active else 0,
                    },
                    "counters": lighting_phase_counters,
                    "timings": lighting_phase_timings,
                    "levels": lighting_levels,
                },
                "sprite_cache": {
                    "available": True,
                    "limits": {"entries": 4096, "estimated_bytes": 67108864},
                    "counters": {
                        "lookups": 10,
                        "hits": 10,
                        "misses": 0,
                        "insertions": 0,
                        "evictions": 0,
                        "rejections": 0,
                        "gc_runs": samples,
                        "gc_removals": 0,
                        "gc_time_ns": 1000,
                    },
                    "start": {"entries": 4, "estimated_bytes": 1024},
                    "end": {"entries": 4, "estimated_bytes": 1024},
                    "peak": {"entries": 4, "estimated_bytes": 1024},
                },
            }
        )
    viewport_width, viewport_height = (320, 240) if viewport == "standard" else (1920, 1080)
    checkpoints = [checkpoint(name, index, viewport_width, viewport_height) for index, name in enumerate(
        benchmark.validate_record.__globals__["EXPECTED_CHECKPOINTS"]
    )]
    checkpoint_sha = visual_lifecycle_digest(checkpoints)
    standard_checkpoints = [checkpoint(name, index) for index, name in enumerate(
        benchmark.validate_record.__globals__["EXPECTED_CHECKPOINTS"]
    )]
    standard_checkpoint_sha = visual_lifecycle_digest(standard_checkpoints)
    return {
        "schema_version": 6,
        "benchmark": "player-view-movement",
        "tick_ms": 125,
        "simulated_tick_hz": 8,
        "identity": {
            "instrumentation": {
                "schema_version": 6,
                "fixture_schema_version": 3,
                "workload": "pvm1-map2-lifecycle-v4",
                "lighting_statistics_version": 5,
                "map_statistics_version": 3,
                "render_profiler_statistics_version": 4,
                "sprite_cache_statistics_version": 3,
            },
            "implementation": {
                "revision": "a" * 40,
                "dirty": False,
                "dirty_known": True,
                "build_type": "Release",
                "compiler_id": "GNU",
                "compiler_version": "15.2.0",
                "cmake_system": "Linux",
                "sdl_version": "3.2.0",
                "sdl_platform": "Linux",
            },
            "run": {
                "runner_os": "Linux",
                "runner_arch": "x86_64",
                "ci": "true",
                "cpu_count": 4,
                "cpu_model": "Test CPU",
                "runner_image_os": "ubuntu24",
                "runner_image_version": "20260801.1",
                "viewport": {
                    "name": viewport,
                    "width": viewport_width,
                    "height": viewport_height,
                },
                "mode": mode,
                "reconstruction": reconstruction,
                "workload_variant": workload_variant,
            },
        },
        "fixture": {
            "manifest_schema_version": 1,
            "manifest_sha256": "b" * 64,
            "snapshot_sha256": "c" * 64,
            "movement_stream_sha256": "d" * 64,
            "transition_snapshot_sha256": "e" * 64,
            "expected_standard_checkpoint_sha256": standard_checkpoint_sha,
            "look_width": 17,
            "look_height": 17,
            "resize_width_delta": 32,
            "resize_height_delta": 24,
            "rng_seed": 0x1961932026,
            "smooth_lighting": mode == "smooth",
        },
        "checkpoint_sha256": checkpoint_sha,
        "same_process_checkpoint_sha256": checkpoint_sha,
        "final_state_digest": checkpoints[-1]["state_digest"],
        "same_process_final_state_digest": checkpoints[-1]["state_digest"],
        "checkpoints": checkpoints,
        "same_process_checkpoints": copy.deepcopy(checkpoints),
        "lifecycle": {
            "full_map_draws": 4,
            "full_draw_reasons": {
                "reset_packet": 1,
                "changed_map_packet": 0,
                "noop_map_packet": 0,
                "animation_only_tick": 0,
                "resize": 2,
                "map_transition": 1,
            },
        },
        "process_peak_rss_bytes": 1024,
        "process_peak_rss_available": True,
        "phases": phases,
    }


def discrete_pair() -> list[dict[str, object]]:
    return [native_record(mode="discrete"), native_record(mode="discrete")]


def remove_phase_map_draws(phase: dict[str, object]) -> None:
    phase["full_map_draws"] = 0
    phase["animation_draws"] = 0
    phase["draw_reasons"] = {field: 0 for field in phase["draw_reasons"]}
    phase["map_time"] = timing(0)
    phase["animation_time"] = timing(0)
    phase["local_minimap"]["map_draws"] = 0
    phase["local_minimap"]["map_time"] = timing(0)
    phase["map"]["map_draws"] = 0
    phase["map"]["primary_map_draws"] = 0
    phase["map"]["auxiliary_map_draws"] = 0
    phase["map"]["animation_draws"] = 0
    phase["map"]["animation_level_draws"] = 0
    phase["map"]["level_draws"] = 0
    phase["map"]["render_commands"] = 0
    phase["map"]["peak_render_commands"] = 0
    phase["map"]["peak_active_levels"] = 0
    for stage in phase["render_stages"].values():
        stage["elapsed"] = 0
        stage["calls"] = 0
    phase["lighting"]["counters"] = empty_lighting_counters()
    for level in phase["lighting"]["levels"]:
        level["counters"] = empty_lighting_counters()


def additional_contexts(
    *, full: bool = False, smooth_samples: int = 2
) -> dict[str, list[dict[str, object]]]:
    contexts = {
        benchmark.STANDARD_DISCRETE_CONTEXT: discrete_pair(),
        benchmark.STANDARD_TRANSLATED_CONTEXT: [
            native_record(workload_variant="isolated-lighting")
            for _ in range(smooth_samples)
        ],
        benchmark.STANDARD_FULL_CONTEXT: [
            native_record(reconstruction="full", workload_variant="isolated-lighting")
            for _ in range(smooth_samples)
        ],
    }
    if full:
        contexts[benchmark.LARGE_DISCRETE_CONTEXT] = [
            native_record(mode="discrete", viewport="large"),
            native_record(mode="discrete", viewport="large"),
        ]
        contexts[benchmark.LARGE_FULL_CONTEXT] = [
            native_record(
                viewport="large", reconstruction="full", workload_variant="isolated-lighting"
            ),
            native_record(
                viewport="large", reconstruction="full", workload_variant="isolated-lighting"
            ),
        ]
        contexts[benchmark.LARGE_TRANSLATED_CONTEXT] = [
            native_record(viewport="large", workload_variant="isolated-lighting"),
            native_record(viewport="large", workload_variant="isolated-lighting"),
        ]
    return contexts


class NativeV6RecordTests(unittest.TestCase):
    def test_parse_accepts_closed_v5_record(self) -> None:
        self.assertEqual(benchmark.parse_result(json.dumps(native_record()))["schema_version"], 6)

    def test_parse_rejects_extra_output_and_duplicate_fields(self) -> None:
        encoded = json.dumps(native_record())
        with self.assertRaisesRegex(benchmark.BenchmarkError, "exactly one"):
            benchmark.parse_result(encoded + "\nnoise\n")
        duplicate = encoded.replace('"schema_version": 6,',
                                    '"schema_version": 6, "schema_version": 6,', 1)
        with self.assertRaisesRegex(benchmark.BenchmarkError, "repeated JSON field"):
            benchmark.parse_result(duplicate)
        with self.assertRaisesRegex(ValueError, "repeated JSON field"):
            movement_verifier.parse_record(duplicate)

    def test_rejects_extra_field_and_negative_counter(self) -> None:
        malformed = native_record()
        malformed["unexpected"] = 1
        with self.assertRaisesRegex(ValueError, "record.*incompatible"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["queue"]["processing_us"] = -1
        with self.assertRaisesRegex(ValueError, "processing_us is invalid"):
            benchmark.validate_record(malformed)

    def test_rejects_wrong_phase_and_idle_packet_accounting(self) -> None:
        malformed = native_record()
        malformed["phases"].reverse()
        with self.assertRaisesRegex(ValueError, "phase cold is invalid"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][2]["map_packets"] = 16
        malformed["phases"][2]["noop_map_packets"] = 16
        with self.assertRaisesRegex(ValueError, "phase idle accounting"):
            benchmark.validate_record(malformed)

    def test_rejects_local_minimap_geometry_cadence_and_call_mismatch(self) -> None:
        malformed = native_record()
        malformed["phases"][1]["local_minimap"]["surface_width"] = 1699
        with self.assertRaisesRegex(ValueError, "local minimap is invalid"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][1]["local_minimap"]["update_interval_ms"] = 125
        with self.assertRaisesRegex(ValueError, "local minimap is invalid"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][1]["map"]["auxiliary_map_draws"] -= 1
        with self.assertRaisesRegex(ValueError, "map accounting is invalid"):
            benchmark.validate_record(malformed)

    def test_isolated_lighting_workload_excludes_local_minimap(self) -> None:
        isolated = native_record(workload_variant="isolated-lighting")
        benchmark.validate_record(isolated)
        self.assertTrue(
            all(phase["local_minimap"]["map_draws"] == 0 for phase in isolated["phases"])
        )
        isolated["phases"][1]["local_minimap"]["enabled"] = True
        with self.assertRaisesRegex(ValueError, "local minimap is invalid"):
            benchmark.validate_record(isolated)

    def test_rejects_queue_reordering_and_coherently_accepts_unknown_dirty(self) -> None:
        malformed = native_record()
        malformed["phases"][1]["queue"]["dequeued_order_digest"] = "f" * 16
        with self.assertRaisesRegex(ValueError, "queue accounting"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][1]["queue"]["order_digests_comparable"] = False
        with self.assertRaisesRegex(ValueError, "queue accounting"):
            benchmark.validate_record(malformed)
        unknown = native_record()
        unknown["identity"]["implementation"].update({"dirty": None, "dirty_known": False})
        benchmark.validate_record(unknown)
        unknown["identity"]["implementation"]["dirty"] = False
        with self.assertRaisesRegex(ValueError, "dirty identity"):
            benchmark.validate_record(unknown)

    def test_rejects_fabricated_work_capacity_and_profiler_scope(self) -> None:
        malformed = native_record()
        malformed["phases"][0]["main_loop"]["work_capacity_fps"]["p95"] = 1000.0
        with self.assertRaisesRegex(ValueError, "work capacity is inconsistent"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        loop = malformed["phases"][1]["main_loop"]["simulated_update_loop_time"]
        for field in ("p50", "p95", "p99", "max"):
            loop[field] = 1_000_000
        for window in loop["windows"]:
            window["p95_ns"] = 1_000_000
        malformed["phases"][1]["main_loop"]["work_capacity_fps"] = {
            "p50": 1000.0,
            "p95": 1000.0,
        }
        with self.assertRaisesRegex(ValueError, "loop timing is inconsistent"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["render_stages"]["ground"]["scope"] = "per_map_draw"
        with self.assertRaisesRegex(ValueError, "render metadata"):
            benchmark.validate_record(malformed)

    def test_rejects_fabricated_queue_service_and_accepts_zero_drain_time(self) -> None:
        malformed = native_record()
        malformed["phases"][1]["queue"]["processing_us"] -= 1
        with self.assertRaisesRegex(ValueError, "queue service"):
            benchmark.validate_record(malformed)
        record = native_record()
        drain = record["phases"][2]["queue"]["drain_time"]
        for field in ("p50", "p95", "p99", "max"):
            drain[field] = 0
        for window in drain["windows"]:
            window["p95_ns"] = 0
        benchmark.validate_record(record)

    def test_accepts_coalesced_redraw_reasons_and_rejects_unexplained_draw(self) -> None:
        record = native_record()
        sustained = record["phases"][1]
        sustained["draw_reasons"]["scroll"] = sustained["full_map_draws"]
        benchmark.validate_record(record)
        sustained["draw_reasons"] = {field: 0 for field in sustained["draw_reasons"]}
        with self.assertRaisesRegex(ValueError, "draw reasons accounting"):
            benchmark.validate_record(record)
        malformed = native_record()
        idle = malformed["phases"][2]
        idle["draw_reasons"]["animation"] = 0
        idle["draw_reasons"]["packet"] = 1
        with self.assertRaisesRegex(ValueError, "draw reasons accounting"):
            benchmark.validate_record(malformed)

    def test_rejects_missing_cold_reset_draw_without_pinning_other_phase_draws(self) -> None:
        malformed = native_record()
        cold = malformed["phases"][0]
        remove_phase_map_draws(cold)
        with self.assertRaisesRegex(ValueError, "draw reason is impossible"):
            benchmark.validate_record(malformed)

    def test_accepts_idle_zero_full_map_timing(self) -> None:
        record = native_record()
        benchmark.validate_record(record)

    def test_accepts_zero_wait_for_an_over_budget_phase(self) -> None:
        record = native_record()
        main_loop = record["phases"][1]["main_loop"]
        over_budget = timing(
            480,
            p50_ns=130_000_000,
            p95_ns=140_000_000,
            first_ns=140_000_000,
            last_ns=140_000_000,
        )
        record["phases"][1]["frame_time"] = copy.deepcopy(over_budget)
        main_loop["work_time"] = copy.deepcopy(over_budget)
        main_loop["simulated_update_loop_time"] = copy.deepcopy(over_budget)
        main_loop["work_capacity_fps"] = {
            "p50": 1_000_000_000 / 130_000_000,
            "p95": 1_000_000_000 / 140_000_000,
        }
        wait = main_loop["simulated_wait_time"]
        for field in ("p50", "p95", "p99", "max"):
            wait[field] = 0
        for window in wait["windows"]:
            window["p95_ns"] = 0
        benchmark.validate_record(record)

    def test_accepts_mixed_zero_wait_windows_for_an_over_budget_phase(self) -> None:
        record = native_record()
        main_loop = record["phases"][1]["main_loop"]
        work = timing(
            480,
            p50_ns=125_000_000,
            p95_ns=140_000_000,
            first_ns=140_000_000,
            last_ns=140_000_000,
        )
        record["phases"][1]["frame_time"] = copy.deepcopy(work)
        main_loop["work_time"] = copy.deepcopy(work)
        main_loop["simulated_update_loop_time"] = copy.deepcopy(work)
        main_loop["work_capacity_fps"] = {
            "p50": 8.0,
            "p95": 1_000_000_000 / 140_000_000,
        }
        wait = main_loop["simulated_wait_time"]
        wait["p50"] = 0
        wait["windows"][0]["p95_ns"] = 0
        benchmark.validate_record(record)

    def test_rejects_timing_window_above_global_maximum(self) -> None:
        malformed = native_record()
        malformed["phases"][1]["frame_time"]["windows"][0]["p95_ns"] = 5_000_000
        with self.assertRaisesRegex(ValueError, "window exceeds its maximum"):
            benchmark.validate_record(malformed)

    def test_rejects_noncanonical_timing_window_partition(self) -> None:
        malformed = native_record()
        windows = malformed["phases"][3]["frame_time"]["windows"]
        windows[0]["samples"] = 16
        windows[1]["start_tick"] = 16
        windows[2]["start_tick"] = 48
        windows[2]["samples"] = 32
        with self.assertRaisesRegex(ValueError, "window partition"):
            benchmark.validate_record(malformed)

    @mock.patch.object(benchmark.subprocess, "run")
    def test_expected_revision_rejects_dirty_or_unknown_source(
        self, run: mock.Mock
    ) -> None:
        for dirty, dirty_known in ((True, True), (None, False)):
            with self.subTest(dirty=dirty, dirty_known=dirty_known):
                record = native_record()
                record["identity"]["implementation"].update(
                    {"dirty": dirty, "dirty_known": dirty_known}
                )
                run.return_value = subprocess.CompletedProcess(
                    [], 0, json.dumps(record) + "\n", ""
                )
                with self.assertRaisesRegex(
                    benchmark.BenchmarkError, "clean expected source"
                ):
                    benchmark.run_benchmark(
                        Path("candidate"), Path("manifest.xml"), "standard", "a" * 40
                    )

    def test_rejects_lifecycle_order_or_repeat_difference(self) -> None:
        malformed = native_record()
        malformed["checkpoints"][0]["name"] = "transition"
        with self.assertRaisesRegex(ValueError, "checkpoint.*order"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["same_process_checkpoints"][4]["pixels_sha256"] = "f" * 64
        with self.assertRaisesRegex(ValueError, "lifecycle checkpoints are not deterministic"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["final_state_digest"] = "f" * 16
        malformed["same_process_final_state_digest"] = "f" * 16
        with self.assertRaisesRegex(ValueError, "final state does not match its transition"):
            benchmark.validate_record(malformed)

    def test_rejects_repeatable_intermediate_visual_change_against_golden(self) -> None:
        malformed = native_record()
        malformed["checkpoints"][1]["pixels_sha256"] = "f" * 64
        malformed["same_process_checkpoints"] = copy.deepcopy(malformed["checkpoints"])
        with self.assertRaisesRegex(ValueError, "does not cover its visual lifecycle"):
            benchmark.validate_record(malformed)

        changed_digest = visual_lifecycle_digest(malformed["checkpoints"])
        malformed["checkpoint_sha256"] = changed_digest
        malformed["same_process_checkpoint_sha256"] = changed_digest
        with self.assertRaisesRegex(ValueError, "does not match its fixture golden"):
            benchmark.validate_record(malformed)

    def test_rejects_lifecycle_geometry_and_mode_mismatch(self) -> None:
        malformed = native_record()
        malformed["checkpoints"][1]["map_x"] = 12
        malformed["same_process_checkpoints"][1]["map_x"] = 12
        with self.assertRaisesRegex(ValueError, "checkpoint geometry"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["identity"]["run"]["mode"] = "discrete"
        with self.assertRaisesRegex(ValueError, "mode and fixture"):
            benchmark.validate_record(malformed)

    def test_rejects_unpinned_viewport_or_workload_identity(self) -> None:
        malformed = native_record(viewport="large")
        malformed["identity"]["run"]["viewport"]["width"] = 320
        with self.assertRaisesRegex(ValueError, "viewport identity"):
            benchmark.validate_record(malformed)
        for field, value in (
            ("look_width", 19),
            ("resize_height_delta", 25),
            ("rng_seed", 196),
        ):
            with self.subTest(field=field):
                malformed = native_record(viewport="large")
                malformed["fixture"][field] = value
                with self.assertRaisesRegex(ValueError, "fixture workload identity"):
                    benchmark.validate_record(malformed)

    def test_rejects_unavailable_nonzero_renderer_allocations(self) -> None:
        malformed = native_record()
        malformed["phases"][0]["map"]["renderer_allocations"] = 1
        with self.assertRaisesRegex(ValueError, "map accounting"):
            benchmark.validate_record(malformed)

    def test_rejects_inconsistent_cache_occupancy_and_peaks(self) -> None:
        malformed = native_record()
        malformed["phases"][0]["lighting"]["end"]["lit_sprite_entries"] += 1
        malformed["phases"][0]["lighting"]["peak"]["lit_sprite_entries"] += 1
        with self.assertRaisesRegex(ValueError, "aggregate state"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["lighting"]["levels"][6]["start"]["entries"] += 1
        malformed["phases"][0]["lighting"]["levels"][6]["peak"]["entries"] += 1
        with self.assertRaisesRegex(ValueError, "aggregate state"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        start = malformed["phases"][0]["lighting"]["levels"][6]["start"]
        start.update(
            {
                "allocated": False,
                "cache_valid": False,
                "dirty": False,
                "entries": 0,
                "bytes": 0,
                "retained_field_bytes": 0,
            }
        )
        with self.assertRaisesRegex(ValueError, "aggregate state"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["lighting"]["levels"][6]["peak"]["bytes"] = 9_999
        with self.assertRaisesRegex(ValueError, "aggregate peak"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["lighting"]["levels"][6]["counters"][
            "field_begins"
        ] += 1
        with self.assertRaisesRegex(ValueError, "per-level counters"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["sprite_cache"]["peak"]["estimated_bytes"] = 1
        with self.assertRaisesRegex(ValueError, "sprite peak"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["sprite_cache"]["limits"]["entries"] = 3
        with self.assertRaisesRegex(ValueError, "sprite occupancy"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["sprite_cache"]["peak"]["estimated_bytes"] = 67_108_865
        with self.assertRaisesRegex(ValueError, "sprite occupancy"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        counters = malformed["phases"][0]["sprite_cache"]["counters"]
        counters["misses"] += 1
        counters["lookups"] += 1
        with self.assertRaisesRegex(ValueError, "sprite occupancy"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["sprite_cache"]["counters"]["gc_removals"] = 99
        with self.assertRaisesRegex(ValueError, "sprite occupancy"):
            benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["sprite_cache"]["counters"]["evictions"] = 1
        malformed["phases"][0]["sprite_cache"]["end"]["entries"] = 3
        benchmark.validate_record(malformed)
        malformed = native_record()
        malformed["phases"][0]["sprite_cache"]["end"]["entries"] += 1
        malformed["phases"][0]["sprite_cache"]["peak"]["entries"] += 1
        with self.assertRaisesRegex(ValueError, "sprite occupancy"):
            benchmark.validate_record(malformed)

    def test_rejects_lighting_render_failures_and_fallbacks(self) -> None:
        for field in ("render_failures", "lit_sprite_fallbacks"):
            with self.subTest(scope="aggregate", field=field):
                malformed = native_record()
                malformed["phases"][1]["lighting"]["counters"][field] = 1
                with self.assertRaisesRegex(ValueError, "rendering failed"):
                    benchmark.validate_record(malformed)
            with self.subTest(scope="level", field=field):
                malformed = native_record()
                malformed["phases"][1]["lighting"]["levels"][6]["counters"][field] = 1
                with self.assertRaisesRegex(ValueError, "level rendering failed"):
                    benchmark.validate_record(malformed)

    def test_requires_mode_coherent_lighting_telemetry(self) -> None:
        smooth = native_record()
        smooth["phases"][1]["render_stages"]["lighting"]["calls"] = 0
        with self.assertRaisesRegex(ValueError, "smooth lighting is incomplete"):
            benchmark.validate_record(smooth)
        smooth = native_record()
        smooth["phases"][1]["lighting"]["counters"]["field_reuses"] += 1
        smooth["phases"][1]["lighting"]["levels"][6]["counters"][
            "field_reuses"
        ] += 1
        with self.assertRaisesRegex(ValueError, "smooth lighting is incomplete"):
            benchmark.validate_record(smooth)

        discrete = native_record(mode="discrete")
        benchmark.validate_record(discrete)
        discrete["phases"][1]["render_stages"]["lighting"].update(
            {"calls": 1, "elapsed": 1}
        )
        with self.assertRaisesRegex(ValueError, "discrete lighting is active"):
            benchmark.validate_record(discrete)

        discrete = native_record(mode="discrete")
        timing_value = discrete["phases"][1]["lighting"]["timings"]["translation"]
        timing_value.update({"calls": 1, "elapsed": 1})
        level_timing = discrete["phases"][1]["lighting"]["levels"][6]["timings"][
            "translation"
        ]
        level_timing.update({"calls": 1, "elapsed": 1})
        with self.assertRaisesRegex(ValueError, "discrete lighting is active"):
            benchmark.validate_record(discrete)

        discrete = native_record(mode="discrete")
        lighting_work = discrete["phases"][1]["lighting_work_time"]
        lighting_work.update({"p50": 1, "p95": 1, "p99": 1, "max": 1})
        for window in lighting_work["windows"]:
            window["p95_ns"] = 1
        with self.assertRaisesRegex(ValueError, "discrete lighting is active"):
            benchmark.validate_record(discrete)

    def test_requires_exhaustive_full_rebuild_causes_and_coherent_timings(self) -> None:
        malformed = native_record(reconstruction="full", workload_variant="isolated-lighting")
        malformed["phases"][1]["lighting"]["counters"][
            "field_full_rebuild_control"
        ] -= 1
        with self.assertRaisesRegex(ValueError, "full rebuild cause is incomplete"):
            benchmark.validate_record(malformed)

        malformed = native_record()
        malformed["phases"][1]["lighting"]["timings"]["translation"]["elapsed"] = 0
        with self.assertRaisesRegex(ValueError, "timing is contradictory"):
            benchmark.validate_record(malformed)

    def test_requires_eviction_work_in_the_invalidation_bucket(self) -> None:
        malformed = native_record()
        phase = malformed["phases"][2]["lighting"]
        phase["counters"]["lit_sprite_evictions"] = 1
        phase["levels"][6]["counters"]["lit_sprite_evictions"] = 1
        with self.assertRaisesRegex(ValueError, "lighting timing is incomplete"):
            benchmark.validate_record(malformed)
        phase["timings"]["sprite_invalidation"].update({"calls": 1, "elapsed": 100})
        phase["levels"][6]["timings"]["sprite_invalidation"].update(
            {"calls": 1, "elapsed": 100}
        )
        benchmark.validate_record(malformed)

    def test_rejects_incomplete_raster_translation_and_scroll_telemetry(self) -> None:
        for field, value, message in (
            ("field_rasterized_quads", 0, "lighting timing is incomplete"),
            ("field_translated_bytes", 5, "exercise every eligible translation"),
            ("field_scroll_x_pixels", 0, "isolated scroll offsets are incomplete"),
            ("field_scroll_y_pixels", 0, "isolated scroll offsets are incomplete"),
        ):
            malformed = native_record(workload_variant="isolated-lighting")
            lighting = malformed["phases"][1]["lighting"]
            lighting["counters"][field] = value
            lighting["levels"][6]["counters"][field] = value
            for level in lighting["levels"]:
                if level is not lighting["levels"][6]:
                    level["counters"][field] = 0
            if field == "field_rasterized_quads":
                lighting["timings"]["rasterization"].update({"calls": 0, "elapsed": 0})
                for level in lighting["levels"]:
                    level["timings"]["rasterization"].update({"calls": 0, "elapsed": 0})
            with self.assertRaisesRegex(ValueError, message):
                benchmark.validate_record(malformed)


class EvidenceTests(unittest.TestCase):
    def test_phase_summary_preserves_real_fps_and_readable_telemetry(self) -> None:
        summary = benchmark.phase_summary([native_record()], "sustained")
        self.assertEqual(summary["ticks_per_run"], 480)
        self.assertEqual(summary["update_cadence_hz"], 8)
        self.assertEqual(summary["update_interval_ms"], 125)
        self.assertEqual(summary["render_reference_fps"], 144)
        self.assertEqual(summary["render_reference_budget_ms"], 6.944)
        self.assertEqual(summary["work_capacity_fps_p50"], 1000.0)
        self.assertEqual(summary["map_p95_ms"], 2.0)
        self.assertEqual(summary["local_minimap_p95_ms"], 2.0)
        self.assertEqual(summary["lighting_work_p95_ms"], 0.6)
        self.assertEqual(summary["work_p95_ms"], 2.0)
        self.assertEqual(summary["map"]["primary_map_draws"], 480)
        self.assertEqual(summary["map"]["auxiliary_map_draws"], 240)
        self.assertEqual(summary["map"]["presents"], 0)
        self.assertEqual(summary["queue"]["processing_ms"], 2400.0)
        self.assertEqual(summary["queue"]["drain_p50_ms"], 1.0)
        self.assertTrue(summary["queue"]["order_digests_comparable"])
        self.assertEqual(summary["lighting"]["field_dirty_pixels"], 2_457_600)
        self.assertEqual(summary["lighting"]["levels"][6]["depth"], 0)
        self.assertEqual(summary["lighting"]["levels"][6]["dirty_ratio_percent"], 1.33)
        self.assertEqual(
            summary["lighting"]["timings"]["translation"]["calls_per_run"], 2400
        )
        self.assertEqual(
            summary["render_stages"]["map"],
            {"scope": "per_map_draw", "calls_per_run": 720, "avg_ms_per_call": 0.001},
        )
        self.assertEqual(summary["render_stages"]["lighting"]["scope"], "per_level")
        self.assertEqual(summary["render_stages"]["lighting"]["calls_per_run"], 2400)
        resources = benchmark.resource_summary([native_record(), native_record()])
        self.assertEqual(resources["process_peak_rss_median_bytes"], 1024)
        self.assertEqual(resources["sprite_cache_end_entries"], 4)
        self.assertEqual(resources["sprite_cache_peak_bytes"], 1024)
        self.assertEqual(resources["sprite_cache_gc_removals"], 0)

    def test_zero_call_render_stage_is_not_reported_as_zero_cost(self) -> None:
        record = native_record(mode="discrete")
        summary = benchmark.phase_summary([record], "sustained")
        self.assertEqual(summary["render_stages"]["lighting"]["calls_per_run"], 0)
        self.assertIsNone(summary["render_stages"]["lighting"]["avg_ms_per_call"])

    def test_window_guard_uses_actual_first_and_last_windows(self) -> None:
        candidate = native_record(first_window_ns=1_000_000, last_window_ns=1_110_000)
        check = benchmark._build_evidence(
            [native_record()], [candidate], [], additional_contexts()
        )["checks"]["candidate_sustained_window_p95"]
        self.assertEqual(check["first_window_ns"], 1_000_000)
        self.assertEqual(check["last_window_ns"], 1_110_000)
        self.assertFalse(check["passed"])

    def test_injected_delay_fails_absolute_and_window_latency_guards(self) -> None:
        delayed = native_record(
            sustained_p95_ns=50_000_000,
            first_window_ns=2_000_000,
            last_window_ns=3_000_000,
        )
        evidence = benchmark._build_evidence(
            [],
            [delayed, copy.deepcopy(delayed)],
            [],
            additional_contexts(),
            enforce_performance=False,
            comparison_note="event-has-no-comparison-base",
        )
        absolute = evidence["checks"]["candidate_sustained_p95"]
        window = evidence["checks"]["candidate_sustained_window_p95"]
        self.assertFalse(absolute["passed"])
        self.assertFalse(window["passed"])
        self.assertFalse(absolute["enforced"])
        self.assertFalse(window["enforced"])
        self.assertTrue(evidence["checks"]["full_redraw_accounting"]["passed"])

    def test_injected_cache_generation_churn_fails_only_cache_guard(self) -> None:
        churned = native_record()
        sustained = churned["phases"][1]
        sustained["lighting"]["counters"]["field_dirty_marks"] = 10_000
        sustained["lighting"]["counters"]["field_rebuilds"] = 10_000
        evidence = benchmark._build_evidence(
            [],
            [churned, copy.deepcopy(churned)],
            [],
            additional_contexts(),
            enforce_performance=False,
            comparison_note="event-has-no-comparison-base",
        )
        self.assertFalse(evidence["checks"]["lighting_cache_churn"]["passed"])
        self.assertFalse(evidence["checks"]["lighting_cache_churn"]["enforced"])
        self.assertTrue(evidence["checks"]["noop_redraw_avoidance"]["passed"])
        self.assertTrue(evidence["checks"]["full_redraw_accounting"]["passed"])

    def test_translated_partial_lighting_passes_without_whole_field_reuse(self) -> None:
        translated = native_record()
        sustained = translated["phases"][1]
        counters = sustained["lighting"]["counters"]
        counters["field_reuses"] = 0
        counters["field_rebuilds"] = 2_400
        counters["field_dirty_marks"] = 2_400
        counters["field_translations"] = 2_400
        counters["field_partial_rebuilds"] = 2_380
        counters["field_full_rebuilds"] = 20
        counters["field_dirty_pixels"] = 93_765_760
        guard = benchmark._guard_native_record(translated)["lighting_cache_churn"]
        self.assertTrue(guard["passed"])
        self.assertEqual(guard["field_translations"], 2_400)
        self.assertEqual(guard["field_partial_rebuilds"], 2_380)

        counters["field_dirty_pixels"] = 480 * 5 * 320 * 240
        self.assertFalse(
            benchmark._guard_native_record(translated)["lighting_cache_churn"][
                "passed"
            ]
        )

    def test_reconstruction_equivalence_requires_the_same_candidate_contract(self) -> None:
        translated = native_record(workload_variant="isolated-lighting")
        full = native_record(reconstruction="full", workload_variant="isolated-lighting")
        self.assertTrue(benchmark._reconstruction_equivalence([translated], [full, full])["passed"])
        changed = copy.deepcopy(full)
        changed["fixture"]["manifest_sha256"] = "f" * 64
        check = benchmark._reconstruction_equivalence([translated], [full, changed])
        self.assertFalse(check["identities_match"])
        self.assertFalse(check["passed"])

        changed = copy.deepcopy(full)
        changed["phases"][1]["lighting"]["counters"]["field_scroll_x_pixels"] += 1
        check = benchmark._reconstruction_equivalence([translated], [full, changed])
        self.assertFalse(check["scroll_offsets_match"])
        self.assertFalse(check["passed"])

    def test_incidental_lighting_reuse_does_not_mask_full_rebuilds(self) -> None:
        regressed = native_record()
        counters = regressed["phases"][1]["lighting"]["counters"]
        counters["field_translations"] = 0
        counters["field_partial_rebuilds"] = 0
        self.assertFalse(
            benchmark._guard_native_record(regressed)["lighting_cache_churn"][
                "passed"
            ]
        )

    def test_incidental_partial_rebuild_does_not_mask_full_rebuilds(self) -> None:
        regressed = native_record()
        counters = regressed["phases"][1]["lighting"]["counters"]
        counters["field_reuses"] = 0
        counters["field_rebuilds"] = 2_400
        counters["field_dirty_marks"] = 2_400
        counters["field_translations"] = 1
        counters["field_partial_rebuilds"] = 1
        counters["field_dirty_pixels"] = 320 * 240 * 2_400 - 1
        self.assertFalse(
            benchmark._guard_native_record(regressed)["lighting_cache_churn"][
                "passed"
            ]
        )

    def test_injected_noop_full_redraw_fails_noop_redraw_guard(self) -> None:
        redrawn = native_record()
        redrawn["phases"][2]["draw_reasons"]["packet"] = 1
        evidence = benchmark._build_evidence(
            [],
            [redrawn, copy.deepcopy(redrawn)],
            [],
            additional_contexts(),
            enforce_performance=False,
            comparison_note="event-has-no-comparison-base",
        )
        self.assertFalse(evidence["checks"]["noop_redraw_avoidance"]["passed"])
        self.assertTrue(evidence["checks"]["noop_redraw_avoidance"]["enforced"])
        self.assertTrue(evidence["checks"]["full_redraw_accounting"]["passed"])

    def test_injected_slower_candidate_fails_only_relative_latency_guard(self) -> None:
        baseline = native_record(sustained_p95_ns=2_000_000)
        slower = native_record(sustained_p95_ns=2_300_000)
        evidence = benchmark._build_evidence(
            [baseline, copy.deepcopy(baseline)],
            [slower, copy.deepcopy(slower)],
            [],
            additional_contexts(),
            enforce_performance=False,
            comparison_note=benchmark.COMPARE_FOUNDATION_NOTE,
        )
        self.assertFalse(evidence["checks"]["base_candidate_sustained_p95"]["passed"])
        self.assertFalse(evidence["checks"]["base_candidate_sustained_p95"]["enforced"])
        self.assertTrue(evidence["checks"]["candidate_sustained_p95"]["passed"])
        self.assertTrue(evidence["checks"]["full_redraw_accounting"]["passed"])

    def test_cross_contract_comparison_keeps_candidate_guards_enforced(self) -> None:
        baseline = native_record(sustained_p95_ns=2_000_000)
        baseline["schema_version"] = 5
        baseline["identity"]["instrumentation"]["schema_version"] = 5
        for phase in baseline["phases"]:
            del phase["render_stages"]
            del phase["lighting_work_time"]
            del phase["lighting"]["timings"]
            for level in phase["lighting"]["levels"]:
                del level["timings"]
                del level["width"]
                del level["height"]
        candidate = native_record(sustained_p95_ns=2_300_000)
        evidence = benchmark._build_evidence(
            [baseline, copy.deepcopy(baseline), copy.deepcopy(baseline)],
            [candidate, copy.deepcopy(candidate), copy.deepcopy(candidate)],
            [],
            additional_contexts(smooth_samples=3),
            enforce_performance=False,
            comparison_note=benchmark.CROSS_CONTRACT_NOTE,
        )
        self.assertEqual(evidence["mode"], "comparison")
        self.assertFalse(evidence["checks"]["base_candidate_sustained_p95"]["enforced"])
        self.assertFalse(evidence["checks"]["checkpoint"]["enforced"])
        self.assertFalse(evidence["checks"]["instrumentation_identity"]["enforced"])
        self.assertFalse(evidence["checks"]["instrumentation_identity"]["passed"])
        self.assertTrue(evidence["checks"]["full_redraw_accounting"]["enforced"])
        report = benchmark._render_complete_evidence(
            evidence, "success", lambda value: value
        )
        self.assertIn("cross-contract comparison", report)
        self.assertIn("alternated on the same runner", report)
        self.assertIn("Map render path p95 (contract-specific)", report)
        self.assertIn("base and candidate identities are not comparable", report)
        self.assertIn("n/a → 0.001 ms", report)
        self.assertEqual(
            evidence["phases"]["baseline_standard"]["sustained"][
                "lighting_work_p95_ms"
            ],
            0,
        )
        self.assertFalse(
            evidence["phases"]["baseline_standard"]["sustained"][
                "lighting_work_available"
            ]
        )
        self.assertNotIn(
            "timings",
            evidence["phases"]["baseline_standard"]["sustained"]["lighting"],
        )
        for heading, candidate_calls in (
            ("### Render-profiler stages (standard smooth sustained)", 720),
            ("### Render-profiler stages (standard smooth cold)", 2),
            ("### Render-profiler stages (standard smooth idle)", 16),
            ("### Render-profiler stages (standard smooth resumed)", 120),
        ):
            with self.subTest(heading=heading):
                start = report.index(heading)
                end = report.find("\n###", start + len(heading))
                section = report[start : end if end != -1 else len(report)]
                self.assertIn(
                    f"| `map` | `per_map_draw` | n/a → {candidate_calls} | "
                    "n/a → 0.001 ms | n/a |",
                    section,
                )

    def test_informational_performance_failure_does_not_hide_or_fail(self) -> None:
        slow = native_record(sustained_p95_ns=50_000_000)
        evidence = benchmark._build_evidence(
            [], [slow, copy.deepcopy(slow)], [], additional_contexts(),
            enforce_performance=False,
            comparison_note="bootstrap-base-missing-movement-instrumentation",
        )
        check = evidence["checks"]["candidate_sustained_p95"]
        self.assertFalse(check["passed"])
        self.assertFalse(check["enforced"])
        self.assertFalse(evidence["failed"])
        self.assertEqual(evidence["status"], "passed")

    def test_correctness_failure_remains_enforced_in_foundation_mode(self) -> None:
        bad = native_record()
        bad["phases"][2]["map"]["render_failures"] = 1
        evidence = benchmark._build_evidence(
            [], [bad, copy.deepcopy(bad)], [], additional_contexts(),
            enforce_performance=False,
            comparison_note="baseline-movement-schema-mismatch",
        )
        check = evidence["checks"]["full_redraw_accounting"]
        self.assertFalse(check["passed"])
        self.assertTrue(check["enforced"])
        self.assertTrue(evidence["failed"])

    def test_noop_correctness_is_enforced_while_lighting_is_informational(self) -> None:
        record = native_record()
        record["phases"][2]["draw_reasons"]["packet"] = 1
        record["phases"][1]["lighting"]["counters"]["field_reuses"] = 0
        evidence = benchmark._build_evidence(
            [], [record, copy.deepcopy(record)], [], additional_contexts(),
            enforce_performance=False,
            comparison_note="bootstrap-base-missing-movement-instrumentation",
        )
        self.assertFalse(evidence["checks"]["noop_redraw_avoidance"]["passed"])
        self.assertTrue(evidence["checks"]["noop_redraw_avoidance"]["enforced"])
        self.assertFalse(evidence["checks"]["lighting_cache_churn"]["enforced"])
        self.assertTrue(evidence["failed"])

    def test_resource_plateau_remains_enforced_in_foundation_mode(self) -> None:
        record = native_record()
        record["phases"][3]["sprite_cache"]["peak"]["estimated_bytes"] *= 2
        evidence = benchmark._build_evidence(
            [],
            [record, copy.deepcopy(record)],
            [],
            additional_contexts(),
            enforce_performance=False,
            comparison_note="event-has-no-comparison-base",
        )
        plateau = evidence["checks"]["cache_memory_plateau"]
        self.assertFalse(plateau["passed"])
        self.assertTrue(plateau["enforced"])
        self.assertTrue(evidence["failed"])

    def test_queue_and_cache_guards_fail_independently(self) -> None:
        record = native_record()
        self.assertTrue(all(item["passed"] for item in benchmark._guard_native_record(record).values()))
        queue_bad = copy.deepcopy(record)
        queue_bad["phases"][3]["queue"]["recoveries"] = 0
        self.assertFalse(benchmark._guard_native_record(queue_bad)["queue_plateau_recovery"]["passed"])
        cache_bad = copy.deepcopy(record)
        cache_bad["phases"][1]["lighting"]["counters"]["field_rebuilds"] = 10_000
        self.assertFalse(benchmark._guard_native_record(cache_bad)["lighting_cache_churn"]["passed"])
        memory_bad = copy.deepcopy(record)
        memory_bad["phases"][3]["sprite_cache"]["peak"]["estimated_bytes"] *= 2
        self.assertFalse(
            benchmark._guard_native_record(memory_bad)["cache_memory_plateau"]["passed"]
        )

    def test_candidate_only_retains_raw_records_and_note(self) -> None:
        records = [
            native_record(),
            native_record(),
            native_record(workload_variant="isolated-lighting"),
            native_record(reconstruction="full", workload_variant="isolated-lighting"),
            native_record(reconstruction="full", workload_variant="isolated-lighting"),
            native_record(workload_variant="isolated-lighting"),
            *discrete_pair(),
        ]
        with mock.patch.object(benchmark, "run_benchmark", side_effect=records) as run:
            evidence = benchmark.candidate_only(
                Path("client"), Path("manifest"), discrete_manifest=Path("discrete.xml"),
                lighting_manifest=Path("lighting.xml"),
                enforce_performance=False,
                comparison_note="event-has-no-comparison-base",
            )
        self.assertEqual(
            [call.args[2] for call in run.call_args_list],
            ["standard"] * 8,
        )
        self.assertEqual(evidence["comparison_note"], "event-has-no-comparison-base")
        self.assertEqual(len(evidence["records"]["candidate_standard"]), 2)
        self.assertEqual(
            len(evidence["records"]["additional_contexts"]["standard_discrete"]), 2
        )

    def test_full_matrix_uses_fresh_pairs_for_both_large_modes(self) -> None:
        records = [
            native_record(),
            native_record(),
            native_record(workload_variant="isolated-lighting"),
            native_record(reconstruction="full", workload_variant="isolated-lighting"),
            native_record(reconstruction="full", workload_variant="isolated-lighting"),
            native_record(workload_variant="isolated-lighting"),
            *discrete_pair(),
            native_record(viewport="large"),
            native_record(viewport="large"),
            native_record(viewport="large", workload_variant="isolated-lighting"),
            native_record(viewport="large", reconstruction="full", workload_variant="isolated-lighting"),
            native_record(viewport="large", reconstruction="full", workload_variant="isolated-lighting"),
            native_record(viewport="large", workload_variant="isolated-lighting"),
            native_record(mode="discrete", viewport="large"),
            native_record(mode="discrete", viewport="large"),
        ]
        with mock.patch.object(
            benchmark, "run_benchmark", side_effect=records
        ) as run:
            evidence = benchmark.candidate_only(
                Path("client"),
                Path("smooth.xml"),
                discrete_manifest=Path("discrete.xml"),
                lighting_manifest=Path("lighting.xml"),
                full_matrix=True,
            )
        self.assertEqual(
            [call.args[2] for call in run.call_args_list],
            ["standard"] * 8 + ["large"] * 8,
        )
        self.assertEqual(evidence["samples"]["candidate_large"], 2)
        self.assertTrue(evidence["checks"]["candidate_large_determinism"]["passed"])

    def test_evidence_requires_exact_discrete_context_matrix(self) -> None:
        candidate = [native_record(), native_record()]
        for contexts in ({}, {"unknown": discrete_pair()}):
            with self.subTest(contexts=set(contexts)):
                with self.assertRaisesRegex(
                    benchmark.BenchmarkError, "incomplete context matrix"
                ):
                    benchmark._build_evidence([], candidate, [], contexts)
        with self.assertRaisesRegex(benchmark.BenchmarkError, "invalid run count"):
            benchmark._build_evidence(
                [],
                candidate,
                [],
                {
                    benchmark.STANDARD_DISCRETE_CONTEXT: discrete_pair()[:1],
                    benchmark.STANDARD_TRANSLATED_CONTEXT: [
                        native_record(workload_variant="isolated-lighting"),
                        native_record(workload_variant="isolated-lighting"),
                    ],
                    benchmark.STANDARD_FULL_CONTEXT: [
                        native_record(reconstruction="full", workload_variant="isolated-lighting"),
                        native_record(reconstruction="full", workload_variant="isolated-lighting"),
                    ],
                },
            )

    def test_compare_foundation_policy_requires_exact_note_pairing(self) -> None:
        baseline = [native_record(), native_record()]
        candidate = [native_record(), native_record()]
        contexts = additional_contexts()
        for enforced, note in ((False, None), (True, benchmark.COMPARE_FOUNDATION_NOTE)):
            with self.subTest(enforced=enforced, note=note):
                with self.assertRaisesRegex(
                    benchmark.BenchmarkError, "performance policy"
                ):
                    benchmark._build_evidence(
                        baseline,
                        candidate,
                        [],
                        contexts,
                        enforce_performance=enforced,
                        comparison_note=note,
                    )

    def test_determinism_requires_two_fresh_processes(self) -> None:
        self.assertFalse(benchmark._context_consistency([native_record()])["passed"])
        self.assertTrue(
            benchmark._context_consistency([native_record(), native_record()])["passed"]
        )

    def test_compare_alternates_runs(self) -> None:
        records = [
            native_record(),
            native_record(),
            native_record(workload_variant="isolated-lighting"),
            native_record(reconstruction="full", workload_variant="isolated-lighting"),
            native_record(),
            native_record(),
            native_record(reconstruction="full", workload_variant="isolated-lighting"),
            native_record(workload_variant="isolated-lighting"),
            native_record(),
            native_record(),
            native_record(workload_variant="isolated-lighting"),
            native_record(reconstruction="full", workload_variant="isolated-lighting"),
            *discrete_pair(),
        ]
        with mock.patch.object(benchmark, "run_benchmark", side_effect=records) as run:
            benchmark.compare(Path("base"), Path("base.xml"), Path("candidate"),
                              Path("candidate.xml"), Path("discrete.xml"),
                              Path("lighting.xml"), 3)
        self.assertEqual(
            [call.args[0].name for call in run.call_args_list[:12]],
            [
                "base", "candidate", "candidate", "candidate",
                "candidate", "base", "candidate", "candidate",
                "base", "candidate", "candidate", "candidate",
            ],
        )

    def test_comparison_accepts_internal_state_digest_changes_when_pixels_match(self) -> None:
        candidate = native_record()
        candidate["final_state_digest"] = "b" * 16
        candidate["same_process_final_state_digest"] = "b" * 16
        for item in candidate["checkpoints"]:
            item["state_digest"] = "b" * 16
        candidate["same_process_checkpoints"] = copy.deepcopy(candidate["checkpoints"])
        check = benchmark._checkpoint_check([native_record()], [candidate])
        self.assertTrue(check["base_candidate_visual_lifecycle_match"])
        self.assertTrue(check["passed"])

    def test_error_evidence_is_bounded_and_versioned(self) -> None:
        evidence = benchmark.error_evidence(benchmark.BenchmarkError("x" * 600))
        self.assertEqual(evidence["schema_version"], 6)
        self.assertEqual(len(evidence["error"]), 500)


class CommentTests(unittest.TestCase):
    def valid_evidence(self) -> dict[str, object]:
        return benchmark._build_evidence(
            [native_record(), native_record(), native_record()],
            [native_record(), native_record(), native_record()],
            [],
            additional_contexts(smooth_samples=3),
        )

    def test_report_separates_update_cadence_from_display_reference(self) -> None:
        report = benchmark.render_comment(self.valid_evidence(), "success")
        self.assertIn("update cadence is not the client display frame rate", report)
        self.assertIn("Measured replay-work capacity FPS (p50/slow-tail)", report)
        self.assertIn("Full map p50/p95", report)
        self.assertIn("Animation pass p50/p95", report)
        self.assertIn("Local minimap map-core p50/p95", report)
        self.assertIn("Attributable lighting movement A/B", report)
        self.assertIn("excludes the separately measured local minimap", report)
        self.assertIn("Display reference", report)
        self.assertIn("8 Hz", report)
        self.assertIn("144 FPS (6.944 ms)", report)
        self.assertIn("informational only", report)
        self.assertIn("Local-minimap calls", report)
        self.assertIn("Presents", report)
        self.assertNotIn("Target FPS", report)
        self.assertNotIn("8.00/8.00", report)
        self.assertIn("1.0 KiB", report)
        self.assertIn("2,457,600", report)
        self.assertIn("unavailable", report)
        self.assertIn("no large-viewport result is claimed", report)
        self.assertIn("Standard discrete", report)
        self.assertIn("Process peak RSS median/max", report)
        self.assertIn("Sprite entries end/peak", report)
        self.assertIn("GC removals", report)
        self.assertIn("Render-profiler stages (standard smooth sustained)", report)
        self.assertIn("Average ms/call (base → candidate)", report)
        self.assertIn("Parent and child scopes overlap and are not additive", report)
        self.assertIn("live profiler buckets", report)
        self.assertIn("`per_level`", report)
        self.assertIn("n/a", report)
        self.assertLessEqual(len(report.encode()), 65_536)
        self.assertNotIn("FPS equivalent", report)

    def test_detailed_standard_smooth_stages_use_matching_baselines(self) -> None:
        baseline = [native_record(), native_record(), native_record()]
        candidate = [native_record(), native_record(), native_record()]
        expected_map_stages = {
            "cold": (2, 1_000, 1_500, "+50.0%"),
            "sustained": (720, 2_000, 2_500, "+25.0%"),
            "idle": (16, 3_000, 3_500, "+16.7%"),
            "resumed": (120, 4_000, 4_500, "+12.5%"),
        }
        for records, elapsed_index in ((baseline, 1), (candidate, 2)):
            for record in records:
                for phase in record["phases"]:
                    calls, baseline_us, candidate_us, _ = expected_map_stages[phase["name"]]
                    phase["render_stages"]["map"]["calls"] = calls
                    phase["render_stages"]["map"]["elapsed"] = (
                        baseline_us if elapsed_index == 1 else candidate_us
                    ) * calls
        evidence = benchmark._build_evidence(
            baseline, candidate, [], additional_contexts(smooth_samples=3)
        )
        report = benchmark.render_comment(evidence, "success")
        for phase, (calls, baseline_us, candidate_us, delta) in expected_map_stages.items():
            with self.subTest(phase=phase):
                heading = f"### Render-profiler stages (standard smooth {phase})"
                start = report.index(heading)
                end = report.find("\n### ", start + len(heading))
                section = report[start : end if end != -1 else len(report)]
                self.assertIn(
                    f"| `map` | `per_map_draw` | {calls} → {calls} | "
                    f"{baseline_us / 1_000:.3f} ms → {candidate_us / 1_000:.3f} ms | "
                    f"{delta} |",
                    section,
                )

        self.assertIn(
            "Those candidate-only contexts do not collect a baseline",
            report,
        )
        self.assertNotIn(f"#### {benchmark.STANDARD_DISCRETE_CONTEXT}", report)

    def test_candidate_only_detailed_stages_remain_unavailable(self) -> None:
        evidence = benchmark._build_evidence(
            [],
            [native_record(), native_record()],
            [],
            additional_contexts(),
            enforce_performance=False,
            comparison_note="bootstrap-base-missing-movement-instrumentation",
        )
        report = benchmark.render_comment(evidence, "success")
        cold_heading = "### Render-profiler stages (standard smooth cold)"
        cold_start = report.index(cold_heading)
        cold_end = report.find("\n### ", cold_start + len(cold_heading))
        cold_section = report[
            cold_start : cold_end if cold_end != -1 else len(report)
        ]
        self.assertIn(
            "| `map` | `per_map_draw` | n/a → 2 | n/a → 0.001 ms | n/a |",
            cold_section,
        )

    def test_full_matrix_comment_fits_github_publication_limit(self) -> None:
        evidence = benchmark._build_evidence(
            [],
            [native_record(), native_record()],
            [native_record(viewport="large"), native_record(viewport="large")],
            additional_contexts(full=True),
            enforce_performance=False,
            comparison_note="event-has-no-comparison-base",
        )
        report = benchmark.render_comment(evidence, "success")
        self.assertLessEqual(len(report.encode()), 65_536)
        self.assertIn("Large | translated", report)
        self.assertIn("uploaded JSON artifact", report)
        self.assertNotIn("### Render-profiler stages (large smooth", report)
        self.assertNotIn(f"#### {benchmark.LARGE_DISCRETE_CONTEXT}", report)

    def test_candidate_only_report_establishes_baseline_without_claiming_delta(self) -> None:
        evidence = benchmark._build_evidence(
            [],
            [native_record(), native_record()],
            [],
            additional_contexts(),
            enforce_performance=False,
            comparison_note="bootstrap-base-missing-movement-instrumentation",
        )
        report = benchmark.render_comment(evidence, "success")
        self.assertIn("establishes the hosted-runner baseline", report)
        self.assertIn("No before/after delta is claimed", report)
        self.assertIn("Before/after summary (standard smooth sustained)", report)
        self.assertIn("Before (base)", report)
        self.assertIn("After (candidate)", report)
        self.assertIn("Unavailable (base predates compatible instrumentation)", report)
        self.assertIn("2.00 ms | Not computed", report)
        self.assertIn("500.00 FPS | Not computed", report)
        self.assertNotIn("Base → candidate change", report)

    def test_report_explicitly_labels_informational_finding(self) -> None:
        slow = native_record(sustained_p95_ns=50_000_000)
        evidence = benchmark._build_evidence(
            [], [slow, copy.deepcopy(slow)], [], additional_contexts(),
            enforce_performance=False,
            comparison_note="bootstrap-base-missing-movement-instrumentation",
        )
        report = benchmark.render_comment(evidence, "success")
        self.assertIn("performance/optimization finding(s) are informational", report)
        self.assertIn("| informational | fail |", report)
        self.assertIn("bootstrap base missing movement instrumentation", report)
        self.assertIn("candidate fresh runs used identical instrumentation", report)

    def test_report_accepts_a_phase_with_no_map_draws(self) -> None:
        records = [native_record(), native_record(), *discrete_pair()]
        evidence = benchmark._build_evidence(
            [],
            records[:2],
            [],
            {
                benchmark.STANDARD_DISCRETE_CONTEXT: records[2:],
                benchmark.STANDARD_TRANSLATED_CONTEXT: [
                    native_record(workload_variant="isolated-lighting"),
                    native_record(workload_variant="isolated-lighting"),
                ],
                benchmark.STANDARD_FULL_CONTEXT: [
                    native_record(reconstruction="full", workload_variant="isolated-lighting"),
                    native_record(reconstruction="full", workload_variant="isolated-lighting"),
                ],
            },
        )
        report = benchmark.render_comment(evidence, "success")
        self.assertIn("0.00/0.00 ms", report)

    def test_report_accepts_evidence_after_sorted_json_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence_path = Path(temporary) / "evidence.json"
            benchmark.write_evidence(evidence_path, self.valid_evidence())
            evidence = json.loads(evidence_path.read_text())
        report = benchmark.render_comment(evidence, "success")
        self.assertIn("Measured replay-work capacity FPS", report)
        self.assertIn("Before/after summary (standard smooth sustained)", report)
        self.assertIn("| Total map-focused update work p95 | 2.00 ms | 2.00 ms | +0.0% |", report)
        self.assertIn("| Full map p95 | 2.00 ms | 2.00 ms | +0.0% |", report)
        self.assertIn("| Local minimap map-core p95 | 2.00 ms | 2.00 ms | +0.0% |", report)
        self.assertIn("| Slow-tail work capacity | 500.00 FPS | 500.00 FPS | +0.0% |", report)
        self.assertIn("Positive timing changes are slower", report)
        self.assertIn("Base → candidate change", report)
        self.assertIn("positive timing deltas mean the candidate was slower", report)

    def test_report_prominently_preserves_overall_client_failure(self) -> None:
        report = benchmark.render_comment(self.valid_evidence(), "failure")
        self.assertIn("Overall Client validation status: `failure`", report)
        self.assertIn("All movement regression checks passed", report)

    def test_report_handles_skipped_error_missing_and_malformed_evidence(self) -> None:
        self.assertIn("was skipped", benchmark.render_comment(
            benchmark.skipped_evidence("not selected"), "success"
        ))
        error = {"schema_version": 6, "status": "error", "error": "untrusted | markdown"}
        error_report = benchmark.render_comment(error, "failure")
        self.assertIn("generation failed", error_report)
        self.assertNotIn("untrusted", error_report)
        self.assertIn("unavailable", benchmark.render_comment(None, "failure"))
        malformed = self.valid_evidence()
        del malformed["phases"]["candidate_standard"]["cold"]["map"]
        self.assertIn("invalid schema", benchmark.render_comment(malformed, "success"))

    def test_report_requires_exact_guard_set_and_enforcement_policy(self) -> None:
        for mutate in (
            lambda checks: checks.pop("checkpoint"),
            lambda checks: checks.update({"unknown_guard": {"passed": True, "enforced": True}}),
            lambda checks: checks["cache_memory_plateau"].update({"enforced": False}),
        ):
            with self.subTest(mutate=mutate):
                evidence = self.valid_evidence()
                mutate(evidence["checks"])
                self.assertIn("invalid schema", benchmark.render_comment(evidence, "success"))

    def test_report_revalidates_raw_records_and_rederives_all_summaries(self) -> None:
        evidence = self.valid_evidence()
        evidence["records"]["candidate_standard"][0] = {}
        self.assertIn("invalid schema", benchmark.render_comment(evidence, "success"))

        for mutate in (
            lambda evidence: evidence["phases"]["candidate_standard"]["sustained"].update(
                {"work_capacity_fps_p50": 9_999.0}
            ),
            lambda evidence: evidence["checks"]["candidate_sustained_p95"].update(
                {"value_ns": evidence["checks"]["candidate_sustained_p95"]["value_ns"] + 1}
            ),
            lambda evidence: evidence["resources"]["candidate_standard"].update(
                {"process_peak_rss_max_bytes": 2_048}
            ),
        ):
            with self.subTest(mutate=mutate):
                evidence = self.valid_evidence()
                mutate(evidence)
                self.assertIn("invalid schema", benchmark.render_comment(evidence, "success"))

        evidence = self.valid_evidence()
        evidence["records"]["candidate_standard"][0]["process_peak_rss_bytes"] = 2_048
        self.assertIn("invalid schema", benchmark.render_comment(evidence, "success"))

    def test_report_requires_builder_sample_contracts(self) -> None:
        comparison = self.valid_evidence()
        comparison["samples"]["baseline_standard"] = 2
        self.assertIn("invalid schema", benchmark.render_comment(comparison, "success"))
        comparison = self.valid_evidence()
        comparison["samples"]["candidate_standard"] = 5
        self.assertIn("invalid schema", benchmark.render_comment(comparison, "success"))

        candidate = benchmark._build_evidence(
            [], [native_record(), native_record()], [], additional_contexts()
        )
        candidate["samples"]["candidate_standard"] = 1
        self.assertIn("invalid schema", benchmark.render_comment(candidate, "success"))

    def test_render_cli_rejects_duplicate_evidence_keys(self) -> None:
        encoded = json.dumps(self.valid_evidence())
        duplicate_documents = (
            encoded[:-1] + ', "status": "passed"}',
            encoded.replace(
                '"samples": {', '"samples": {"candidate_standard": 3,', 1
            ),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence_path = root / "evidence.json"
            report_path = root / "report.md"
            for index, document in enumerate(duplicate_documents):
                with self.subTest(index=index):
                    evidence_path.write_text(document)
                    self.assertEqual(
                        benchmark.main(
                            [
                                "render-comment",
                                "--input",
                                str(evidence_path),
                                "--client-result",
                                "success",
                                "--output",
                                str(report_path),
                            ]
                        ),
                        0,
                    )
                    self.assertIn("invalid schema", report_path.read_text())

    def test_cli_initializes_closed_error_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "evidence.json"
            result = benchmark.main(
                [
                    "error",
                    "--reason", "client-validation-ended-before-movement-evidence",
                    "--output", str(output),
                ]
            )
            self.assertEqual(result, 0)
            self.assertEqual(json.loads(output.read_text())["status"], "error")

    def test_candidate_cli_passes_comparison_note_and_checkpoint_root(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client = root / "client"
            manifest = root / "manifest.xml"
            discrete = root / "discrete.xml"
            client.write_text("x")
            manifest.write_text("x")
            discrete.write_text("x")
            output = root / "evidence.json"
            with mock.patch.object(benchmark, "candidate_only", return_value={
                "schema_version": 6, "status": "passed", "failed": False
            }) as candidate:
                result = benchmark.main(
                    [
                        "candidate-only", "--candidate-client", str(client),
                        "--candidate-manifest", str(manifest), "--output", str(output),
                        "--discrete-manifest", str(discrete),
                        "--lighting-manifest", str(discrete),
                        "--comparison-note", "baseline-movement-schema-mismatch",
                    ]
                )
            self.assertEqual(result, 0)
            self.assertFalse(candidate.call_args.kwargs["enforce_performance"])
            self.assertEqual(candidate.call_args.kwargs["comparison_note"],
                             "baseline-movement-schema-mismatch")
            self.assertEqual(candidate.call_args.args[3], discrete.resolve())
            self.assertEqual(candidate.call_args.kwargs["checkpoint_root"],
                             root / "movement-checkpoints")

    def test_compare_command_publishes_error_evidence_before_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = [
                root / name
                for name in (
                    "base", "base.xml", "base-schema.py", "candidate", "candidate.xml",
                    "discrete.xml",
                )
            ]
            for path in files:
                path.write_text("input")
            files[2].write_text("def validate_record(value):\n    return value\n")
            output = root / "evidence.json"
            with mock.patch.object(
                benchmark, "compare", side_effect=benchmark.BenchmarkError("injected failure")
            ):
                result = benchmark.main(
                    [
                        "compare", "--baseline-client", str(files[0]),
                        "--baseline-manifest", str(files[1]),
                        "--baseline-schema", str(files[2]),
                        "--candidate-client", str(files[3]),
                        "--candidate-manifest", str(files[4]),
                        "--discrete-manifest", str(files[5]), "--output", str(output),
                        "--lighting-manifest", str(files[5]),
                    ]
                )
            self.assertEqual(result, 2)
            self.assertIn("injected failure", json.loads(output.read_text())["error"])

    def test_compare_cli_pairs_foundation_switch_and_note(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = [
                root / name
                for name in (
                    "base", "base.xml", "base-schema.py", "candidate", "candidate.xml",
                    "discrete.xml",
                )
            ]
            for path in files:
                path.write_text("input")
            files[2].write_text("def validate_record(value):\n    return value\n")
            output = root / "evidence.json"
            arguments = [
                "compare",
                "--baseline-client", str(files[0]),
                "--baseline-manifest", str(files[1]),
                "--baseline-schema", str(files[2]),
                "--candidate-client", str(files[3]),
                "--candidate-manifest", str(files[4]),
                "--discrete-manifest", str(files[5]),
                "--lighting-manifest", str(files[5]),
                "--informational-performance",
                "--comparison-note", benchmark.COMPARE_FOUNDATION_NOTE,
                "--output", str(output),
            ]
            with mock.patch.object(
                benchmark,
                "compare",
                return_value={"schema_version": 6, "status": "passed", "failed": False},
            ) as compare:
                self.assertEqual(benchmark.main(arguments), 0)
            self.assertFalse(compare.call_args.kwargs["enforce_performance"])
            self.assertEqual(
                compare.call_args.kwargs["comparison_note"], benchmark.COMPARE_FOUNDATION_NOTE
            )

            unpaired = [item for item in arguments if item != "--informational-performance"]
            self.assertEqual(benchmark.main(unpaired), 2)
            self.assertIn("must be used together", json.loads(output.read_text())["error"])


if __name__ == "__main__":
    unittest.main()
