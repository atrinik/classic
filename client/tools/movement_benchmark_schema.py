#!/usr/bin/env python3
"""Closed schema validation for deterministic movement benchmark records."""

from __future__ import annotations

import hashlib
import math
import re


EXPECTED_SAMPLES = {"cold": 1, "sustained": 480, "idle": 16, "resumed": 80}
EXPECTED_PACKETS = {"cold": 1, "sustained": 480, "idle": 8, "resumed": 80}
EXPECTED_CHANGED = {"cold": 1, "sustained": 480, "idle": 0, "resumed": 80}
EXPECTED_FULL_MAP_DRAWS = {"cold": 1, "sustained": 480, "idle": 0, "resumed": 80}
# Visibility fades request animation-only work only while an alpha transition
# is active. Once the bounded fade/stale lifecycle settles, idle ticks must not
# rebuild or repaint the map.
EXPECTED_ANIMATION_DRAWS = {"cold": 0, "sustained": 0, "idle": 0, "resumed": 0}
LIGHTING_SAMPLE_BYTES = 10
EXPECTED_LOCAL_MINIMAP_DRAWS = {"cold": 1, "sustained": 240, "idle": 0, "resumed": 40}
EXPECTED_CHECKPOINTS = (
    "cold",
    "step_b",
    "restored_a_1",
    "step_c",
    "restored_a_2",
    "sustained_end",
    "idle_end",
    "resumed_end",
    "resized",
    "resize_restored",
    "reset",
    "transition",
)
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
STATE_DIGEST = re.compile(r"[0-9a-f]{16}\Z")
DRAW_REASON_FIELDS = {
    "external",
    "packet",
    "scroll",
    "animation",
    "lighting",
    "resize",
    "ui",
}
LIFECYCLE_REASON_FIELDS = {
    "reset_packet",
    "changed_map_packet",
    "noop_map_packet",
    "animation_only_tick",
    "resize",
    "map_transition",
}
LIGHTING_COUNTER_FIELDS = {
    "field_begins",
    "field_dirty_marks",
    "field_dirty_pixels",
    "field_rasterized_quads",
    "field_translations",
    "field_translated_pixels",
    "field_translated_bytes",
    "field_scroll_x_pixels",
    "field_scroll_y_pixels",
    "field_translation_fallback_active",
    "field_translation_fallback_bounds",
    "field_translation_fallback_control",
    "field_partial_rebuilds",
    "field_full_rebuilds",
    "field_full_rebuild_cache",
    "field_full_rebuild_active",
    "field_full_rebuild_bounds",
    "field_full_rebuild_control",
    "field_full_rebuild_other",
    "field_rebuilds",
    "field_reuses",
    "render_calls",
    "render_failures",
    "lit_sprite_draws",
    "lit_sprite_lookups",
    "lit_sprite_hits",
    "lit_sprite_misses",
    "lit_sprite_insertions",
    "lit_sprite_evictions",
    "lit_sprite_fallbacks",
    "lit_sprite_clears",
    "lit_sprite_cleared_entries",
    "lit_sprite_structure_lookups",
    "lit_sprite_structure_hits",
    "lit_sprite_structure_misses",
    "lit_sprite_structure_constructions",
    "lit_sprite_structure_insertions",
    "lit_sprite_structure_evictions",
    "lit_sprite_structure_invalidations",
    "lit_sprite_projected_lookups",
    "lit_sprite_projected_hits",
    "lit_sprite_projected_misses",
    "lit_sprite_projected_constructions",
    "lit_sprite_projected_insertions",
    "lit_sprite_projected_evictions",
    "lit_sprite_projected_invalidations",
    "lit_sprite_invalidation_field",
    "lit_sprite_invalidation_scroll",
    "lit_sprite_invalidation_source",
    "lit_sprite_invalidation_reset",
    "lit_sprite_invalidation_eviction",
}
LIGHTING_TIMING_FIELDS = {
    "translation",
    "dirty_clear",
    "rasterization",
    "extrapolation",
    "tone_map_multiply",
    "sprite_lookup",
    "sprite_construction",
    "sprite_invalidation",
}
LIGHTING_STATE_FIELDS = {
    "allocated_levels",
    "active_levels",
    "cache_valid_levels",
    "dirty_levels",
    "lit_sprite_entries",
    "lit_sprite_bytes",
    "retained_field_bytes",
    "state_digest",
}
RENDER_STAGES = {
    "map": "per_map_draw",
    "pointer": "per_frame",
    "map_scratch_clear": "per_map_draw",
    "ground": "per_level",
    "ground_composite": "per_map_draw",
    "lighting": "per_level",
    "objects": "per_level",
    "paint": "per_map_draw",
    "ui": "per_map_draw",
    "command_sort": "per_map_draw",
    "living_occlusion": "per_map_draw",
    "sprite_effects": "per_map_draw",
    "hint_replay": "per_map_draw",
}


def _mapping(value: object, fields: set[str], context: str) -> dict[str, object]:
    if not isinstance(value, dict) or set(value) != fields:
        raise ValueError(f"movement benchmark {context} has an incompatible schema")
    return value


def _integer(value: object, context: str, *, positive: bool = False) -> int:
    if type(value) is not int or value < (1 if positive else 0):
        raise ValueError(f"movement benchmark {context} is invalid")
    return value


def _validate_sustained_lit_sprite_retention(counters: dict[str, int]) -> None:
    constructions = (
        counters["lit_sprite_structure_constructions"]
        + counters["lit_sprite_projected_constructions"]
    )
    if (
        counters["lit_sprite_hits"] == 0
        or constructions * 10 > counters["lit_sprite_lookups"]
        or counters["lit_sprite_invalidation_scroll"] != 0
        or counters["lit_sprite_invalidation_field"] != 0
    ):
        raise ValueError("movement benchmark reconstructed unchanged lit sprites")


def _number(value: object, context: str, *, positive: bool = False) -> float:
    if type(value) not in (int, float) or not math.isfinite(value) \
            or value < 0 or (positive and value == 0):
        raise ValueError(f"movement benchmark {context} is invalid")
    return float(value)


def _digest(value: object, pattern: re.Pattern[str], context: str) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise ValueError(f"movement benchmark {context} is invalid")
    return value


