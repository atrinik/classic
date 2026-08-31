#!/usr/bin/env python3
"""Validate complete, fresh-process GPU qualification JSONL artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import xml.etree.ElementTree as ET
import zlib
from collections import Counter
from pathlib import Path


WORKLOADS = {
    "dense-17x17-five-depth-1080p",
    "dense-25x25-seven-depth-1440p",
    "wire-ceiling-28x28-thirteen-depth-1440p",
    "wire-ceiling-28x28-thirteen-depth-4k",
    "actor-door-roof-animation-25x25",
}
WORKLOAD_CONTRACTS = {
    "dense-17x17-five-depth-1080p": (17, 1920, 1080, 5, False, 64, 20, 25, 10),
    "dense-25x25-seven-depth-1440p": (25, 2560, 1440, 7, False, 64, 20, 25, 12),
    "wire-ceiling-28x28-thirteen-depth-1440p": (28, 2560, 1440, 13, False, 64, 20, 25, 16),
    "wire-ceiling-28x28-thirteen-depth-4k": (28, 3840, 2160, 13, False, 64, 25, 30, 20),
    "actor-door-roof-animation-25x25": (25, 2560, 1440, 7, True, 64, 8, 10, 5),
}
PRODUCTION_FIXTURES = (
    "smooth", "radial-light-smooth", "exit-cues", "living-outline-translucent",
    "living-outline-retained-fow", "timed-light", "map-overlay-linked-depth",
    "map-overlay-widget-state", "map-overlay-elevated", "visibility-fade-centered",
    "brynknot-movement", "gpu-qualification-town-25x25",
    "gpu-ui-closure",
)
UI_CLOSURE_STATES = (
    "intro_server_browser", "login_popup", "popup_character_selection",
    "gameplay_widgets_text_windows", "context_menu_tooltip_notification",
    "notification_fading", "popup_generic_input_controls", "popup_book",
    "popup_settings_controls", "popup_color_picker", "popup_connection_preference",
    "popup_join_password", "popup_credits", "popup_painting", "popup_region_map_minimap",
    "region_map_fow_transition",
    "region_map_fow_retained", "screenshot_window", "screenshot_map",
)
ROOT_GLYPH_CONTRACTS = {
    "intro_server_browser": (383, "29c427a4eff9acbd"),
    "login_popup": (385, "4266544b0b8b6fbd"),
    "popup_character_selection": (385, "4266544b0b8b6fbd"),
}
ASSERTION_ATTRIBUTES = {
    "ui_names_targets": ("player-names", "target-ui"),
    "visibility_fade": ("visibility-fade-test",),
    "map_interaction": ("map-interaction-test",),
    "damage_animation": ("damage-animation",),
    "kill_animation": ("kill-animation",),
    "elevated_animation": ("animation-elevated",),
    "layer_content_animation": ("animation-layer-content",),
}
FIXTURE_ROOT = Path(__file__).resolve().parents[1] / "src/tests/fixtures/player_view"
GOLDEN_CONTRACT = FIXTURE_ROOT / "gpu-approved-goldens.json"
STAGES = {
    "command_build",
    "albedo_owner",
    "light_tone",
    "ui",
    "cpu_submission",
    "gpu_completion_wait",
    "present_wait",
}
MAP_PACING_FIELDS = {
    "submissions", "completions", "in_flight_peak", "queue_depth_samples",
    "queue_depth_total", "queue_age_total_ns", "queue_age_max_ns",
    "frame_latency_total_ns", "frame_latency_max_ns", "dropped_updates",
    "merged_updates", "cpu_recording_calls", "cpu_recording_ns",
    "submission_calls", "submission_ns", "completion_calls", "completion_ns",
    "present_wait_calls", "present_wait_ns",
}
LIFECYCLE_EVENTS = [
    "cold_asset_upload", "resize_grow", "resize_restore", "teleport", "reconnect",
    "foreground_resume", "fullscreen_enter", "fullscreen_leave", "display_migration",
    "swapchain_recreation", "device_loss", "screenshot_readback",
]
HEX_64 = re.compile(r"[0-9a-f]{64}\Z")
REVISION = re.compile(r"[0-9a-f]{40}\Z")


class ArtifactError(ValueError):
    """A qualification artifact violates its versioned contract."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ArtifactError(message)


def _records(path: Path) -> list[dict]:
    records: list[dict] = []
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                raise ArtifactError(f"{path}:{line_number}: invalid JSON: {error}") from error
            _require(isinstance(value, dict), f"{path}:{line_number}: record must be an object")
            records.append(value)
    _require(bool(records), f"{path}: artifact is empty")
    return records


def _png_rgba(contents: bytes, artifact: Path) -> tuple[list[int], bytes]:
    _require(len(contents) >= 33 and contents[:8] == b"\x89PNG\r\n\x1a\n",
             f"review artifact is not PNG: {artifact}")
    position = 8
    dimensions: list[int] | None = None
    compressed = bytearray()
    saw_end = False
    while position < len(contents):
        _require(position + 12 <= len(contents), f"truncated PNG chunk: {artifact}")
        length = int.from_bytes(contents[position:position + 4], "big")
        chunk_end = position + 12 + length
        _require(chunk_end <= len(contents), f"truncated PNG chunk data: {artifact}")
        chunk_type = contents[position + 4:position + 8]
        data = contents[position + 8:position + 8 + length]
        checksum = int.from_bytes(contents[position + 8 + length:chunk_end], "big")
        _require((zlib.crc32(chunk_type + data) & 0xffffffff) == checksum,
                 f"PNG chunk checksum mismatch: {artifact}")
        if chunk_type == b"IHDR":
            _require(dimensions is None and length == 13,
                     f"invalid PNG header: {artifact}")
            dimensions = [int.from_bytes(data[:4], "big"),
                          int.from_bytes(data[4:8], "big")]
            _require(data[8:] == bytes((8, 6, 0, 0, 0)),
                     f"review artifact is not non-interlaced RGBA8: {artifact}")
        elif chunk_type == b"IDAT":
            compressed.extend(data)
        elif chunk_type == b"IEND":
            _require(length == 0 and chunk_end == len(contents),
                     f"invalid PNG end marker: {artifact}")
            saw_end = True
            break
        position = chunk_end
    _require(dimensions is not None and saw_end and bool(compressed),
             f"incomplete PNG: {artifact}")
    width, height = dimensions
    _require(width > 0 and height > 0, f"invalid PNG dimensions: {artifact}")
    try:
        scanlines = zlib.decompress(compressed)
    except zlib.error as error:
        raise ArtifactError(f"invalid PNG image data: {artifact}: {error}") from error
    stride = width * 4
    _require(len(scanlines) == (stride + 1) * height,
             f"invalid PNG scanline length: {artifact}")
    rgba = bytearray()
    previous = bytearray(stride)
    for row_index in range(height):
        offset = row_index * (stride + 1)
        filter_type = scanlines[offset]
        row = bytearray(scanlines[offset + 1:offset + 1 + stride])
        _require(filter_type <= 4, f"unsupported PNG filter: {artifact}")
        for index in range(stride):
            left = row[index - 4] if index >= 4 else 0
            above = previous[index]
            upper_left = previous[index - 4] if index >= 4 else 0
            if filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            elif filter_type == 4:
                candidate = left + above - upper_left
                distances = (abs(candidate - left),
                             abs(candidate - above),
                             abs(candidate - upper_left))
                predictor = (left, above, upper_left)[distances.index(min(distances))]
            else:
                predictor = 0
            row[index] = (row[index] + predictor) & 0xff
        rgba.extend(row)
        previous = row
    return dimensions, bytes(rgba)


