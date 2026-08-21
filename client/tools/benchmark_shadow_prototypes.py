#!/usr/bin/env python3
"""Measure bounded, offline shadow prototypes against frozen player-view scenes.

This is deliberately a research model.  It rasterizes masks and counts bounded
work; it is not linked into the client renderer and must not become a shadow
implementation without a separate production contract.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import statistics
import sys
from typing import Any


SCHEMA_VERSION = 1
MAX_PIXELS = 128 * 96
MAX_CASTERS = 64
TECHNIQUES = ("contact_blob", "field_screen_space", "directional_silhouette")
IDLE_FRAMES = 16
MOVEMENT_FRAMES = 480


class PrototypeError(ValueError):
    """The closed research fixture or result contract is invalid."""


def _no_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise PrototypeError(f"repeated JSON field: {key}")
        result[key] = value
    return result


def _mapping(value: Any, fields: set[str], context: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != fields:
        raise PrototypeError(f"{context} has an incompatible schema")
    return value


def _integer(value: Any, context: str, *, positive: bool = False) -> int:
    if type(value) is not int or value < (1 if positive else 0):
        raise PrototypeError(f"{context} is invalid")
    return value


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class Caster:
    name: str
    x: int
    y: int
    depth: int
    elevation: int
    opacity: int
    fade: int
    radius: int
    animated: bool
    visible: bool
    transformed: bool
    effect: bool


@dataclass(frozen=True)
class Scene:
    name: str
    width: int
    height: int
    radiance: tuple[int, ...]
    blocked: frozenset[tuple[int, int]]
    casters: tuple[Caster, ...]
    directions: tuple[tuple[str, int, int], ...]


def _parse_fixture(path: Path) -> tuple[dict[str, Any], tuple[Scene, ...]]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_no_duplicate_keys)
    except (OSError, json.JSONDecodeError) as error:
        raise PrototypeError(f"cannot read fixture: {path}") from error
    root = _mapping(raw, {"schema_version", "source_fixture", "viewport", "workload", "light_cases", "scenes"}, "fixture")
    if root["schema_version"] != SCHEMA_VERSION:
        raise PrototypeError("fixture schema version is unsupported")
    if not isinstance(root["source_fixture"], str) or not root["source_fixture"]:
        raise PrototypeError("fixture source is invalid")
    viewport = _mapping(root["viewport"], {"width", "height"}, "fixture viewport")
    width = _integer(viewport["width"], "fixture width", positive=True)
    height = _integer(viewport["height"], "fixture height", positive=True)
    if width * height > MAX_PIXELS:
        raise PrototypeError("fixture viewport exceeds the bounded pixel limit")
    workload = _mapping(root["workload"], {"movement_frames", "idle_frames", "path"}, "fixture workload")
    if _integer(workload["movement_frames"], "movement frames") != MOVEMENT_FRAMES:
        raise PrototypeError("movement fixture must use the dense 480-frame workload")
    if _integer(workload["idle_frames"], "idle frames") != IDLE_FRAMES:
        raise PrototypeError("idle fixture must contain exactly 16 settled frames")
    path_points = workload["path"]
    if not isinstance(path_points, list) or len(path_points) != 8:
        raise PrototypeError("movement path must contain eight deterministic points")
    for point in path_points:
        _mapping(point, {"x", "y"}, "movement path point")
        if type(point["x"]) is not int or type(point["y"]) is not int:
            raise PrototypeError("movement path coordinates are invalid")
    light_cases = root["light_cases"]
    required_light_cases = {
        "dawn", "noon", "dusk", "night", "full-moon", "new-moon", "starlight-only",
        "map-local-colored", "negative-light", "timed-keyframe",
    }
    if (
        not isinstance(light_cases, list)
        or len(light_cases) != len(required_light_cases)
        or {case.get("state") for case in light_cases if isinstance(case, dict)} != required_light_cases
    ):
        raise PrototypeError("fixture light cases do not cover the required matrix")
    for case in light_cases:
        item = _mapping(case, {"state", "scalar", "rgb", "negative", "timed"}, "light case")
        _integer(item["scalar"], "light scalar")
        if item["scalar"] > 255 or not isinstance(item["rgb"], list) or len(item["rgb"]) != 3:
            raise PrototypeError("light case radiance is invalid")
        if any(type(channel) is not int or not 0 <= channel <= 255 for channel in item["rgb"]):
            raise PrototypeError("light case color is invalid")
        if type(item["negative"]) is not bool or type(item["timed"]) is not bool:
            raise PrototypeError("light case flags are invalid")

    scenes: list[Scene] = []
    raw_scenes = root["scenes"]
    if not isinstance(raw_scenes, list) or len(raw_scenes) != 2:
        raise PrototypeError("fixture must contain the identical two-scene matrix")
    scene_names: set[str] = set()
    for raw_scene in raw_scenes:
        scene = _mapping(raw_scene, {"name", "field_seed", "roof_rows", "blocked", "casters", "directions"}, "scene")
        name = scene["name"]
        if not isinstance(name, str) or not name:
            raise PrototypeError("scene name is invalid")
        if name in scene_names:
            raise PrototypeError(f"duplicate scene name: {name}")
        scene_names.add(name)
        seed = _integer(scene["field_seed"], f"{name} field seed")
        roof_rows = scene["roof_rows"]
        if not isinstance(roof_rows, list) or any(type(row) is not int or not 0 <= row < height for row in roof_rows):
            raise PrototypeError(f"{name} roof rows are invalid")
        blocked_values = scene["blocked"]
        blocked: set[tuple[int, int]] = set()
        if not isinstance(blocked_values, list):
            raise PrototypeError(f"{name} blocked cells are invalid")
        for cell in blocked_values:
            item = _mapping(cell, {"x", "y"}, f"{name} blocked cell")
            x, y = _integer(item["x"], "blocked x"), _integer(item["y"], "blocked y")
            if x >= width or y >= height:
                raise PrototypeError(f"{name} blocked cell is outside the viewport")
            blocked.add((x, y))
        raw_casters = scene["casters"]
        if not isinstance(raw_casters, list) or not 0 < len(raw_casters) <= MAX_CASTERS:
            raise PrototypeError(f"{name} caster count is invalid")
        casters: list[Caster] = []
        caster_names: set[str] = set()
        for raw_caster in raw_casters:
            caster = _mapping(
                raw_caster,
                {"name", "x", "y", "depth", "elevation", "opacity", "fade", "radius", "animated", "visible", "transformed", "effect"},
                f"{name} caster",
            )
            caster_name = caster["name"]
            if not isinstance(caster_name, str) or not caster_name or caster_name in caster_names:
                raise PrototypeError(f"{name} caster name is invalid or repeated")
            caster_names.add(caster_name)
            values = {key: caster[key] for key in ("x", "y", "depth", "elevation", "opacity", "fade", "radius")}
            for key, value in values.items():
                _integer(value, f"{name} caster {key}")
            if values["x"] >= width or values["y"] >= height or values["opacity"] > 255 or values["fade"] > 255 or values["radius"] > 8:
                raise PrototypeError(f"{name} caster bounds are invalid")
            if not all(type(caster[key]) is bool for key in ("animated", "visible", "transformed", "effect")):
                raise PrototypeError(f"{name} caster flags are invalid")
            casters.append(Caster(caster["name"], **values, animated=caster["animated"], visible=caster["visible"], transformed=caster["transformed"], effect=caster["effect"]))
        directions: list[tuple[str, int, int]] = []
        raw_directions = scene["directions"]
        if not isinstance(raw_directions, list) or len(raw_directions) != 5:
            raise PrototypeError(f"{name} must cover five timed/celestial states")
        direction_names: set[str] = set()
        for raw_direction in raw_directions:
            direction = _mapping(raw_direction, {"state", "dx", "dy"}, f"{name} light state")
            if not isinstance(direction["state"], str) or type(direction["dx"]) is not int or type(direction["dy"]) is not int:
                raise PrototypeError(f"{name} light state is invalid")
            if not direction["state"] or direction["state"] in direction_names:
                raise PrototypeError(f"{name} light state is invalid or repeated")
            direction_names.add(direction["state"])
            directions.append((direction["state"], direction["dx"], direction["dy"]))
        radiance = tuple((seed + x * 17 + y * 29 + (11 if y in roof_rows else 0)) % 256 for y in range(height) for x in range(width))
        scenes.append(Scene(name, width, height, radiance, frozenset(blocked), tuple(casters), tuple(directions)))
    return root, tuple(scenes)


def _idx(scene: Scene, x: int, y: int) -> int:
    return y * scene.width + x


def _clamp(value: int, low: int, high: int) -> int:
    return max(low, min(high, value))


def _bounds(caster: Caster, x: int, y: int, scene: Scene) -> tuple[int, int, int, int]:
    radius = caster.radius + (caster.elevation // 4)
    return (
        _clamp(x - radius, 0, scene.width - 1),
        _clamp(y - radius, 0, scene.height - 1),
        _clamp(x + radius, 0, scene.width - 1),
        _clamp(y + radius, 0, scene.height - 1),
    )


def _dirty_cells(scene: Scene, positions: dict[str, tuple[int, int]], previous: dict[str, tuple[int, int]] | None) -> set[tuple[int, int]]:
    if previous is None:
        return {(x, y) for y in range(scene.height) for x in range(scene.width)}
    if positions == previous:
        return set()
    dirty: set[tuple[int, int]] = set()
    for caster in scene.casters:
        if not caster.visible:
            continue
        for point in (positions[caster.name], previous[caster.name]):
            left, top, right, bottom = _bounds(caster, point[0], point[1], scene)
            dirty.update((x, y) for y in range(top, bottom + 1) for x in range(left, right + 1))
    return dirty


def _eligible(caster: Caster, x: int, y: int, scene: Scene) -> bool:
    return caster.visible and caster.opacity > 0 and caster.fade > 0 and (x, y) not in scene.blocked


def _ordered_casters(scene: Scene) -> tuple[Caster, ...]:
    return tuple(sorted(scene.casters, key=lambda caster: (caster.depth, caster.name)))


def _contact(scene: Scene, positions: dict[str, tuple[int, int]], dirty: set[tuple[int, int]], state_scalar: int) -> tuple[bytes, dict[str, int]]:
    mask = bytearray(scene.width * scene.height)
    pixels = spans = 0
    for x, y in dirty:
        pixels += 1
        for caster in _ordered_casters(scene):
            cx, cy = positions[caster.name]
            radius = caster.radius + caster.elevation // 4
            dx, dy = x - cx, y - cy
            if not _eligible(caster, x, y, scene) or dx * dx + dy * dy > radius * radius:
                continue
            if scene.radiance[_idx(scene, x, y)] == 0 or state_scalar == 0:
                continue
            mask[_idx(scene, x, y)] = max(mask[_idx(scene, x, y)], (caster.opacity * caster.fade * state_scalar) // (255 * 255 * 4))
        if mask[_idx(scene, x, y)]:
            spans += 1
    return bytes(mask), {"processed_pixels": pixels, "processed_spans": spans, "allocations": 1, "memory_bytes": len(mask)}


def _field(scene: Scene, positions: dict[str, tuple[int, int]], dirty: set[tuple[int, int]], state_scalar: int) -> tuple[bytes, dict[str, int]]:
    mask = bytearray(scene.width * scene.height)
    pixels = spans = 0
    for x, y in dirty:
        pixels += 1
        for caster in _ordered_casters(scene):
            cx, cy = positions[caster.name]
            radius = caster.radius + caster.elevation // 4 + 1
            if not _eligible(caster, x, y, scene) or (x - cx) ** 2 + (y - cy) ** 2 > radius * radius:
                continue
            center = scene.radiance[_idx(scene, x, y)]
            neighbors = [
                scene.radiance[_idx(scene, nx, ny)]
                for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))
                if 0 <= nx < scene.width and 0 <= ny < scene.height
            ]
            if neighbors and center * 2 < sum(neighbors) and state_scalar:
                strength = (sum(neighbors) - center * 2) * state_scalar // (2 * 255)
                mask[_idx(scene, x, y)] = max(mask[_idx(scene, x, y)], min(96, strength))
        if mask[_idx(scene, x, y)]:
            spans += 1
    return bytes(mask), {"processed_pixels": pixels, "processed_spans": spans, "allocations": 1, "memory_bytes": len(mask)}


def _directional(scene: Scene, positions: dict[str, tuple[int, int]], dirty: set[tuple[int, int]], direction: tuple[int, int], state_scalar: int) -> tuple[bytes, dict[str, int]]:
    mask = bytearray(scene.width * scene.height)
    pixels = spans = 0
    dx, dy = direction
    for x, y in dirty:
        pixels += 1
        for caster in _ordered_casters(scene):
            cx, cy = positions[caster.name]
            if not _eligible(caster, x, y, scene) or state_scalar == 0:
                continue
            distance = (x - cx) * dx + (y - cy) * dy
            lateral = abs((x - cx) * dy - (y - cy) * dx)
            if distance > 0 and distance <= 6 + caster.elevation and lateral <= caster.radius:
                strength = caster.opacity * caster.fade * state_scalar // (255 * 8 * 255)
                mask[_idx(scene, x, y)] = max(mask[_idx(scene, x, y)], strength)
        if mask[_idx(scene, x, y)]:
            spans += 1
    return bytes(mask), {"processed_pixels": pixels, "processed_spans": spans, "allocations": 1, "memory_bytes": len(mask)}


def _oracle_direction(scene: Scene, state: str, frame: int) -> tuple[int, int]:
    """Return a benchmark-only direction; production clients do not have it."""
    if state == "timed-keyframe":
        return scene.directions[(frame // 96) % len(scene.directions)][1:]
    for name, dx, dy in scene.directions:
        if name == state:
            return dx, dy
    if state in {"night", "full-moon", "new-moon", "starlight-only"}:
        return next((dx, dy) for name, dx, dy in scene.directions if name == "night-starlight")
    return 0, 0


def _frame_positions(scene: Scene, frame: int, path: list[dict[str, int]]) -> dict[str, tuple[int, int]]:
    step = path[frame % len(path)]
    return {caster.name: (_clamp(caster.x + (step["x"] if caster.animated else 0), 0, scene.width - 1), _clamp(caster.y + (step["y"] if caster.animated else 0), 0, scene.height - 1)) for caster in scene.casters}


def _run_technique(scene: Scene, technique: str, path: list[dict[str, int]], state_scalar: int, oracle_state: str) -> dict[str, Any]:
    previous: dict[str, tuple[int, int]] | None = None
    previous_mask = bytes(scene.width * scene.height)
    samples: list[int] = []
    checkpoints: list[str] = []
    totals = {"processed_pixels": 0, "processed_spans": 0, "allocations": 0, "memory_bytes": 0, "cache_hits": 0, "cache_misses": 0, "dirty_area": 0}
    for frame in range(MOVEMENT_FRAMES + IDLE_FRAMES):
        positions = _frame_positions(scene, frame if frame < MOVEMENT_FRAMES else MOVEMENT_FRAMES - 1, path)
        dirty = _dirty_cells(scene, positions, previous if frame < MOVEMENT_FRAMES else positions)
        if not dirty:
            totals["cache_hits"] += 1
            samples.append(0)
            if frame in (0, 59, 239, 479, 495):
                checkpoints.append(_sha256(previous_mask))
            continue
        totals["cache_misses"] += 1
        if technique == "contact_blob":
            mask, counters = _contact(scene, positions, dirty, state_scalar)
        elif technique == "field_screen_space":
            mask, counters = _field(scene, positions, dirty, state_scalar)
        else:
            direction = _oracle_direction(scene, oracle_state, frame)
            mask, counters = _directional(scene, positions, dirty, direction, state_scalar)
        work_units = counters["processed_pixels"] + counters["processed_spans"] * 2
        samples.append(work_units)
        for key in ("processed_pixels", "processed_spans", "allocations", "memory_bytes"):
            totals[key] += counters[key]
        totals["dirty_area"] += len(dirty)
        previous_mask = mask
        if frame in (0, 59, 239, 479, 495):
            checkpoints.append(_sha256(mask))
        previous = positions
    movement_samples = samples[:MOVEMENT_FRAMES]
    idle_samples = samples[MOVEMENT_FRAMES:]
    return {
        "technique": technique,
        "metrics": {
            **totals,
            "frame_p50_work_units": int(statistics.median(movement_samples)),
            "frame_p95_work_units": int(statistics.quantiles(movement_samples, n=20, method="inclusive")[18]),
            "idle_nonzero_frames": sum(value != 0 for value in idle_samples),
            "checkpoints_sha256": checkpoints,
            "final_mask_sha256": _sha256(previous_mask),
        },
    }


def measure(fixture_path: Path) -> dict[str, Any]:
    root, scenes = _parse_fixture(fixture_path)
    results: list[dict[str, Any]] = []
    light_cases = root["light_cases"]
    for scene in scenes:
        for case in light_cases:
            for technique in TECHNIQUES:
                scalar = 0 if case["negative"] else case["scalar"]
                result = _run_technique(scene, technique, root["workload"]["path"], scalar, case["state"])
                result["scene"] = scene.name
                result["light_case"] = case["state"]
                result["light_case_input"] = {
                    "requested_scalar": case["scalar"],
                    "effective_scalar": scalar,
                    "rgb": case["rgb"],
                    "rgb_peak": max(case["rgb"]),
                    "negative": case["negative"],
                    "timed": case["timed"],
                }
                if technique == "directional_silhouette":
                    direction = _oracle_direction(scene, case["state"], 0)
                    result["oracle_direction"] = [direction[0], direction[1]]
                results.append(result)
    directional = [result for result in results if result["technique"] == "directional_silhouette"]
    return {
        "schema_version": SCHEMA_VERSION,
        "benchmark": "client-shadow-prototypes",
        "source_fixture": root["source_fixture"],
        "matrix": {
            "identical_scene_inputs": True,
            "techniques": list(TECHNIQUES),
            "render_order": "ascending physical depth, then caster name",
            "oracle_direction_only": True,
            "production_direction_input_available": False,
            "light_cases": [case["state"] for case in light_cases],
        },
        "results": results,
        "decision": {
            "status": "rejected-for-production",
            "reason": "No candidate can produce temporally valid solar/lunar cast silhouettes from the aggregate field without a separate bounded viewer-authorized direction contract.",
            "best_bounded_visual_alternative": "field_screen_space",
            "fallback": "retain existing aggregate field lighting and actor/effect fades; do not render a projected shadow",
            "maximum_incremental_work_units": 12000,
            "directional_oracle_checkpoints": [result["metrics"]["checkpoints_sha256"] for result in directional],
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        evidence = measure(args.fixture.resolve())
    except (OSError, PrototypeError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    for result in evidence["results"]:
        metrics = result["metrics"]
        print(f"{result['scene']}/{result['technique']}: p50={metrics['frame_p50_work_units']} p95={metrics['frame_p95_work_units']} idle_nonzero={metrics['idle_nonzero_frames']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