def _timing(
    value: object,
    samples: int,
    context: str,
    *,
    allow_empty: bool = False,
    allow_zero: bool = False,
) -> dict[str, object]:
    timing = _mapping(
        value,
        {"unit", "samples", "p50", "p95", "p99", "max", "windows"},
        context,
    )
    if timing["unit"] != "ns" or timing["samples"] != samples:
        raise ValueError(f"movement benchmark {context} identity is invalid")
    if samples == 0:
        if not allow_empty or any(timing[field] != 0 for field in ("p50", "p95", "p99", "max")) \
                or timing["windows"] != []:
            raise ValueError(f"movement benchmark {context} empty timing is invalid")
        return timing
    quantiles = [
        _integer(timing[field], f"{context} {field}", positive=not allow_zero)
        for field in ("p50", "p95", "p99", "max")
    ]
    if quantiles != sorted(quantiles):
        raise ValueError(f"movement benchmark {context} quantiles are invalid")
    windows = timing["windows"]
    if not isinstance(windows, list) or len(windows) != (samples + 31) // 32:
        raise ValueError(f"movement benchmark {context} windows are incomplete")
    covered = 0
    for index, item in enumerate(windows):
        window = _mapping(
            item,
            {"start_tick", "samples", "p95_ns"},
            f"{context} window {index}",
        )
        if window["start_tick"] != covered:
            raise ValueError(f"movement benchmark {context} window order is invalid")
        window_samples = _integer(
            window["samples"], f"{context} window samples", positive=True
        )
        if window_samples != min(32, samples - covered):
            raise ValueError(f"movement benchmark {context} window partition is invalid")
        window_p95 = _integer(
            window["p95_ns"], f"{context} window p95", positive=not allow_zero
        )
        if window_p95 > timing["max"]:
            raise ValueError(f"movement benchmark {context} window exceeds its maximum")
        covered += window_samples
    if covered != samples:
        raise ValueError(f"movement benchmark {context} windows are incomplete")
    return timing


def _draw_reasons(value: object, draws: int, context: str) -> dict[str, object]:
    reasons = _mapping(value, DRAW_REASON_FIELDS, context)
    counts = [_integer(item, f"{context} {field}") for field, item in reasons.items()]
    # Reasons are bit flags: one draw can have several coalesced reasons. Every
    # draw must have at least one reason, but their sum need not equal draws.
    if any(item > draws for item in counts) or sum(counts) < draws:
        raise ValueError(f"movement benchmark {context} accounting is invalid")
    return reasons


def _lighting_counter_accounting(counters: dict[str, object], context: str) -> None:
    if counters["lit_sprite_hits"] + counters["lit_sprite_misses"] \
            != counters["lit_sprite_lookups"]:
        raise ValueError(f"movement benchmark {context} lit-sprite accounting is invalid")
    mode_prefixes = ("lit_sprite_structure", "lit_sprite_projected")
    for prefix in mode_prefixes:
        if (
            counters[f"{prefix}_hits"] + counters[f"{prefix}_misses"]
            != counters[f"{prefix}_lookups"]
            or counters[f"{prefix}_constructions"] != counters[f"{prefix}_misses"]
            or counters[f"{prefix}_insertions"] > counters[f"{prefix}_constructions"]
            or counters[f"{prefix}_evictions"] > counters[f"{prefix}_invalidations"]
        ):
            raise ValueError(f"movement benchmark {context} {prefix} accounting is invalid")
    for event in ("lookups", "hits", "misses", "insertions", "evictions"):
        if sum(counters[f"{prefix}_{event}"] for prefix in mode_prefixes) \
                != counters[f"lit_sprite_{event}"]:
            raise ValueError(
                f"movement benchmark {context} lit-sprite mode accounting is invalid"
            )
    invalidations = sum(counters[f"{prefix}_invalidations"] for prefix in mode_prefixes)
    causes = sum(
        counters[f"lit_sprite_invalidation_{cause}"]
        for cause in ("field", "scroll", "source", "reset", "eviction")
    )
    cleared = sum(
        counters[f"lit_sprite_invalidation_{cause}"]
        for cause in ("field", "scroll", "reset")
    )
    if (
        invalidations != causes
        or counters["lit_sprite_invalidation_eviction"] != counters["lit_sprite_evictions"]
        or counters["lit_sprite_cleared_entries"] != cleared
    ):
        raise ValueError(f"movement benchmark {context} invalidation cause is incomplete")