def _rgba_sha256(dimensions: list[int], pixels: bytes) -> str:
    encoded_dimensions = b"".join(dimension.to_bytes(4, "big") for dimension in dimensions)
    return hashlib.sha256(encoded_dimensions + pixels).hexdigest()


def _rgba_crop(pixels: bytes,
               source_size: list[int],
               crop: list[int]) -> bytes:
    width, height = source_size
    left, top, crop_width, crop_height = crop
    _require(len(pixels) == width * height * 4 and left >= 0 and top >= 0 and
             crop_width > 0 and crop_height > 0 and left + crop_width <= width and
             top + crop_height <= height,
             "invalid RGBA crop geometry")
    return b"".join(
        pixels[((top + row) * width + left) * 4:
               ((top + row) * width + left + crop_width) * 4]
        for row in range(crop_height)
    )


def _require_ui_screenshot_crop(record: dict, artifacts: dict[str, bytes]) -> None:
    by_name = {state["name"]: state for state in record["ui_closure"]}
    viewport = record["viewport"]
    map_size = by_name["screenshot_map"]["output_size"]
    left = (viewport[0] - map_size[0]) // 2
    top = (viewport[1] - map_size[1]) // 2
    expected = _rgba_crop(artifacts["screenshot_window"],
                          viewport,
                          [left, top, map_size[0], map_size[1]])
    _require(artifacts["screenshot_map"] == expected,
             "map screenshot is not the exact centered crop of the completed window")


def _review_artifact(path: Path,
                     name: object,
                     digest: object,
                     expected_size: object,
                     semantic_digest: object | None = None) -> bytes:
    _require(isinstance(name, str) and name.startswith("review/") and ".." not in name,
             "review artifact path is not closed")
    _require(bool(HEX_64.fullmatch(str(digest))), "review artifact digest is invalid")
    artifact = path.parent / name
    _require(artifact.is_file(), f"review artifact is missing: {artifact}")
    contents = artifact.read_bytes()
    _require(hashlib.sha256(contents).hexdigest() == digest,
             f"review artifact digest mismatch: {artifact}")
    dimensions, pixels = _png_rgba(contents, artifact)
    _require(dimensions == expected_size,
             f"review artifact dimensions mismatch: {artifact}")
    if semantic_digest is not None:
        _require(bool(HEX_64.fullmatch(str(semantic_digest))) and
                 _rgba_sha256(dimensions, pixels) == semantic_digest,
                 f"review artifact pixels mismatch: {artifact}")
    return pixels