def _lighting(value: object, context: str) -> None:
    lighting = _mapping(
        value,
        {"available", "start", "end", "peak", "counters", "timings", "levels"},
        context,
    )
    if lighting["available"] is not True:
        raise ValueError(f"movement benchmark {context} availability is invalid")
    for boundary in ("start", "end"):
        state = _mapping(lighting[boundary], LIGHTING_STATE_FIELDS, f"{context} {boundary}")
        for field in LIGHTING_STATE_FIELDS - {"state_digest"}:
            _integer(state[field], f"{context} {boundary} {field}")
        _digest(state["state_digest"], STATE_DIGEST, f"{context} {boundary} digest")
    peak = _mapping(lighting["peak"], LIGHTING_STATE_FIELDS - {"state_digest"}, f"{context} peak")
    for field, item in peak.items():
        _integer(item, f"{context} peak {field}")
    for field in LIGHTING_STATE_FIELDS - {"state_digest"}:
        if peak[field] < max(lighting["start"][field], lighting["end"][field]):
            raise ValueError(f"movement benchmark {context} peak state is invalid")
    for boundary in ("start", "end"):
        state = lighting[boundary]
        if state["active_levels"] > state["allocated_levels"] \
                or state["cache_valid_levels"] > state["allocated_levels"] \
                or state["dirty_levels"] > state["allocated_levels"]:
            raise ValueError(f"movement benchmark {context} level state is invalid")
    if lighting["end"]["active_levels"] != 0 or lighting["end"]["dirty_levels"] != 0:
        raise ValueError(f"movement benchmark {context} did not settle its lighting state")
    counters = _mapping(lighting["counters"], LIGHTING_COUNTER_FIELDS, f"{context} counters")
    for field, item in counters.items():
        _integer(item, f"{context} counters {field}")
    _lighting_counter_accounting(counters, context)
    if counters["render_failures"] != 0 or counters["lit_sprite_fallbacks"] != 0:
        raise ValueError(f"movement benchmark {context} rendering failed")
    timings = _mapping(lighting["timings"], LIGHTING_TIMING_FIELDS, f"{context} timings")
    for name, timing in timings.items():
        timing = _mapping(timing, {"unit", "calls", "elapsed"}, f"{context} {name} timing")
        if timing["unit"] != "ns":
            raise ValueError(f"movement benchmark {context} timing unit is invalid")
        _integer(timing["calls"], f"{context} {name} timing calls")
        _integer(timing["elapsed"], f"{context} {name} timing elapsed")
        if (timing["calls"] == 0) != (timing["elapsed"] == 0):
            raise ValueError(f"movement benchmark {context} {name} timing is contradictory")
    levels = lighting["levels"]
    if not isinstance(levels, list) or len(levels) != 13:
        raise ValueError(f"movement benchmark {context} levels are incomplete")
    parsed_levels: list[dict[str, object]] = []
    for expected_depth, item in zip(range(-6, 7), levels, strict=True):
        level = _mapping(
            item,
            {"depth", "width", "height", "start", "end", "peak", "counters", "timings"},
            f"{context} level {expected_depth}",
        )
        if level["depth"] != expected_depth:
            raise ValueError(f"movement benchmark {context} level order is invalid")
        for dimension in ("width", "height"):
            _integer(level[dimension], f"{context} level {expected_depth} {dimension}")
        for boundary in ("start", "end"):
            state = _mapping(
                level[boundary],
                {
                    "allocated",
                    "cache_valid",
                    "dirty",
                    "entries",
                    "bytes",
                    "retained_field_bytes",
                    "state_digest",
                },
                f"{context} level {expected_depth} {boundary}",
            )
            if (
                type(state["allocated"]) is not bool
                or type(state["cache_valid"]) is not bool
                or type(state["dirty"]) is not bool
            ):
                raise ValueError(f"movement benchmark {context} level state is invalid")
            for field in ("entries", "bytes", "retained_field_bytes"):
                _integer(state[field], f"{context} level {expected_depth} {field}")
            _digest(state["state_digest"], STATE_DIGEST, f"{context} level digest")
        level_peak = _mapping(
            level["peak"], {"entries", "bytes", "retained_field_bytes"}, f"{context} level peak"
        )
        for field, item in level_peak.items():
            _integer(item, f"{context} level peak {field}")
            if item < max(level["start"][field], level["end"][field]):
                raise ValueError(f"movement benchmark {context} level peak is invalid")
        level_counters = _mapping(
            level["counters"], LIGHTING_COUNTER_FIELDS, f"{context} level counters"
        )
        for field, item in level_counters.items():
            _integer(item, f"{context} level counter {field}")
        _lighting_counter_accounting(
            level_counters, f"{context} level {expected_depth}"
        )
        level_timings = _mapping(
            level["timings"], LIGHTING_TIMING_FIELDS, f"{context} level timings"
        )
        for name, timing in level_timings.items():
            timing = _mapping(
                timing,
                {"unit", "calls", "elapsed"},
                f"{context} level {expected_depth} {name} timing",
            )
            if timing["unit"] != "ns":
                raise ValueError(f"movement benchmark {context} level timing is invalid")
            _integer(timing["calls"], f"{context} level timing calls")
            _integer(timing["elapsed"], f"{context} level timing elapsed")
            if (timing["calls"] == 0) != (timing["elapsed"] == 0):
                raise ValueError(
                    f"movement benchmark {context} level {name} timing is contradictory"
                )
        if level_counters["render_failures"] != 0 \
                or level_counters["lit_sprite_fallbacks"] != 0:
            raise ValueError(f"movement benchmark {context} level rendering failed")
        for boundary in ("start", "end"):
            if not level[boundary]["allocated"] and (
                level[boundary]["cache_valid"]
                or level[boundary]["dirty"]
                or any(
                    level[boundary][field] != 0
                    for field in ("entries", "bytes", "retained_field_bytes")
                )
            ):
                raise ValueError(f"movement benchmark {context} unallocated level has state")
        if level["end"]["allocated"] != (level["width"] > 0 and level["height"] > 0):
            raise ValueError(f"movement benchmark {context} level dimensions are invalid")
        parsed_levels.append(level)

    full_cause_fields = (
        "field_full_rebuild_cache",
        "field_full_rebuild_active",
        "field_full_rebuild_bounds",
        "field_full_rebuild_control",
        "field_full_rebuild_other",
    )
    for item_context, item_counters in (
        (context, counters),
        *(
            (f"{context} level {level['depth']}", level["counters"])
            for level in parsed_levels
        ),
    ):
        if (
            sum(item_counters[field] for field in full_cause_fields)
            != item_counters["field_full_rebuilds"]
            or item_counters["field_full_rebuild_other"] != 0
        ):
            raise ValueError(
                f"movement benchmark {item_context} full rebuild cause is incomplete"
            )

    stable_counter_fields = LIGHTING_COUNTER_FIELDS - {
        "lit_sprite_clears",
        "lit_sprite_cleared_entries",
    }
    if any(
        counters[field] != sum(level["counters"][field] for level in parsed_levels)
        for field in stable_counter_fields
    ):
        raise ValueError(f"movement benchmark {context} per-level counters are inconsistent")
    for name in LIGHTING_TIMING_FIELDS:
        for field in ("calls", "elapsed"):
            level_total = sum(level["timings"][name][field] for level in parsed_levels)
            if timings[name][field] != level_total:
                raise ValueError(
                    f"movement benchmark {context} per-level {name} {field} "
                    f"is inconsistent ({timings[name][field]} != {level_total})"
                )
    for aggregate_field, level_field in (
        ("lit_sprite_entries", "entries"),
        ("lit_sprite_bytes", "bytes"),
        ("retained_field_bytes", "retained_field_bytes"),
    ):
        if peak[aggregate_field] < max(
            level["peak"][level_field] for level in parsed_levels
        ):
            raise ValueError(f"movement benchmark {context} aggregate peak is inconsistent")

    for boundary in ("start", "end"):
        aggregate = {
            "allocated_levels": sum(
                level[boundary]["allocated"] for level in parsed_levels
            ),
            "cache_valid_levels": sum(
                level[boundary]["cache_valid"] for level in parsed_levels
            ),
            "dirty_levels": sum(level[boundary]["dirty"] for level in parsed_levels),
            "lit_sprite_entries": sum(
                level[boundary]["entries"] for level in parsed_levels
            ),
            "lit_sprite_bytes": sum(level[boundary]["bytes"] for level in parsed_levels),
            "retained_field_bytes": sum(
                level[boundary]["retained_field_bytes"] for level in parsed_levels
            ),
        }
        if any(lighting[boundary][field] != total for field, total in aggregate.items()):
            raise ValueError(f"movement benchmark {context} aggregate state is inconsistent")


def _checkpoint_list(value: object, context: str) -> list[dict[str, object]]:
    if not isinstance(value, list) or len(value) != len(EXPECTED_CHECKPOINTS):
        raise ValueError(f"movement benchmark {context} is incomplete")
    result: list[dict[str, object]] = []
    for expected_name, item in zip(EXPECTED_CHECKPOINTS, value, strict=True):
        checkpoint = _mapping(
            item,
            {"name", "pixels_sha256", "state_digest", "map_x", "map_y", "viewport_width", "viewport_height"},
            f"{context} {expected_name}",
        )
        if checkpoint["name"] != expected_name:
            raise ValueError(f"movement benchmark {context} order is invalid")
        _digest(checkpoint["pixels_sha256"], SHA256, f"{context} {expected_name} pixels")
        _digest(checkpoint["state_digest"], STATE_DIGEST, f"{context} {expected_name} state")
        _integer(checkpoint["map_x"], f"{context} {expected_name} map x")
        _integer(checkpoint["map_y"], f"{context} {expected_name} map y")
        _integer(checkpoint["viewport_width"], f"{context} {expected_name} width", positive=True)
        _integer(checkpoint["viewport_height"], f"{context} {expected_name} height", positive=True)
        result.append(checkpoint)
    return result


def _visual_lifecycle_digest(checkpoints: list[dict[str, object]]) -> str:
    digest = hashlib.sha256(b"pvm-checkpoints-v1\n")
    for checkpoint in checkpoints:
        digest.update(
            (
                f"{checkpoint['name']}\t{checkpoint['pixels_sha256']}\t"
                f"{checkpoint['map_x']}\t{checkpoint['map_y']}\t"
                f"{checkpoint['viewport_width']}\t{checkpoint['viewport_height']}\n"
            ).encode("ascii")
        )
    return digest.hexdigest()