def _percentile(values: list[int], percentile: int) -> int:
    ordered = sorted(values)
    return ordered[(len(ordered) * percentile + 99) // 100 - 1]


def _validate_map_pacing(value: dict, label: str) -> None:
    _require(isinstance(value, dict) and set(value) == MAP_PACING_FIELDS,
             f"{label} map-pacing evidence schema is incomplete")
    for field in MAP_PACING_FIELDS:
        _require(type(value[field]) is int and value[field] >= 0,
                 f"{label} map-pacing {field} is invalid")
    _require(value["completions"] <= value["submissions"],
             f"{label} map-pacing completions exceed submissions")
    _require(value["queue_depth_samples"] == value["submissions"],
             f"{label} map-pacing queue samples do not cover submissions")
    if value["submissions"] == 0:
        _require(value["in_flight_peak"] == 0,
                 f"{label} map-pacing in-flight peak is inconsistent")
    else:
        _require(0 < value["in_flight_peak"] <= value["submissions"],
                 f"{label} map-pacing in-flight peak is inconsistent")
    max_queue_depth = max(value["in_flight_peak"] - 1, 0)
    _require(value["queue_depth_total"] <=
             value["queue_depth_samples"] * max_queue_depth,
             f"{label} map-pacing queue depth is unbounded")
    _require(value["queue_age_max_ns"] <= value["queue_age_total_ns"] and
             value["frame_latency_max_ns"] <= value["frame_latency_total_ns"],
             f"{label} map-pacing maxima exceed their totals")


def validate_record(record: dict) -> str:
    _require(record.get("schema_version") == 4, "unsupported schema_version")
    _require(record.get("benchmark") == "gpu-interop-stress-qualification", "wrong benchmark")
    _require(record.get("dirty") is False, "qualified revision must be clean")
    _require(bool(REVISION.fullmatch(str(record.get("revision", "")))), "invalid revision")
    _require(bool(HEX_64.fullmatch(str(record.get("shader_cohort", "")))), "invalid shader cohort")
    gpu = record.get("gpu", {})
    _require(gpu.get("backend") in {"vulkan", "direct3d12", "metal"}, "unsupported backend")
    _require(gpu.get("qualified_hardware") is True, "hardware attestation is absent")
    for field in ("device", "driver_name", "driver_version"):
        _require(isinstance(gpu.get(field), str) and gpu[field] and gpu[field] != "unavailable",
                 f"missing gpu.{field}")
    _require(gpu.get("hardware_tier") in {"reference", "minimum"},
             "missing qualified hardware tier")
    build = record.get("build", {})
    _require(build.get("type") == "Release", "qualification build is not Release")
    for field in ("compiler", "system"):
        _require(isinstance(build.get(field), str) and build[field], f"missing build.{field}")
    workload = record.get("workload", {})
    name = workload.get("name")
    _require(name in WORKLOADS, "unknown workload")
    logical, width, height, depths, animation, actors, p95_ms, p99_ms, stretch_ms = \
        WORKLOAD_CONTRACTS[name]
    semantic_fields = {
        "redraw_reason_frames", "animated_command_transitions", "command_depth_mask",
        "living_depth_mask", "door_depth_mask", "roof_depth_mask", "frames_with_stretch",
        "frames_with_living", "frames_with_door", "frames_with_roof",
    }
    expected_workload = {
        "name": name,
        "logical_view": logical,
        "window_logical_width": width,
        "window_logical_height": height,
        "render_output_width": width,
        "render_output_height": height,
        "active_depths": depths,
        "animation_only": animation,
        "actors": actors,
        "production_path": True,
        "stretch_exercised": True,
        "animation_path_verified": animation,
    }
    _require(set(workload) == set(expected_workload) | semantic_fields and
             all(workload.get(field) == value for field, value in expected_workload.items()),
             "workload geometry or production semantics do not match the required row")
    for field in semantic_fields:
        _require(isinstance(workload.get(field), int) and workload[field] >= 0,
                 f"invalid workload {field}")
    _require(workload["command_depth_mask"].bit_count() == depths,
             "painter commands do not cover every active depth")
    _require(workload["living_depth_mask"] != 0, "actor painter depth is absent")
    _require(workload["frames_with_stretch"] == 40 and
             workload["frames_with_living"] == 40,
             "stretch/actor semantics were not present in every measured frame")
    if animation:
        _require(workload["redraw_reason_frames"] == 40,
                 "animation frames did not retain the animation-only redraw reason")
        _require(workload["animated_command_transitions"] > 0,
                 "animated painter-command identity did not transition")
        _require(workload["frames_with_door"] == 40 and
                 workload["frames_with_roof"] == 40,
                 "door/roof semantics were not present in every animation frame")
        _require(workload["door_depth_mask"] != 0 and
                 workload["roof_depth_mask"].bit_count() == depths,
                 "door/roof painter depths do not match the animation scene")
    else:
        _require(workload["redraw_reason_frames"] == 0 and
                 workload["animated_command_transitions"] == 0,
                 "static workload reported animation-only evidence")
    cold = record.get("cold", {})
    upload_classes = {"source_uploads", "source_upload_bytes", "instance_uploads",
                      "instance_upload_bytes", "light_uploads", "light_upload_bytes",
                      "slot_uniform_uploads", "slot_uniform_upload_bytes"}
    _require(set(cold) == {"scope", "uploads", "upload_bytes", "resource_creations",
                           "peak_retained_bytes"} | upload_classes,
             "cold evidence schema is incomplete")
    _require(cold.get("scope") == "fresh-process", "row is not process-cold")
    for field in ("uploads", "upload_bytes", "resource_creations", "peak_retained_bytes"):
        _require(isinstance(cold.get(field), int) and cold[field] > 0,
                 f"cold {field} must prove fresh retained work")
    for field in upload_classes:
        _require(isinstance(cold.get(field), int) and cold[field] >= 0,
                 f"cold {field} must be a non-negative integer")
    _require(cold["uploads"] == cold["source_uploads"] + cold["instance_uploads"] +
             cold["light_uploads"] + cold["slot_uniform_uploads"] and
             cold["upload_bytes"] == cold["source_upload_bytes"] +
             cold["instance_upload_bytes"] + cold["light_upload_bytes"] +
             cold["slot_uniform_upload_bytes"],
             "cold uploads do not match classified attribution")
    _require(cold["slot_uniform_uploads"] > 0 and
             cold["slot_uniform_upload_bytes"] >= cold["slot_uniform_uploads"] * 16 and
             cold["slot_uniform_upload_bytes"] <= cold["slot_uniform_uploads"] * 1024,
             "cold slot-uniform uploads exceed the bounded 256-slot chunks")
    frames = record.get("frame_windows_ns")
    _require(isinstance(frames, list) and len(frames) == 40, "frame window must have 40 samples")
    _require(all(isinstance(value, int) and value > 0 for value in frames), "invalid frame sample")
    _require(_percentile(frames, 95) < p95_ms * 1_000_000,
             f"{name} frame p95 exceeds {p95_ms} ms")
    _require(_percentile(frames, 99) < p99_ms * 1_000_000,
             f"{name} frame p99 exceeds {p99_ms} ms")
    summary = record.get("frame_ns", {})
    _require(summary == {"p95": _percentile(frames, 95),
                         "p99": _percentile(frames, 99),
                         "max": max(frames)}, "frame summary does not derive from raw samples")
    stretches = record.get("stretch_frame_windows_ns")
    _require(isinstance(stretches, list) and len(stretches) == 40,
             "stretch frame window must have 40 samples")
    _require(all(isinstance(value, int) and value > 0 for value in stretches),
             "invalid stretch frame sample")
    _require(stretches == frames,
             "stretch windows are not bound to the measured stretch-bearing frames")
    _require(_percentile(stretches, 95) < stretch_ms * 1_000_000,
             f"{name} stretch p95 exceeds {stretch_ms} ms")
    windows = record.get("stage_windows_ns", {})
    _require(set(windows) == STAGES, "stage windows do not match schema")
    for stage, values in windows.items():
        _require(isinstance(values, list) and len(values) == 40, f"{stage} must have 40 samples")
        _require(all(isinstance(value, int) and value >= 0 for value in values),
                 f"invalid {stage} sample")
    fenced_gpu = [windows["cpu_submission"][index] + windows["gpu_completion_wait"][index]
                  for index in range(40)]
    _require(_percentile(fenced_gpu, 95) < 5_000_000,
             "conservative fenced map GPU p95 exceeds 5 ms")
    if name == "dense-17x17-five-depth-1080p":
        _require(_percentile(windows["cpu_submission"], 95) < 3_000_000,
                 "17x17 stretch CPU submission p95 exceeds 3 ms")
        _require(_percentile(windows["gpu_completion_wait"], 95) < 7_000_000,
                 "17x17 stretch GPU completion p95 exceeds 7 ms")
    checkpoint = record.get("checkpoint", {})
    _require(checkpoint.get("algorithm") == "sha256-rgba32-with-dimensions",
             "wrong checkpoint algorithm")
    _require(checkpoint.get("timed") is False, "checkpoint readback entered timed samples")
    _require(bool(HEX_64.fullmatch(str(checkpoint.get("pixels_sha256", "")))),
             "invalid checkpoint digest")
    animation_pixels = checkpoint.get("animation_pixels_sha256")
    _require(isinstance(animation_pixels, list) and len(animation_pixels) == 3,
             "animation checkpoint cohort is incomplete")
    if animation:
        _require(all(bool(HEX_64.fullmatch(str(value))) for value in animation_pixels),
                 "animation checkpoint digest is invalid")
        _require(len(set(animation_pixels)) >= 2,
                 "door/roof/actor animation checkpoint did not change")
    else:
        _require(animation_pixels == ["", "", ""],
                 "static workload has unexpected animation checkpoints")
    steady = record.get("steady_state", {})
    for field in ("source_uploads", "source_upload_bytes", "light_uploads",
                  "light_upload_bytes", "resource_creations", "resource_destructions",
                  "readbacks", "fallbacks"):
        _require(steady.get(field) == 0, f"steady-state {field} must be zero")
    _require(set(steady) == {"uploads", "upload_bytes", "source_uploads",
                             "source_upload_bytes", "instance_uploads",
                             "instance_upload_bytes", "light_uploads",
                             "light_upload_bytes", "slot_uniform_uploads",
                             "slot_uniform_upload_bytes", "resource_creations",
                             "resource_destructions", "readbacks", "commands", "batches",
                             "draws", "retained_bytes", "peak_retained_bytes", "fallbacks",
                             "map_pacing"},
             "steady-state evidence schema is incomplete")
    _validate_map_pacing(steady.get("map_pacing"), "steady-state")
    for field in ("uploads", "upload_bytes", "instance_uploads", "instance_upload_bytes",
                  "slot_uniform_uploads", "slot_uniform_upload_bytes"):
        _require(isinstance(steady.get(field), int) and steady[field] >= 0,
                 f"steady-state {field} must be a non-negative integer")
    _require(steady["uploads"] == steady["instance_uploads"] +
             steady["slot_uniform_uploads"] and
             steady["upload_bytes"] == steady["instance_upload_bytes"] +
             steady["slot_uniform_upload_bytes"],
             "steady-state uploads are not classified instance/slot data")
    if animation:
        _require(steady["instance_uploads"] <= len(frames) * 64 and
                 steady["instance_upload_bytes"] <= len(frames) * 4096,
                 "animated instance deltas exceed the bounded upload budget")
    else:
        _require(steady["instance_uploads"] == 0 and steady["instance_upload_bytes"] == 0,
                 "unchanged painter records performed instance uploads")
    for field in ("commands", "batches", "draws", "retained_bytes", "peak_retained_bytes"):
        _require(isinstance(steady.get(field), int) and steady[field] > 0,
                 f"steady-state {field} must be positive")
    _require(steady["draws"] == steady["batches"],
             "each adjacent-compatible painter batch must use one instanced draw")
    completed_maps = len(frames) + len(frames) // 10
    _require(steady["batches"] >= completed_maps and
             steady["slot_uniform_uploads"] == steady["batches"] - completed_maps,
             "slot-uniform uploads do not match world batch submissions")
    _require(steady["slot_uniform_upload_bytes"] >= steady["slot_uniform_uploads"] * 16 and
             steady["slot_uniform_upload_bytes"] <= steady["slot_uniform_uploads"] * 1024,
             "slot-uniform upload bytes exceed the bounded 256-slot chunks")
    _require(steady["batches"] * 10 <= steady["commands"] * 9,
             "painter submission did not reduce command draws by at least ten percent")
    _require(steady["peak_retained_bytes"] >= steady["retained_bytes"],
             "steady-state retained peak is below retained bytes")
    return name


def validate(paths: list[Path], require_complete: bool) -> None:
    counts: Counter[str] = Counter()
    revisions: set[str] = set()
    cohorts: set[str] = set()
    backends: set[str] = set()
    identities: set[tuple[str, str, str, str, str]] = set()
    for path in paths:
        for record in _records(path):
            counts[validate_record(record)] += 1
            revisions.add(record["revision"])
            cohorts.add(record["shader_cohort"])
            backends.add(record["gpu"]["backend"])
            gpu = record["gpu"]
            identities.add((gpu["backend"], gpu["device"], gpu["driver_name"],
                            gpu["driver_version"], gpu["hardware_tier"]))
    _require(len(revisions) == 1, "artifacts contain different revisions")
    _require(len(cohorts) == 1, "artifacts contain different shader cohorts")
    _require(len(identities) == 1, "artifacts mix GPU device, driver, or hardware tier")
    if require_complete:
        _require(set(counts) == WORKLOADS, "complete matrix is missing or has unknown rows")
        _require(all(counts[name] >= 3 for name in WORKLOADS),
                 "every workload requires at least three fresh-process runs")
    print(json.dumps({"records": sum(counts.values()),
                      "workloads": dict(sorted(counts.items())),
                      "backends": sorted(backends)}, sort_keys=True))


def validate_lifecycle_record(record: dict) -> None:
    _require(record.get("schema_version") == 3, "unsupported lifecycle schema_version")
    _require(record.get("benchmark") == "gpu-production-recovery-lifecycle",
             "wrong lifecycle benchmark")
    _require(record.get("dirty") is False, "qualified revision must be clean")
    _require(bool(REVISION.fullmatch(str(record.get("revision", "")))), "invalid revision")
    _require(bool(HEX_64.fullmatch(str(record.get("shader_cohort", "")))),
             "invalid shader cohort")
    gpu = record.get("gpu", {})
    _require(gpu.get("backend") in {"vulkan", "direct3d12", "metal"}, "unsupported backend")
    _require(gpu.get("qualified_hardware") is True, "hardware attestation is absent")
    for field in ("device", "driver_name", "driver_version"):
        _require(isinstance(gpu.get(field), str) and gpu[field] and gpu[field] != "unavailable",
                 f"missing gpu.{field}")
    _require(gpu.get("hardware_tier") in {"reference", "minimum"},
             "missing lifecycle hardware tier")
    build = record.get("build", {})
    _require(build.get("type") == "Release", "lifecycle build is not Release")
    for field in ("compiler", "system"):
        _require(isinstance(build.get(field), str) and build[field], f"missing build.{field}")
    _require(record.get("fixture") == "brynknot-movement", "wrong lifecycle fixture")
    _require(bool(HEX_64.fullmatch(str(record.get("manifest_sha256", "")))),
             "invalid lifecycle manifest identity")
    viewport = record.get("viewport")
    resize_delta = record.get("resize_delta")
    _require(isinstance(viewport, list) and len(viewport) == 2 and
             all(isinstance(value, int) and value > 0 for value in viewport),
             "invalid lifecycle viewport")
    _require(isinstance(resize_delta, list) and len(resize_delta) == 2 and
             all(isinstance(value, int) and value > 0 for value in resize_delta),
             "invalid lifecycle resize delta")
    _require(record.get("production_path") is True, "lifecycle did not use production composition")
    _require(record.get("sustained_frames_per_event") == 40,
             "lifecycle sustained window is incomplete")
    events = record.get("events")
    _require(isinstance(events, list) and [event.get("name") for event in events] ==
             LIFECYCLE_EVENTS, "lifecycle events are incomplete")
    no_recovery = {"cold_asset_upload", "teleport", "reconnect", "screenshot_readback"}
    for event in events:
        expected_attempts = 0 if event["name"] in no_recovery else 1
        _require(event.get("recovery_attempts") == expected_attempts,
                 f"{event['name']} recovery attempt count is not bounded")
        _require(event.get("fullscreen") is (event["name"] == "fullscreen_enter"),
                 f"{event['name']} fullscreen state is wrong")
        action = event.get("action", {})
        screenshot = event["name"] == "screenshot_readback"
        expected_action_fields = {
            "asynchronous", "duration_ns", "uploads", "upload_bytes",
            "source_uploads", "source_upload_bytes",
            "instance_uploads", "instance_upload_bytes",
            "light_uploads", "light_upload_bytes",
            "slot_uniform_uploads", "slot_uniform_upload_bytes",
            "resource_creations", "resource_destructions", "device_recoveries",
            "recovery_failures", "readbacks", "fallbacks", "map_pacing",
        }
        _require(set(action) == expected_action_fields,
                 f"{event['name']} action evidence schema is incomplete")
        _require(action.get("asynchronous") is screenshot,
                 f"{event['name']} asynchronous action state is wrong")
        _require(isinstance(action.get("duration_ns"), int) and action["duration_ns"] > 0,
                 f"{event['name']} action duration is absent")
        _require(action.get("readbacks") == (1 if screenshot else 0),
                 f"{event['name']} action readback count is wrong")
        _require(action.get("fallbacks") == 0 and action.get("recovery_failures") == 0,
                 f"{event['name']} action used a fallback or failed recovery")
        _validate_map_pacing(action.get("map_pacing"), f"{event['name']} action")
        recovery = event["name"] not in no_recovery
        _require(action.get("device_recoveries") == (1 if recovery else 0),
                 f"{event['name']} action recovery statistics are wrong")
        for field in ("uploads", "upload_bytes", "source_uploads", "source_upload_bytes",
                      "instance_uploads", "instance_upload_bytes", "light_uploads",
                      "light_upload_bytes", "slot_uniform_uploads", "slot_uniform_upload_bytes",
                      "resource_creations", "resource_destructions"):
            _require(isinstance(action.get(field), int) and action[field] >= 0,
                     f"{event['name']} action {field} is invalid")
        _require(action["uploads"] == action["source_uploads"] +
                 action["instance_uploads"] + action["light_uploads"] +
                 action["slot_uniform_uploads"] and
                 action["upload_bytes"] == action["source_upload_bytes"] +
                 action["instance_upload_bytes"] + action["light_upload_bytes"] +
                 action["slot_uniform_upload_bytes"],
                 f"{event['name']} action upload accounting is inconsistent")
        _require(action["slot_uniform_upload_bytes"] >= action["slot_uniform_uploads"] * 16 and
                 action["slot_uniform_upload_bytes"] <= action["slot_uniform_uploads"] * 1024,
                 f"{event['name']} action slot-uniform uploads are unbounded")
        if event["name"] == "cold_asset_upload":
            _require(action["uploads"] > 0 and action["upload_bytes"] > 0 and
                     action["resource_creations"] > 0,
                     "cold action did not capture first-frame retained construction")
        output_size = event.get("output_size")
        _require(isinstance(output_size, list) and len(output_size) == 2 and
                 all(isinstance(value, int) and value > 0 for value in output_size),
                 f"{event['name']} output size is invalid")
        display_mode_size = event.get("display_mode_size")
        expected_display_mode = output_size if event["name"] == "fullscreen_enter" else [0, 0]
        _require(display_mode_size == expected_display_mode,
                 f"{event['name']} active display-mode size is wrong")
        _require(bool(HEX_64.fullmatch(str(event.get("pixels_sha256", "")))),
                 f"{event['name']} checkpoint is invalid")
        _require(isinstance(event.get("artifact"), str) and event["artifact"].startswith("review/"),
                 f"{event['name']} review artifact is absent")
        _require(bool(HEX_64.fullmatch(str(event.get("artifact_sha256", "")))),
                 f"{event['name']} review artifact digest is absent")
        frames = event.get("frame_windows_ns")
        _require(isinstance(frames, list) and len(frames) == 40 and
                 all(isinstance(value, int) and value > 0 for value in frames),
                 f"{event['name']} sustained frame window is invalid")
        frame_ns = event.get("frame_ns", {})
        _require(frame_ns.get("p95") == _percentile(frames, 95) and
                 frame_ns.get("p99") == _percentile(frames, 99) and
                 frame_ns.get("max") == max(frames),
                 f"{event['name']} lifecycle percentiles are inconsistent")
        _require(frame_ns["p95"] < 20_000_000 and frame_ns["p99"] < 25_000_000,
                 f"{event['name']} did not return to the sustained row budget")
        steady = event.get("steady_state", {})
        _require(steady.get("uploads") == steady.get("slot_uniform_uploads") and
                 steady.get("upload_bytes") == steady.get("slot_uniform_upload_bytes"),
                 f"{event['name']} steady uploads are not slot-uniform-only")
        _require(steady["slot_uniform_upload_bytes"] >= steady["slot_uniform_uploads"] * 16 and
                 steady["slot_uniform_upload_bytes"] <= steady["slot_uniform_uploads"] * 1024 and
                 steady["slot_uniform_uploads"] <= steady.get("batches", -1),
                 f"{event['name']} steady slot-uniform uploads are unbounded")
        for field in ("resource_creations", "resource_destructions", "readbacks", "fallbacks"):
            _require(steady.get(field) == 0, f"{event['name']} steady {field} is not zero")
        for field in ("commands", "batches", "draws", "retained_bytes"):
            _require(isinstance(steady.get(field), int) and steady[field] > 0,
                     f"{event['name']} steady {field} is absent")
        _validate_map_pacing(steady.get("map_pacing"), f"{event['name']} steady-state")
    by_name = {event["name"]: event for event in events}
    baseline = by_name["cold_asset_upload"]["pixels_sha256"]
    _require(by_name["resize_grow"]["output_size"] ==
             [viewport[0] + resize_delta[0], viewport[1] + resize_delta[1]],
             "resize-grow output size is wrong")
    _require(by_name["resize_grow"]["pixels_sha256"] != baseline,
             "resize-grow checkpoint did not change")
    _require(by_name["teleport"]["pixels_sha256"] != baseline,
             "teleport checkpoint did not change")
    for name in set(LIFECYCLE_EVENTS) - {"resize_grow", "teleport", "fullscreen_enter"}:
        _require(by_name[name]["output_size"] == viewport,
                 f"{name} did not restore the baseline output size")
        _require(by_name[name]["pixels_sha256"] == baseline,
                 f"{name} did not restore the baseline scene")
    checkpoint = record.get("final_checkpoint", {})
    _require(checkpoint.get("algorithm") == "sha256-rgba32-with-dimensions",
             "wrong lifecycle checkpoint algorithm")
    _require(bool(HEX_64.fullmatch(str(checkpoint.get("pixels_sha256", "")))),
             "invalid lifecycle checkpoint")
    _require(checkpoint["pixels_sha256"] == baseline,
             "final lifecycle checkpoint did not restore the baseline")


def _lifecycle_fullscreen_golden(contract: dict, record: dict, fullscreen: dict):
    display_mode = fullscreen["display_mode_size"]
    mode_key = f"{display_mode[0]}x{display_mode[1]}"
    return contract.get("backends", {}).get(record["gpu"]["backend"], {}).get(
        "brynknot-movement", {}).get("fullscreen", {}).get(
            record["gpu"]["hardware_tier"], {}).get(mode_key)


def validate_lifecycle(paths: list[Path], closure: bool = True) -> None:
    sourced_records = [(path, record) for path in paths for record in _records(path)]
    records = [record for _, record in sourced_records]
    for path, record in sourced_records:
        validate_lifecycle_record(record)
        for event in record["events"]:
            _review_artifact(path,
                             event["artifact"],
                             event["artifact_sha256"],
                             event["output_size"],
                             event["pixels_sha256"])
    _require(len(records) >= 1, "lifecycle artifact is empty")
    fixture = FIXTURE_ROOT / "brynknot-movement.xml"
    expected_manifest = hashlib.sha256(fixture.read_bytes()).hexdigest()
    for record in records:
        _require(record["manifest_sha256"] == expected_manifest,
                 "lifecycle manifest identity mismatch")
    if not closure:
        print(json.dumps({"lifecycle_records": len(records), "closure": False}, sort_keys=True))
        return
    contract = json.loads(GOLDEN_CONTRACT.read_text(encoding="utf-8"))
    _require(contract.get("human_approved") is True,
             "GPU golden contract lacks human approval")
    for record in records:
        expected = contract.get("backends", {}).get(record["gpu"]["backend"], {}).get(
            "brynknot-movement", {}).get("initial")
        _require(expected == record["events"][0]["pixels_sha256"],
                 "lifecycle baseline does not match the approved backend golden")
        fullscreen = next(event for event in record["events"]
                          if event["name"] == "fullscreen_enter")
        expected_fullscreen = _lifecycle_fullscreen_golden(contract, record, fullscreen)
        _require(expected_fullscreen == fullscreen["pixels_sha256"],
                 "lifecycle fullscreen output does not match the approved "
                 "backend, hardware-tier, and display-mode golden")
    print(json.dumps({"lifecycle_records": len(records)}, sort_keys=True))


def validate_production_record(record: dict) -> bool:
    _require(record.get("schema_version") == 2, "unsupported production schema_version")
    _require(record.get("renderer") == "gpu-production-player-view", "wrong production renderer")
    _require(record.get("fixture") in PRODUCTION_FIXTURES, "unknown production fixture")
    _require(record.get("dirty") is False, "qualified revision must be clean")
    _require(bool(REVISION.fullmatch(str(record.get("revision", "")))), "invalid revision")
    for field in ("manifest_sha256", "snapshot_sha256", "pixels_sha256",
                  "initial_pixels_sha256"):
        _require(bool(HEX_64.fullmatch(str(record.get(field, "")))), f"invalid {field}")
    _require(record.get("backend") in {"vulkan", "direct3d12", "metal"},
             "unsupported backend")
    _require(record.get("qualified_hardware") is True, "hardware attestation is absent")
    for field in ("device", "driver_name", "driver_version"):
        _require(isinstance(record.get(field), str) and record[field] and
                 record[field] != "unavailable", f"missing {field}")
    for prefix in ("initial", "final"):
        _require(isinstance(record.get(f"{prefix}_artifact"), str) and
                 record[f"{prefix}_artifact"].startswith("review/"),
                 f"missing {prefix} review artifact")
        _require(bool(HEX_64.fullmatch(str(record.get(f"{prefix}_artifact_sha256", "")))),
                 f"missing {prefix} review artifact digest")
    _require(record.get("hardware_tier") in {"reference", "minimum"},
             "missing production hardware tier")
    build = record.get("build", {})
    _require(build.get("type") == "Release", "production fixture build is not Release")
    for field in ("compiler", "system"):
        _require(isinstance(build.get(field), str) and build[field], f"missing build.{field}")
    assertions = record.get("assertions")
    _require(isinstance(assertions, dict) and set(assertions) == set(ASSERTION_ATTRIBUTES),
             "production semantic assertions are incomplete")
    _require(all(isinstance(value, bool) for value in assertions.values()),
             "production semantic assertion status is invalid")
    if assertions["ui_names_targets"]:
        _require(bool(HEX_64.fullmatch(str(record.get("ui_pixels_sha256", "")))),
                 "UI checkpoint digest is absent")
    else:
        _require(record.get("ui_pixels_sha256") == "", "unexpected UI checkpoint digest")
    timed = record.get("timed_light_lifecycle") is True
    _require(isinstance(record.get("timed_light_lifecycle"), bool),
             "timed-light lifecycle status is absent")
    _require(isinstance(record.get("borrowed_temporal_light_samples"), int) and
             record["borrowed_temporal_light_samples"] >= 0,
             "timed-light borrowing count is invalid")
    if timed:
        _require(record.get("fixture") == "timed-light", "unexpected timed-light lifecycle")
        _require(bool(HEX_64.fullmatch(str(record.get("timed_endpoint_pixels_sha256", "")))),
                 "timed-light endpoint checkpoint is absent")
        _require(record["timed_endpoint_pixels_sha256"] != record["initial_pixels_sha256"],
                 "timed-light midpoint and endpoint are indistinguishable")
        _require(record["pixels_sha256"] == record["initial_pixels_sha256"],
                 "timed-light lifecycle did not restore the initial midpoint")
        _require(record["borrowed_temporal_light_samples"] > 0,
                 "timed FOW borrowing was not exercised")
    else:
        _require(record.get("timed_endpoint_pixels_sha256") == "",
                 "unexpected timed-light endpoint checkpoint")
        _require(record["borrowed_temporal_light_samples"] == 0,
                 "unexpected timed-light borrowing count")
    for field in ("archived_software_pixels_sha256", "archived_software_ui_pixels_sha256"):
        value = record.get(field)
        _require(value == "" or bool(HEX_64.fullmatch(str(value))),
                 f"{field} is not an archived exact identity")
    movement = record.get("movement_lifecycle") is True
    _require(isinstance(record.get("golden_verified"), bool), "golden status is absent")
    if movement:
        _require(record["pixels_sha256"] == record["initial_pixels_sha256"],
                 "A-to-B-to-A lifecycle did not restore the initial frame")
        _require(bool(HEX_64.fullmatch(str(record.get("transition_pixels_sha256", "")))),
                 "movement transition checkpoint is absent")
        _require(record["transition_pixels_sha256"] != record["initial_pixels_sha256"],
                 "movement transition is not observably distinct")
        _require(bool(HEX_64.fullmatch(str(record.get("archived_software_lifecycle_sha256", "")))),
                 "archived software lifecycle identity is absent")
    else:
        _require(record.get("archived_software_lifecycle_sha256") == "",
                 "unexpected archived software lifecycle identity")
    ui_closure = record.get("ui_closure")
    if record["fixture"] == "gpu-ui-closure":
        _require(isinstance(ui_closure, list) and
                 [state.get("name") for state in ui_closure] == list(UI_CLOSURE_STATES),
                 "complete-screen GPU state sweep is incomplete")
        by_name = {state["name"]: state for state in ui_closure}
        for name, (expected_count, expected_hash) in ROOT_GLYPH_CONTRACTS.items():
            root_glyphs = by_name[name].get("root_glyphs", {})
            _require(root_glyphs == {
                "count": expected_count,
                "semantic_hash": expected_hash,
            }, f"{name} root glyph submission contract changed")
        for state in ui_closure:
            _require(bool(HEX_64.fullmatch(str(state.get("pixels_sha256", "")))),
                     f"{state['name']} UI checkpoint is invalid")
            size = state.get("output_size")
            expected_size = ([dimension * 3 // 4 for dimension in record["viewport"]]
                             if state["name"] == "screenshot_map" else record["viewport"])
            _require(isinstance(size, list) and size == expected_size,
                     f"{state['name']} UI checkpoint size is invalid")
            screenshot = state["name"].startswith("screenshot_")
            expected_command = "/screenshot map" if state["name"] == "screenshot_map" else (
                "/screenshot" if screenshot else "")
            _require(state.get("command") == expected_command and
                     state.get("asynchronous") is screenshot,
                     f"{state['name']} player-facing screenshot evidence is wrong")
            _require(isinstance(state.get("artifact"), str) and
                     state["artifact"].startswith("review/") and
                     bool(HEX_64.fullmatch(str(state.get("artifact_sha256", "")))),
                     f"{state['name']} review artifact evidence is absent")
            steady = state.get("steady_state", {})
            _require(steady.get("uploads") == steady.get("slot_uniform_uploads") and
                     steady.get("upload_bytes") == steady.get("slot_uniform_upload_bytes"),
                     f"{state['name']} retained UI uploads are not slot-uniform-only")
            _require(steady["slot_uniform_upload_bytes"] >=
                     steady["slot_uniform_uploads"] * 16 and
                     steady["slot_uniform_upload_bytes"] <=
                     steady["slot_uniform_uploads"] * 1024,
                     f"{state['name']} retained UI slot-uniform uploads are unbounded")
            for field in ("resource_creations", "resource_destructions", "fallbacks"):
                _require(steady.get(field) == 0,
                         f"{state['name']} retained UI {field} is not zero")
            _require(steady.get("readbacks") == (1 if screenshot else 0),
                     f"{state['name']} readback count is wrong")
        _require(len({by_name["intro_server_browser"]["pixels_sha256"],
                      by_name["login_popup"]["pixels_sha256"],
                      by_name["gameplay_widgets_text_windows"]["pixels_sha256"]}) == 3,
                 "intro, login, and gameplay UI checkpoints are indistinguishable")
        _require(by_name["popup_character_selection"]["pixels_sha256"] !=
                 by_name["gameplay_widgets_text_windows"]["pixels_sha256"],
                 "character-selection popup was not visibly rendered")
        _require(by_name["screenshot_map"]["pixels_sha256"] !=
                 by_name["screenshot_window"]["pixels_sha256"],
                 "map screenshot crop is not visibly distinct from the window")
        _require(by_name["region_map_fow_transition"]["pixels_sha256"] ==
                 by_name["region_map_fow_retained"]["pixels_sha256"] and
                 by_name["popup_region_map_minimap"]["pixels_sha256"] !=
                 by_name["region_map_fow_transition"]["pixels_sha256"],
                 "region-map FOW transition is not stable")
    else:
        _require(ui_closure is None, "unexpected complete-screen UI sweep")
    return movement


def _production_cohort() -> dict[str, tuple[str, str, dict[str, bool], str, str, str]]:
    cohort = {}
    for fixture in PRODUCTION_FIXTURES:
        path = FIXTURE_ROOT / f"{fixture}.xml"
        manifest = path.read_bytes()
        root = ET.fromstring(manifest)
        snapshot_digest = root.attrib["snapshot-sha256"]
        for name in ("snapshot", "interface", "layout"):
            digest = root.attrib[f"{name}-sha256"]
            input_path = (path.parent / root.attrib["input-root"] / root.attrib[name]).resolve()
            _require(hashlib.sha256(input_path.read_bytes()).hexdigest() == digest,
                     f"{fixture} {name} pin does not match its input")
        assertions = {
            field: all(root.attrib.get(attribute) == "true" for attribute in attributes)
            for field, attributes in ASSERTION_ATTRIBUTES.items()
        }
        archived_lifecycle = root.attrib.get("expected-standard-checkpoint-sha256", "")
        archived_pixels = root.attrib.get("archived-software-pixels-sha256", "")
        archived_ui = root.attrib.get("archived-software-ui-pixels-sha256", "")
        cohort[fixture] = (hashlib.sha256(manifest).hexdigest(), snapshot_digest, assertions,
                           archived_lifecycle, archived_pixels, archived_ui)
    return cohort


def _validate_approved_goldens(records: list[dict]) -> None:
    contract = json.loads(GOLDEN_CONTRACT.read_text(encoding="utf-8"))
    _require(contract.get("schema_version") == 1, "unsupported GPU golden contract")
    _require(contract.get("contract") == "exact-rgba8", "unapproved pixel tolerance contract")
    _require(contract.get("human_approved") is True, "GPU golden contract lacks human approval")
    approved = contract.get("backends", {})
    for record in records:
        backend = record["backend"]
        expected = approved.get(backend, {}).get(record["fixture"], {})
        _require(expected.get("initial") == record["initial_pixels_sha256"],
                 f"{record['fixture']} {backend} initial golden mismatch")
        _require(expected.get("final") == record["pixels_sha256"],
                 f"{record['fixture']} {backend} final golden mismatch")
        if record["movement_lifecycle"]:
            _require(expected.get("transition") == record["transition_pixels_sha256"],
                     f"{record['fixture']} {backend} transition golden mismatch")
        else:
            _require(expected.get("transition") is None,
                     f"{record['fixture']} {backend} has an unexpected transition golden")
        if record["timed_light_lifecycle"]:
            _require(expected.get("timed_endpoint") == record["timed_endpoint_pixels_sha256"],
                     f"{record['fixture']} {backend} timed endpoint golden mismatch")
        else:
            _require(expected.get("timed_endpoint") is None,
                     f"{record['fixture']} {backend} has an unexpected timed endpoint golden")
        if record["fixture"] == "gpu-ui-closure":
            expected_states = expected.get("ui_closure", {})
            _require(set(expected_states) == set(UI_CLOSURE_STATES),
                     f"{backend} complete-screen golden state set is incomplete")
            for state in record["ui_closure"]:
                _require(expected_states.get(state["name"]) == state["pixels_sha256"],
                         f"{backend} {state['name']} UI golden mismatch")
        else:
            _require(expected.get("ui_closure") is None,
                     f"{record['fixture']} {backend} has unexpected UI closure goldens")


def validate_production(paths: list[Path], closure: bool) -> None:
    sourced_records = [(path, record) for path in paths for record in _records(path)]
    records = [record for _, record in sourced_records]
    movements = [validate_production_record(record) for record in records]
    for path, record in sourced_records:
        for prefix in ("initial", "final"):
            semantic_field = "initial_pixels_sha256" if prefix == "initial" else \
                "pixels_sha256"
            _review_artifact(path,
                             record[f"{prefix}_artifact"],
                             record[f"{prefix}_artifact_sha256"],
                             record["viewport"],
                             record[semantic_field])
        if record["fixture"] == "gpu-ui-closure":
            ui_artifacts = {}
            for state in record["ui_closure"]:
                pixels = _review_artifact(path,
                                          state["artifact"],
                                          state["artifact_sha256"],
                                          state["output_size"],
                                          state["pixels_sha256"])
                ui_artifacts[state["name"]] = pixels
            _require_ui_screenshot_crop(record, ui_artifacts)
    _require(len(records) == len(PRODUCTION_FIXTURES),
             "production fixture matrix has the wrong record count")
    _require(sum(movements) == 1,
             "production fixture matrix requires one A-to-B-to-A lifecycle")
    cohort = _production_cohort()
    _require({record["fixture"] for record in records} == set(PRODUCTION_FIXTURES),
             "production fixture cohort is incomplete or duplicated")
    for record in records:
        (manifest_digest, snapshot_digest, assertions, archived_lifecycle,
         archived_pixels, archived_ui) = cohort[record["fixture"]]
        _require((record["manifest_sha256"], record["snapshot_sha256"]) ==
                 (manifest_digest, snapshot_digest), f"{record['fixture']} identity mismatch")
        _require(record["assertions"] == assertions,
                 f"{record['fixture']} semantic assertion coverage mismatch")
        _require(record["archived_software_lifecycle_sha256"] == archived_lifecycle,
                 f"{record['fixture']} archived lifecycle identity mismatch")
        _require(record["archived_software_pixels_sha256"] == archived_pixels,
                 f"{record['fixture']} archived software pixel identity mismatch")
        _require(record["archived_software_ui_pixels_sha256"] == archived_ui,
                 f"{record['fixture']} archived software UI identity mismatch")
    _require(len({record["revision"] for record in records}) == 1,
             "production fixtures contain different revisions")
    _require(len({record["backend"] for record in records}) == 1,
             "production fixtures contain different backends")
    _require(len({(record["backend"], record["device"], record["driver_name"],
                   record["driver_version"], record["hardware_tier"])
                  for record in records}) == 1,
             "production fixtures mix device, driver, or hardware tier")
    if closure:
        _validate_approved_goldens(records)
    print(json.dumps({"production_records": len(records), "closure": closure}, sort_keys=True))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifacts", nargs="+", type=Path)
    parser.add_argument("--require-complete", action="store_true")
    parser.add_argument("--lifecycle", action="store_true")
    parser.add_argument("--collect-lifecycle", action="store_true")
    parser.add_argument("--production-fixtures", action="store_true")
    parser.add_argument("--collect-production-fixtures", action="store_true")
    arguments = parser.parse_args()
    try:
        if arguments.lifecycle or arguments.collect_lifecycle:
            _require(not (arguments.lifecycle and arguments.collect_lifecycle),
                     "choose lifecycle collection or closure validation, not both")
            validate_lifecycle(arguments.artifacts, arguments.lifecycle)
        elif arguments.production_fixtures or arguments.collect_production_fixtures:
            _require(not (arguments.production_fixtures and arguments.collect_production_fixtures),
                     "choose collection or closure validation, not both")
            validate_production(arguments.artifacts, arguments.production_fixtures)
        else:
            validate(arguments.artifacts, arguments.require_complete)
    except (ArtifactError, OSError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