def validate_record(value: object) -> dict[str, object]:
    """Validate and return one complete version-eight native record."""
    record = _mapping(
        value,
        {
            "schema_version",
            "benchmark",
            "tick_ms",
            "simulated_tick_hz",
            "identity",
            "fixture",
            "checkpoint_sha256",
            "same_process_checkpoint_sha256",
            "final_state_digest",
            "same_process_final_state_digest",
            "movement_route",
            "checkpoints",
            "same_process_checkpoints",
            "lifecycle",
            "process_peak_rss_bytes",
            "process_peak_rss_available",
            "phases",
        },
        "record",
    )
    if record["schema_version"] != 9 or record["benchmark"] != "player-view-movement" \
            or record["tick_ms"] != 125 or record["simulated_tick_hz"] != 8:
        raise ValueError("movement benchmark emitted an incompatible schema")
    checkpoint = _digest(record["checkpoint_sha256"], SHA256, "checkpoint")
    if record["same_process_checkpoint_sha256"] != checkpoint:
        raise ValueError("movement benchmark checkpoint is not deterministic")
    final_state = _digest(record["final_state_digest"], STATE_DIGEST, "final state")
    if record["same_process_final_state_digest"] != final_state:
        raise ValueError("movement benchmark final state is not deterministic")
    checkpoints = _checkpoint_list(record["checkpoints"], "checkpoints")
    repeated = _checkpoint_list(record["same_process_checkpoints"], "same-process checkpoints")
    if checkpoints != repeated:
        raise ValueError("movement benchmark lifecycle checkpoints are not deterministic")
    if final_state != checkpoints[-1]["state_digest"]:
        raise ValueError("movement benchmark final state does not match its transition")
    lifecycle = _mapping(record["lifecycle"], {"full_map_draws", "full_draw_reasons"}, "lifecycle")
    if lifecycle["full_map_draws"] != 4:
        raise ValueError("movement benchmark lifecycle draw accounting is invalid")
    lifecycle_reasons = _mapping(
        lifecycle["full_draw_reasons"], LIFECYCLE_REASON_FIELDS, "lifecycle reasons"
    )
    for field, value in lifecycle_reasons.items():
        _integer(value, f"lifecycle reasons {field}")
    if lifecycle_reasons != {
        "reset_packet": 1,
        "changed_map_packet": 0,
        "noop_map_packet": 0,
        "animation_only_tick": 0,
        "resize": 2,
        "map_transition": 1,
    }:
        raise ValueError("movement benchmark lifecycle reason accounting is invalid")
    _integer(record["process_peak_rss_bytes"], "process peak RSS")
    if type(record["process_peak_rss_available"]) is not bool:
        raise ValueError("movement benchmark process peak RSS availability is invalid")
    if record["process_peak_rss_available"] != (record["process_peak_rss_bytes"] > 0):
        raise ValueError("movement benchmark process peak RSS availability is inconsistent")

    route = _mapping(
        record["movement_route"],
        {"clock", "coordinate_system", "character_speed_inferred", "steps"},
        "movement route",
    )
    if route["clock"] != "simulated-monotonic-us" or route["coordinate_system"] != "map-origin" \
            or route["character_speed_inferred"] is not False:
        raise ValueError("movement benchmark route metadata is invalid")
    steps = route["steps"]
    if not isinstance(steps, list) or len(steps) != 4:
        raise ValueError("movement benchmark route steps are incomplete")
    expected_route = ((11, 10), (10, 10), (10, 11), (10, 10))
    previous_received = -1
    previous_applied = -1
    for index, step_value in enumerate(steps):
        step = _mapping(
            step_value,
            {"packet_index", "phase_tick", "received_us", "applied_us", "map_x", "map_y"},
            "movement route step",
        )
        for field in ("packet_index", "phase_tick", "received_us", "applied_us", "map_x", "map_y"):
            _integer(step[field], f"movement route {field}")
        if (step["packet_index"], step["phase_tick"]) != (index, index) \
                or (step["map_x"], step["map_y"]) != expected_route[index] \
                or step["received_us"] <= previous_received \
                or step["applied_us"] < step["received_us"] \
                or step["applied_us"] < previous_applied:
            raise ValueError("movement benchmark route timing or origin is invalid")
        previous_received = step["received_us"]
        previous_applied = step["applied_us"]

    identity = _mapping(record["identity"], {"instrumentation", "implementation", "run"}, "identity")
    instrumentation = _mapping(
        identity["instrumentation"],
        {
            "schema_version",
            "fixture_schema_version",
            "workload",
            "lighting_statistics_version",
            "map_statistics_version",
            "render_profiler_statistics_version",
            "sprite_cache_statistics_version",
        },
        "instrumentation identity",
    )
    if instrumentation != {
        "schema_version": 9,
        "fixture_schema_version": 3,
        "workload": "pvm1-map2-lifecycle-v4",
        "lighting_statistics_version": 6,
        "map_statistics_version": 3,
        "render_profiler_statistics_version": 5,
        "sprite_cache_statistics_version": 3,
    }:
        raise ValueError("movement benchmark instrumentation identity is invalid")
    implementation = _mapping(
        identity["implementation"],
        {
            "revision",
            "dirty",
            "dirty_known",
            "build_type",
            "compiler_id",
            "compiler_version",
            "cmake_system",
            "sdl_version",
            "sdl_platform",
        },
        "implementation identity",
    )
    for field in implementation.keys() - {"dirty", "dirty_known"}:
        if not isinstance(implementation[field], str) or not implementation[field]:
            raise ValueError("movement benchmark implementation identity is invalid")
    if type(implementation["dirty_known"]) is not bool or (
        implementation["dirty_known"] and type(implementation["dirty"]) is not bool
    ) or (not implementation["dirty_known"] and implementation["dirty"] is not None):
        raise ValueError("movement benchmark implementation dirty identity is invalid")
    run = _mapping(
        identity["run"],
        {
            "runner_os",
            "runner_arch",
            "ci",
            "cpu_count",
            "cpu_model",
            "runner_image_os",
            "runner_image_version",
            "viewport",
            "mode",
            "reconstruction",
            "workload_variant",
            "fine_timing",
        },
        "run identity",
    )
    for field in ("runner_os", "runner_arch", "ci", "cpu_model", "runner_image_os", "runner_image_version", "mode", "reconstruction", "workload_variant"):
        if not isinstance(run[field], str) or not run[field]:
            raise ValueError("movement benchmark run identity is invalid")
    if run["reconstruction"] not in ("translated", "full"):
        raise ValueError("movement benchmark reconstruction identity is invalid")
    if run["workload_variant"] not in ("production", "isolated-lighting"):
        raise ValueError("movement benchmark workload identity is invalid")
    if run["mode"] not in ("smooth", "discrete"):
        raise ValueError("movement benchmark run mode is invalid")
    if type(run["fine_timing"]) is not bool:
        raise ValueError("movement benchmark fine timing identity is invalid")
    _integer(run["cpu_count"], "run identity CPU count", positive=True)
    viewport = _mapping(run["viewport"], {"name", "width", "height"}, "viewport identity")
    if viewport["name"] not in ("standard", "large", "brynknot"):
        raise ValueError("movement benchmark viewport identity is invalid")
    expected_viewport = {
        "standard": (320, 240),
        "large": (1920, 1080),
        "brynknot": (1024, 780),
    }.get(viewport["name"])
    if expected_viewport is None:
        raise ValueError("movement benchmark viewport identity is invalid")
    if (viewport["width"], viewport["height"]) != expected_viewport:
        raise ValueError("movement benchmark viewport identity is invalid")

    fixture = _mapping(
        record["fixture"],
        {
            "manifest_schema_version",
            "manifest_sha256",
            "snapshot_sha256",
            "movement_stream_sha256",
            "transition_snapshot_sha256",
            "expected_standard_checkpoint_sha256",
            "look_width",
            "look_height",
            "resize_width_delta",
            "resize_height_delta",
            "rng_seed",
            "smooth_lighting",
        },
        "fixture",
    )
    if fixture["manifest_schema_version"] != 1 or type(fixture["smooth_lighting"]) is not bool:
        raise ValueError("movement benchmark fixture is invalid")
    for field in (
        "manifest_sha256",
        "snapshot_sha256",
        "movement_stream_sha256",
        "transition_snapshot_sha256",
        "expected_standard_checkpoint_sha256",
    ):
        _digest(fixture[field], SHA256, f"fixture {field}")
    if (
        fixture["look_width"] != 17
        or fixture["look_height"] != 17
        or fixture["resize_width_delta"] != 32
        or fixture["resize_height_delta"] != 24
        or fixture["rng_seed"] != 0x1961932026
    ):
        raise ValueError("movement benchmark fixture workload identity is invalid")
    if run["mode"] != ("smooth" if fixture["smooth_lighting"] else "discrete"):
        raise ValueError("movement benchmark run mode and fixture are inconsistent")

    origin_x = checkpoints[0]["map_x"]
    origin_y = checkpoints[0]["map_y"]
    base_width = viewport["width"]
    base_height = viewport["height"]
    expected_coordinates = {
        "step_b": (origin_x + 1, origin_y),
        "step_c": (origin_x, origin_y + 1),
    }
    for item in checkpoints:
        expected_x, expected_y = expected_coordinates.get(item["name"], (origin_x, origin_y))
        expected_width, expected_height = (
            (base_width + fixture["resize_width_delta"],
             base_height + fixture["resize_height_delta"])
            if item["name"] == "resized"
            else (base_width, base_height)
        )
        if (item["map_x"], item["map_y"]) != (expected_x, expected_y) \
                or (item["viewport_width"], item["viewport_height"]) != (
                    expected_width, expected_height
                ):
            raise ValueError("movement benchmark lifecycle checkpoint geometry is invalid")
    if checkpoint != _visual_lifecycle_digest(checkpoints):
        raise ValueError("movement benchmark checkpoint does not cover its visual lifecycle")
    if viewport["name"] in ("standard", "brynknot") and checkpoint != fixture[
        "expected_standard_checkpoint_sha256"
    ]:
        raise ValueError("movement benchmark standard checkpoint does not match its fixture golden")

    phases = record["phases"]
    if not isinstance(phases, list) or len(phases) != len(EXPECTED_SAMPLES):
        raise ValueError("movement benchmark phases are incomplete")
    for (name, samples), phase_value in zip(EXPECTED_SAMPLES.items(), phases, strict=True):
        phase = _mapping(
            phase_value,
            {
                "name",
                "samples",
                "map_packets",
                "changed_map_packets",
                "noop_map_packets",
                "full_map_draws",
                "animation_draws",
                "animation_ticks",
                "draw_reasons",
                "frame_time",
                "main_loop",
                "map_time",
                "animation_time",
                "lighting_work_time",
                "local_minimap",
                "queue",
                "map",
                "render_stages",
                "lighting",
                "sprite_cache",
            },
            f"phase {name}",
        )
        if phase["name"] != name or phase["samples"] != samples:
            raise ValueError(f"movement benchmark phase {name} is invalid")
        for field in (
            "map_packets",
            "changed_map_packets",
            "noop_map_packets",
            "full_map_draws",
            "animation_draws",
            "animation_ticks",
        ):
            _integer(phase[field], f"phase {name} {field}")
        if phase["map_packets"] != EXPECTED_PACKETS[name] \
                or phase["changed_map_packets"] != EXPECTED_CHANGED[name] \
                or phase["changed_map_packets"] + phase["noop_map_packets"] != phase["map_packets"] \
                or phase["animation_ticks"] != samples:
            raise ValueError(f"movement benchmark phase {name} accounting is invalid")
        phase_reasons = _draw_reasons(
            phase["draw_reasons"],
            phase["full_map_draws"] + phase["animation_draws"],
            f"phase {name} draw reasons",
        )
        if (
            phase["full_map_draws"] != EXPECTED_FULL_MAP_DRAWS[name]
            or phase["animation_draws"] != EXPECTED_ANIMATION_DRAWS[name]
            or phase_reasons["packet"] != phase["changed_map_packets"]
            or phase_reasons["scroll"] > phase["changed_map_packets"]
            or phase_reasons["animation"] > phase["animation_ticks"]
            or any(phase_reasons[field] != 0 for field in ("external", "lighting", "resize", "ui"))
        ):
            raise ValueError(f"movement benchmark phase {name} draw reason is impossible")
        if name == "cold" and (
            phase["full_map_draws"] != 1
            or phase_reasons["packet"] != 1
        ):
            raise ValueError("movement benchmark cold phase did not render its reset")
        frame_time = _timing(phase["frame_time"], samples, f"phase {name} frame time")
        map_time = _timing(
            phase["map_time"],
            phase["full_map_draws"],
            f"phase {name} map time",
            allow_empty=True,
        )
        del map_time
        animation_time = _timing(
            phase["animation_time"],
            phase["animation_draws"],
            f"phase {name} animation time",
            allow_empty=True,
        )
        del animation_time
        lighting_work_time = _timing(
            phase["lighting_work_time"],
            phase["full_map_draws"],
            f"phase {name} isolated lighting work time",
            allow_empty=True,
            allow_zero=run["mode"] == "discrete" or not run["fine_timing"],
        )
        main_loop = _mapping(
            phase["main_loop"],
            {
                "update_cadence_hz",
                "update_interval_ns",
                "work_time",
                "simulated_wait_time",
                "simulated_update_loop_time",
                "work_capacity_fps",
            },
            f"phase {name} main loop",
        )
        if _number(
            main_loop["update_cadence_hz"],
            f"phase {name} update cadence",
            positive=True,
        ) != 8 or main_loop["update_interval_ns"] != 125_000_000:
            raise ValueError(f"movement benchmark phase {name} update cadence is invalid")
        work_time = _timing(main_loop["work_time"], samples, f"phase {name} work time")
        wait_time = _timing(
            main_loop["simulated_wait_time"],
            samples,
            f"phase {name} simulated wait time",
            allow_zero=True,
        )
        loop_time = _timing(
            main_loop["simulated_update_loop_time"],
            samples,
            f"phase {name} simulated update loop time",
        )
        if work_time != frame_time:
            raise ValueError(f"movement benchmark phase {name} work timing is inconsistent")
        target_tick = main_loop["update_interval_ns"]
        if any(
            loop_time[field] != max(work_time[field], target_tick)
            for field in ("p50", "p95", "p99", "max")
        ) or any(
            loop_window["p95_ns"] != max(work_window["p95_ns"], target_tick)
            for work_window, loop_window in zip(
                work_time["windows"], loop_time["windows"], strict=True
            )
        ):
            raise ValueError(f"movement benchmark phase {name} loop timing is inconsistent")
        del wait_time
        capacity = _mapping(
            main_loop["work_capacity_fps"],
            {"p50", "p95"},
            f"phase {name} work capacity FPS",
        )
        fps_p50 = _number(capacity["p50"], f"phase {name} p50 work capacity", positive=True)
        fps_p95 = _number(capacity["p95"], f"phase {name} p95 work capacity", positive=True)
        expected_p50 = 1_000_000_000 / work_time["p50"]
        expected_p95 = 1_000_000_000 / work_time["p95"]
        if not math.isclose(fps_p50, expected_p50, rel_tol=1e-4) or not math.isclose(
            fps_p95, expected_p95, rel_tol=1e-4
        ):
            raise ValueError(f"movement benchmark phase {name} work capacity is inconsistent")

        local_minimap = _mapping(
            phase["local_minimap"],
            {
                "enabled",
                "update_interval_ms",
                "surface_width",
                "surface_height",
                "legacy_surface_width",
                "legacy_surface_height",
                "map_draws",
                "map_time",
            },
            f"phase {name} local minimap",
        )
        isolated = run["workload_variant"] == "isolated-lighting"
        expected_minimap = (
            local_minimap["enabled"] is False
            and local_minimap["update_interval_ms"] == 0
            and local_minimap["surface_width"] == 0
            and local_minimap["surface_height"] == 0
            and local_minimap["legacy_surface_width"] == 0
            and local_minimap["legacy_surface_height"] == 0
            and local_minimap["map_draws"] == 0
            if isolated
            else local_minimap["enabled"] is True
            and local_minimap["update_interval_ms"] == 250
            and local_minimap["surface_width"] == 1700
            and local_minimap["surface_height"] == 1200
            and local_minimap["legacy_surface_width"] == 1700
            and local_minimap["legacy_surface_height"] == 1200
            and local_minimap["map_draws"] in range(
                EXPECTED_LOCAL_MINIMAP_DRAWS[name],
                EXPECTED_LOCAL_MINIMAP_DRAWS[name] + (2 if name == "resumed" else 1),
            )
            and local_minimap["map_draws"] <= phase["full_map_draws"]
        )
        if not expected_minimap:
            raise ValueError(f"movement benchmark phase {name} local minimap is invalid")
        _timing(
            local_minimap["map_time"],
            local_minimap["map_draws"],
            f"phase {name} local minimap map time",
            allow_empty=True,
        )

        queue = _mapping(
            phase["queue"],
            {
                "enqueued",
                "dequeued",
                "budget_yields",
                "recoveries",
                "start_depth",
                "end_depth",
                "peak_depth",
                "start_bytes",
                "end_bytes",
                "peak_bytes",
                "oldest_age_us",
                "current_oldest_age_us",
                "processing_us",
                "service_clock",
                "simulated_command_us",
                "drain_time",
                "due",
                "budget_due",
                "order_digests_comparable",
                "enqueued_order_digest",
                "dequeued_order_digest",
            },
            f"phase {name} queue",
        )
        for field in queue.keys() - {
            "due",
            "budget_due",
            "order_digests_comparable",
            "service_clock",
            "drain_time",
            "enqueued_order_digest",
            "dequeued_order_digest",
        }:
            _integer(queue[field], f"phase {name} queue {field}")
        if (
            type(queue["due"]) is not bool
            or type(queue["budget_due"]) is not bool
            or type(queue["order_digests_comparable"]) is not bool
        ):
            raise ValueError(f"movement benchmark phase {name} queue state is invalid")
        if queue["service_clock"] != "simulated" \
                or queue["simulated_command_us"] != 5_000 \
                or queue["processing_us"] != queue["dequeued"] * queue["simulated_command_us"]:
            raise ValueError(f"movement benchmark phase {name} queue service is invalid")
        _timing(
            queue["drain_time"],
            samples,
            f"phase {name} queue drain time",
            allow_zero=True,
        )
        enqueued_digest = _digest(queue["enqueued_order_digest"], STATE_DIGEST, f"phase {name} enqueued order")
        dequeued_digest = _digest(queue["dequeued_order_digest"], STATE_DIGEST, f"phase {name} dequeued order")
        if queue["enqueued"] != phase["map_packets"] or queue["dequeued"] != phase["map_packets"] \
                or queue["start_depth"] != 0 or queue["end_depth"] != 0 \
                or queue["start_bytes"] != 0 or queue["end_bytes"] != 0 \
                or queue["current_oldest_age_us"] != 0 or queue["due"] or queue["budget_due"] \
                or not queue["order_digests_comparable"] \
                or enqueued_digest != dequeued_digest:
            raise ValueError(f"movement benchmark phase {name} queue accounting is invalid")

        map_stats = _mapping(
            phase["map"],
            {
                "map_draws",
                "primary_map_draws",
                "auxiliary_map_draws",
                "animation_draws",
                "animation_level_draws",
                "presents",
                "present_failures",
                "render_failures",
                "fault_injections",
                "fault_detections",
                "level_draws",
                "render_commands",
                "annotations",
                "ui_tiles",
                "peak_render_commands",
                "peak_active_levels",
                "renderer_allocation_statistics_available",
                "renderer_allocations",
                "renderer_allocation_bytes",
            },
            f"phase {name} map",
        )
        if type(map_stats["renderer_allocation_statistics_available"]) is not bool:
            raise ValueError(f"movement benchmark phase {name} allocation availability is invalid")
        for field in map_stats.keys() - {"renderer_allocation_statistics_available"}:
            _integer(map_stats[field], f"phase {name} map {field}")
        total_map_draws = phase["full_map_draws"] + local_minimap["map_draws"]
        if map_stats["map_draws"] != total_map_draws \
                or map_stats["primary_map_draws"] != phase["full_map_draws"] \
                or map_stats["auxiliary_map_draws"] != local_minimap["map_draws"] \
                or map_stats["animation_draws"] != phase["animation_draws"] \
                or map_stats["animation_level_draws"] != (
                    phase["animation_draws"] * map_stats["peak_active_levels"]
                ) \
                or any(map_stats[field] != 0 for field in ("presents", "present_failures", "render_failures", "fault_injections", "fault_detections")) \
                or (not map_stats["renderer_allocation_statistics_available"] and any(
                    map_stats[field] != 0 for field in ("renderer_allocations", "renderer_allocation_bytes")
                )):
            raise ValueError(f"movement benchmark phase {name} map accounting is invalid")

        stages = _mapping(phase["render_stages"], set(RENDER_STAGES), f"phase {name} render stages")
        for stage_name, expected_scope in RENDER_STAGES.items():
            stage = _mapping(stages[stage_name], {"unit", "elapsed", "calls", "scope"}, f"phase {name} render stage {stage_name}")
            if stage["unit"] != "us" or stage["scope"] != expected_scope:
                raise ValueError(f"movement benchmark phase {name} render metadata is invalid")
            _integer(stage["elapsed"], f"phase {name} render elapsed")
            _integer(stage["calls"], f"phase {name} render calls")
        total_render_passes = total_map_draws + phase["animation_draws"]
        if any(
            stages[stage]["calls"] != total_render_passes
            for stage in ("map", "paint", "command_sort", "sprite_effects", "ui")
        ) or any(
            stages[stage]["calls"] != total_map_draws
            for stage in ("map_scratch_clear", "ground_composite")
        ) or any(
            stages[stage]["calls"] != phase["full_map_draws"] + phase["animation_draws"]
            for stage in ("living_occlusion", "hint_replay")
        ) or stages["ground"]["calls"] != map_stats["level_draws"] \
                or stages["objects"]["calls"] != (
                    map_stats["level_draws"] + map_stats["animation_level_draws"]
                ) or any(
            stages[stage]["calls"] > map_stats["level_draws"]
            for stage in ("lighting",)
        ):
            raise ValueError(f"movement benchmark phase {name} map profiler is incomplete")
        _lighting(phase["lighting"], f"phase {name} lighting")
        lighting = phase["lighting"]
        lighting_counters = lighting["counters"]
        if run["mode"] == "smooth":
            primary_level_draws = (
                map_stats["primary_map_draws"] * map_stats["peak_active_levels"]
            )
            if (
                stages["lighting"]["calls"] != primary_level_draws
                or lighting_counters["render_calls"] != primary_level_draws
                or lighting_counters["field_begins"] != primary_level_draws
                or lighting_counters["field_rebuilds"]
                + lighting_counters["field_reuses"]
                != lighting_counters["field_begins"]
            ):
                raise ValueError(
                    f"movement benchmark phase {name} smooth lighting is incomplete"
                )
            if lighting_counters["field_full_rebuilds"] \
                    + lighting_counters["field_partial_rebuilds"] \
                    != lighting_counters["field_rebuilds"]:
                raise ValueError(
                    f"movement benchmark phase {name} rebuild decisions overlap"
                )
            timing_calls = {
                timing_name: timing["calls"]
                for timing_name, timing in lighting["timings"].items()
            }
            if run["fine_timing"] and (
                timing_calls["translation"]
                != lighting_counters["field_translations"]
                + lighting_counters["field_translation_fallback_bounds"]
                + lighting_counters["field_translation_fallback_control"]
                or timing_calls["dirty_clear"]
                != lighting_counters["field_dirty_marks"]
                or timing_calls["rasterization"]
                != lighting_counters["field_rasterized_quads"]
                or lighting_counters["field_rasterized_quads"]
                < lighting_counters["field_rebuilds"]
                or timing_calls["extrapolation"]
                != lighting_counters["field_rebuilds"]
                or timing_calls["tone_map_multiply"]
                != phase["full_map_draws"]
                or timing_calls["sprite_lookup"]
                != lighting_counters["lit_sprite_lookups"]
                or timing_calls["sprite_construction"]
                != lighting_counters["lit_sprite_misses"]
                or timing_calls["sprite_invalidation"]
                < lighting_counters["lit_sprite_clears"]
                + (1 if lighting_counters["lit_sprite_evictions"] else 0)
                + (1 if lighting_counters["lit_sprite_invalidation_source"] else 0)
                or timing_calls["sprite_invalidation"]
                > lighting_counters["lit_sprite_clears"]
                + lighting_counters["lit_sprite_evictions"]
                + lighting_counters["lit_sprite_invalidation_source"]
            ):
                raise ValueError(
                    f"movement benchmark phase {name} lighting timing is incomplete"
                )
            if not run["fine_timing"] and any(
                timing[field] != 0
                for timing in lighting["timings"].values()
                for field in ("calls", "elapsed")
            ):
                raise ValueError(
                    f"movement benchmark phase {name} disabled lighting timing is nonzero"
                )
            if not run["fine_timing"] and (
                any(lighting_work_time[field] != 0 for field in ("p50", "p95", "p99", "max"))
                or any(window["p95_ns"] != 0 for window in lighting_work_time["windows"])
            ):
                raise ValueError(
                    f"movement benchmark phase {name} disabled lighting work timing is nonzero"
                )
            if name == "sustained":
                eligible = lighting_counters["field_begins"]
                _validate_sustained_lit_sprite_retention(lighting_counters)
                eligible_pixels = sum(
                    level["counters"]["field_begins"]
                    * level["width"]
                    * level["height"]
                    for level in lighting["levels"]
                )
                if run["reconstruction"] == "translated" and (
                    lighting_counters["field_translations"] != eligible
                    or lighting_counters["field_partial_rebuilds"]
                    + lighting_counters["field_full_rebuilds"]
                    != eligible
                    or lighting_counters["field_full_rebuilds"]
                    > 4 * lighting["peak"]["allocated_levels"]
                    or lighting_counters["field_translation_fallback_active"] != 0
                    or lighting_counters["field_translation_fallback_bounds"] != 0
                    or lighting_counters["field_translation_fallback_control"] != 0
                    or lighting_counters["field_full_rebuild_active"] != 0
                    or lighting_counters["field_full_rebuild_bounds"] != 0
                    or lighting_counters["field_full_rebuild_control"] != 0
                    or lighting_counters["field_translated_pixels"] == 0
                    or lighting_counters["field_translated_bytes"]
                    != lighting_counters["field_translated_pixels"]
                    * LIGHTING_SAMPLE_BYTES
                    or lighting_counters["field_dirty_pixels"] >= eligible_pixels
                ):
                    raise ValueError(
                        "movement benchmark did not exercise every eligible translation"
                    )
                if run["reconstruction"] == "full" and (
                    lighting_counters["field_translations"] != 0
                    or lighting_counters["field_partial_rebuilds"] != 0
                    or lighting_counters["field_full_rebuilds"] != eligible
                    or lighting_counters["field_translation_fallback_control"] != eligible
                    or lighting_counters["field_full_rebuild_cache"] != 0
                    or lighting_counters["field_full_rebuild_active"] != 0
                    or lighting_counters["field_full_rebuild_bounds"] != 0
                    or lighting_counters["field_full_rebuild_control"] != eligible
                    or lighting_counters["field_translated_pixels"] != 0
                    or lighting_counters["field_translated_bytes"] != 0
                    or lighting_counters["field_dirty_pixels"] != eligible_pixels
                ):
                    raise ValueError(
                        "movement benchmark full-rebuild control is incomplete"
                    )
                if isolated and (
                    lighting_counters["field_scroll_x_pixels"] != eligible * 24
                    or lighting_counters["field_scroll_y_pixels"] != eligible * 12
                ):
                    raise ValueError(
                        "movement benchmark isolated scroll offsets are incomplete"
                    )
        else:
            meaningful_counters = LIGHTING_COUNTER_FIELDS - {
                "lit_sprite_clears",
                "lit_sprite_cleared_entries",
            }
            state_fields = LIGHTING_STATE_FIELDS - {"state_digest"}
            if (
                stages["lighting"]["calls"] != 0
                or stages["lighting"]["elapsed"] != 0
                or any(lighting_counters[field] != 0 for field in meaningful_counters)
                or any(
                    lighting[boundary][field] != 0
                    for boundary in ("start", "end")
                    for field in state_fields
                )
                or any(lighting["peak"][field] != 0 for field in state_fields)
                or any(
                    timing[field] != 0
                    for timing in lighting["timings"].values()
                    for field in ("calls", "elapsed")
                )
                or any(
                    lighting_work_time[field] != 0
                    for field in ("p50", "p95", "p99", "max")
                )
                or any(
                    window["p95_ns"] != 0
                    for window in lighting_work_time["windows"]
                )
            ):
                raise ValueError(
                    f"movement benchmark phase {name} discrete lighting is active"
                )
            if run["reconstruction"] != "translated":
                raise ValueError("movement benchmark discrete control is invalid")

        sprite = _mapping(phase["sprite_cache"], {"available", "limits", "counters", "start", "end", "peak"}, f"phase {name} sprite cache")
        if sprite["available"] is not True:
            raise ValueError(f"movement benchmark phase {name} sprite availability is invalid")
        limits = _mapping(sprite["limits"], {"entries", "estimated_bytes"}, f"phase {name} sprite limits")
        for field, item in limits.items():
            _integer(item, f"phase {name} sprite limit {field}", positive=True)
        sprite_counters = _mapping(sprite["counters"], {"lookups", "hits", "misses", "insertions", "evictions", "rejections", "gc_runs", "gc_removals", "gc_time_ns"}, f"phase {name} sprite counters")
        for field, item in sprite_counters.items():
            _integer(item, f"phase {name} sprite counter {field}")
        if sprite_counters["hits"] + sprite_counters["misses"] != sprite_counters["lookups"]:
            raise ValueError(f"movement benchmark phase {name} sprite accounting is invalid")
        for boundary in ("start", "end", "peak"):
            state = _mapping(sprite[boundary], {"entries", "estimated_bytes"}, f"phase {name} sprite {boundary}")
            for field, item in state.items():
                _integer(item, f"phase {name} sprite {boundary} {field}")
        available_entries = sprite["start"]["entries"] + sprite_counters["insertions"]
        if (
            sprite_counters["misses"] != sprite_counters["insertions"] + sprite_counters["rejections"]
            or sprite_counters["gc_removals"] + sprite_counters["evictions"] > available_entries
            or sprite["end"]["entries"]
            != available_entries - sprite_counters["gc_removals"] - sprite_counters["evictions"]
            or sprite["peak"]["entries"] > limits["entries"]
            or sprite["peak"]["estimated_bytes"] > limits["estimated_bytes"]
        ):
            raise ValueError(f"movement benchmark phase {name} sprite occupancy is invalid")
        if any(
            sprite["peak"][field] < max(sprite["start"][field], sprite["end"][field])
            for field in ("entries", "estimated_bytes")
        ):
            raise ValueError(f"movement benchmark phase {name} sprite peak is invalid")
    return record
