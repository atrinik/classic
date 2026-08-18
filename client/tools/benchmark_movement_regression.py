#!/usr/bin/env python3
"""Run, compare, and report deterministic sustained-map replay results."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import importlib.util
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time
from typing import NoReturn

from movement_benchmark_schema import RENDER_STAGES, validate_record


EVIDENCE_SCHEMA_VERSION = 7
NATIVE_SCHEMA_VERSION = 9
SUSTAINED_P95_LIMIT_NS = 33_300_000
LARGE_SUSTAINED_P95_LIMIT_NS = 125_000_000
DISPLAY_REFERENCE_FPS = 144
DISPLAY_REFERENCE_BUDGET_MS = 1000 / DISPLAY_REFERENCE_FPS
RELATIVE_LIMIT_PERCENT = 10
REQUIRED_PHASES = {"cold": 1, "sustained": 480, "idle": 16, "resumed": 80}
EXPECTED_CHANGED_PACKETS = {"cold": 1, "sustained": 480, "idle": 0, "resumed": 80}
COMPARISON_NOTES = (
    "bootstrap-base-missing-movement-instrumentation",
    "baseline-movement-schema-mismatch",
    "event-has-no-comparison-base",
    "performance-calibration-pending-sibling-integration",
)
COMPARE_FOUNDATION_NOTE = "performance-calibration-pending-sibling-integration"
CROSS_CONTRACT_NOTE = "baseline-movement-schema-mismatch"
INFORMATIONAL_COMPARISON_NOTES = {COMPARE_FOUNDATION_NOTE, CROSS_CONTRACT_NOTE}
STANDARD_DISCRETE_CONTEXT = "standard_discrete"
LARGE_DISCRETE_CONTEXT = "large_discrete"
STANDARD_TRANSLATED_CONTEXT = "standard_lighting_translated"
LARGE_TRANSLATED_CONTEXT = "large_lighting_translated"
STANDARD_FULL_CONTEXT = "standard_full"
LARGE_FULL_CONTEXT = "large_full"
INITIAL_ERROR_REASONS = ("client-validation-ended-before-movement-evidence",)
LIGHTING_FIELDS = {
    "field_rebuilds",
    "field_reuses",
    "field_dirty_pixels",
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
    "lit_sprite_lookups",
    "lit_sprite_hits",
    "lit_sprite_misses",
    "lit_sprite_evictions",
    "entries",
    "bytes",
    "retained_field_bytes",
}
LIGHTING_TIMING_FIELDS = (
    "translation",
    "dirty_clear",
    "rasterization",
    "extrapolation",
    "tone_map_multiply",
    "sprite_lookup",
    "sprite_construction",
    "sprite_invalidation",
)
NATIVE_GUARD_NAMES = (
    "full_redraw_accounting",
    "noop_redraw_avoidance",
    "animation_isolation",
    "idle_wait_recovery",
    "queue_plateau_recovery",
    "lighting_cache_churn",
    "cache_memory_plateau",
    "ordered_final_state",
)
PERFORMANCE_CHECK_NAMES = {
    "candidate_sustained_p95",
    "candidate_large_sustained_p95",
    "candidate_sustained_window_p95",
    "candidate_large_sustained_window_p95",
    "base_candidate_sustained_p95",
}
INFORMATIONAL_OPTIMIZATION_SUFFIXES = (
    "lighting_cache_churn",
)


class BenchmarkError(RuntimeError):
    """A movement benchmark command or its JSON contract failed."""


RecordValidator = Callable[[object], dict[str, object]]


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise BenchmarkError(f"movement benchmark repeated JSON field: {key}")
        result[key] = value
    return result


def _require_exact_fields(record: object, expected: set[str], context: str) -> dict[str, object]:
    if not isinstance(record, dict) or set(record) != expected:
        raise BenchmarkError(f"movement benchmark {context} has an incompatible schema")
    return record


def _require_integer(value: object, context: str, *, positive: bool = False) -> int:
    if type(value) is not int or (positive and value <= 0) or (not positive and value < 0):
        raise BenchmarkError(f"movement benchmark {context} is invalid")
    return value


def load_record_validator(path: Path) -> RecordValidator:
    """Load the immutable base revision's closed record validator."""
    spec = importlib.util.spec_from_file_location("movement_benchmark_base_schema", path)
    if spec is None or spec.loader is None:
        raise BenchmarkError("cannot load the baseline movement schema")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except (ImportError, OSError, SyntaxError) as error:
        raise BenchmarkError("cannot load the baseline movement schema") from error
    validator = getattr(module, "validate_record", None)
    if not callable(validator):
        raise BenchmarkError("baseline movement schema has no record validator")
    return validator


def parse_result(
    output: str, record_validator: RecordValidator = validate_record
) -> dict[str, object]:
    lines = [line for line in output.splitlines() if line.strip()]
    if len(lines) != 1:
        raise BenchmarkError("movement benchmark must emit exactly one JSON record")
    try:
        result = json.loads(lines[0], object_pairs_hook=_reject_duplicate_keys)
    except json.JSONDecodeError as error:
        raise BenchmarkError("movement benchmark emitted invalid JSON") from error
    try:
        return record_validator(result)
    except ValueError as error:
        raise BenchmarkError(str(error)) from error


def run_benchmark(
    client: Path,
    manifest: Path,
    viewport: str,
    expected_revision: str | None = None,
    checkpoint_directory: Path | None = None,
    record_validator: RecordValidator = validate_record,
    reconstruction: str | None = None,
    workload_variant: str | None = None,
) -> dict[str, object]:
    timeout = 900 if viewport == "large" else 180
    environment = os.environ.copy()
    if checkpoint_directory is not None:
        checkpoint_directory.parent.mkdir(parents=True, exist_ok=True)
        unique_directory = Path(
            tempfile.mkdtemp(
                prefix=f"{checkpoint_directory.name}-",
                dir=checkpoint_directory.parent,
            )
        )
        environment["ATRINIK_MOVEMENT_CHECKPOINT_DIR"] = str(unique_directory)
    started = time.monotonic()
    print(f"movement benchmark: starting {client.name} ({viewport})", file=sys.stderr, flush=True)
    command = [str(client), "--player-view-movement-benchmark", str(manifest), viewport]
    if reconstruction is not None:
        command.append(reconstruction)
    if workload_variant is not None:
        if reconstruction is None:
            raise BenchmarkError("movement workload variant requires a reconstruction mode")
        command.append(workload_variant)
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
        env=environment,
    )
    print(
        f"movement benchmark: completed {client.name} ({viewport}) in "
        f"{time.monotonic() - started:.1f}s",
        file=sys.stderr,
        flush=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or "no diagnostic output"
        raise BenchmarkError(
            f"{client.name} {viewport} movement benchmark failed "
            f"({result.returncode}): {detail}"
        )
    record = parse_result(result.stdout, record_validator)
    if expected_revision is not None:
        implementation = record["identity"]["implementation"]
        if implementation["revision"].lower() != expected_revision.lower():
            raise BenchmarkError(
                f"{client.name} benchmark revision does not identify its expected source"
            )
        if not implementation["dirty_known"] or implementation["dirty"] is not False:
            raise BenchmarkError(
                f"{client.name} benchmark does not identify a clean expected source"
            )
    return record


def phase(record: dict[str, object], name: str) -> dict[str, object]:
    return next(item for item in record["phases"] if item["name"] == name)


def _median_integer(values: list[int]) -> int:
    return int(statistics.median(values))


def _phase_median(records: list[dict[str, object]], name: str, field: str) -> int:
    values = []
    for record in records:
        phase_record = phase(record, name)
        if "frame_time" in phase_record and field in (
            "p50_ns",
            "p95_ns",
            "p99_ns",
            "max_ns",
            "first_window_p95_ns",
            "last_window_p95_ns",
        ):
            if field in ("first_window_p95_ns", "last_window_p95_ns"):
                windows = phase_record["frame_time"]["windows"]
                window = windows[0] if field.startswith("first") else windows[-1]
                values.append(window["p95_ns"])
            else:
                values.append(phase_record["frame_time"][field.removesuffix("_ns")])
        else:
            values.append(phase_record[field])
    return _median_integer(values)


def _nested_medians(
    records: list[dict[str, object]], name: str, section: str, fields: set[str]
) -> dict[str, int]:
    result: dict[str, int] = {}
    for field in fields:
        values = []
        for record in records:
            nested = phase(record, name)[section]
            if section == "lighting" and "counters" in nested:
                if field in nested.get("counters", {}):
                    values.append(nested["counters"][field])
                elif field == "entries":
                    values.append(nested["end"]["lit_sprite_entries"])
                elif field == "bytes":
                    values.append(nested["end"]["lit_sprite_bytes"])
                elif field == "retained_field_bytes":
                    values.append(nested["end"][field])
                else:
                    values.append(0)
            elif section == "queue" and field == "depth" and "peak_depth" in nested:
                values.append(nested["peak_depth"])
            elif section == "queue" and field == "bytes" and "peak_bytes" in nested:
                values.append(nested["peak_bytes"])
            else:
                values.append(nested[field])
        result[field] = _median_integer(values)
    return result


def _render_stages_available(records: list[dict[str, object]], name: str) -> bool:
    return all(
        isinstance(phase(record, name).get("render_stages"), dict)
        and set(phase(record, name)["render_stages"]) == set(RENDER_STAGES)
        for record in records
    )


def _render_stage_summary(
    records: list[dict[str, object]], name: str
) -> dict[str, dict[str, object | None]]:
    """Summarize profiler stages as median per-run average invocation times."""
    if not _render_stages_available(records, name):
        return {
            stage_name: {"scope": scope, "calls_per_run": 0, "avg_ms_per_call": None}
            for stage_name, scope in RENDER_STAGES.items()
        }
    result: dict[str, dict[str, object | None]] = {}
    for stage_name, scope in RENDER_STAGES.items():
        stages = [phase(record, name)["render_stages"][stage_name] for record in records]
        calls = _median_integer([stage["calls"] for stage in stages])
        averages = [stage["elapsed"] / stage["calls"] / 1_000 for stage in stages if stage["calls"]]
        result[stage_name] = {
            "scope": scope,
            "calls_per_run": calls,
            "avg_ms_per_call": round(statistics.median(averages), 3) if averages else None,
        }
    return result


def _lighting_timing_summary(
    records: list[dict[str, object]], name: str, level_index: int | None = None
) -> dict[str, dict[str, int | float | None]]:
    """Summarize attributable lighting scopes without discarding raw evidence."""
    result: dict[str, dict[str, int | float | None]] = {}
    for timing_name in LIGHTING_TIMING_FIELDS:
        values = []
        for record in records:
            lighting = phase(record, name)["lighting"]
            source = (
                lighting["timings"]
                if level_index is None
                else lighting["levels"][level_index]["timings"]
            )
            values.append(source[timing_name])
        calls = _median_integer([value["calls"] for value in values])
        elapsed_ns = _median_integer([value["elapsed"] for value in values])
        result[timing_name] = {
            "calls_per_run": calls,
            "elapsed_ms_per_run": round(elapsed_ns / 1_000_000, 3),
            "avg_us_per_call": round(elapsed_ns / calls / 1_000, 3) if calls else None,
        }
    return result


def _lighting_level_summary(
    records: list[dict[str, object]], name: str
) -> list[dict[str, object]]:
    result = []
    for index, depth in enumerate(range(-6, 7)):
        levels = [phase(record, name)["lighting"]["levels"][index] for record in records]
        counters = {
            field: _median_integer([level["counters"][field] for level in levels])
            for field in LIGHTING_FIELDS | {"field_begins"}
            if field not in {"entries", "bytes", "retained_field_bytes"}
        }
        width = _median_integer([level["width"] for level in levels])
        height = _median_integer([level["height"] for level in levels])
        eligible_pixels = counters["field_begins"] * width * height
        result.append(
            {
                "depth": depth,
                "width": width,
                "height": height,
                "counters": counters,
                "dirty_ratio_percent": (
                    round(counters["field_dirty_pixels"] / eligible_pixels * 100, 2)
                    if eligible_pixels
                    else None
                ),
                "timings": _lighting_timing_summary(records, name, index),
            }
        )
    return result


def phase_summary(records: list[dict[str, object]], name: str) -> dict[str, object]:
    """Summarize per-run telemetry while retaining raw records in the evidence."""
    if not records:
        raise BenchmarkError("movement benchmark has no records to summarize")
    representative = phase(records[0], name)
    p50_ns = _phase_median(records, name, "p50_ns")
    p95_ns = _phase_median(records, name, "p95_ns")
    map_p50_ns = _median_integer(
        [phase(record, name)["map_time"]["p50"] for record in records]
    )
    map_p95_ns = _median_integer(
        [phase(record, name)["map_time"]["p95"] for record in records]
    )
    animation_p50_ns = _median_integer(
        [
            phase(record, name).get("animation_time", {"p50": 0})["p50"]
            for record in records
        ]
    )
    animation_p95_ns = _median_integer(
        [
            phase(record, name).get("animation_time", {"p95": 0})["p95"]
            for record in records
        ]
    )
    minimap_p50_ns = _median_integer(
        [phase(record, name)["local_minimap"]["map_time"]["p50"] for record in records]
    )
    minimap_p95_ns = _median_integer(
        [phase(record, name)["local_minimap"]["map_time"]["p95"] for record in records]
    )
    attribution_available = all(
        "lighting_work_time" in phase(record, name)
        and "timings" in phase(record, name)["lighting"]
        for record in records
    )
    lighting_work_p50_ns = (
        _median_integer(
            [phase(record, name)["lighting_work_time"]["p50"] for record in records]
        )
        if attribution_available
        else 0
    )
    lighting_work_p95_ns = (
        _median_integer(
            [phase(record, name)["lighting_work_time"]["p95"] for record in records]
        )
        if attribution_available
        else 0
    )
    lighting = _nested_medians(records, name, "lighting", LIGHTING_FIELDS)
    lookups = lighting["lit_sprite_lookups"]
    hit_rate = None if lookups == 0 else round(lighting["lit_sprite_hits"] / lookups * 100, 1)
    queue_fields = {
        "enqueued",
        "dequeued",
        "budget_yields",
        "recoveries",
        "peak_depth",
        "peak_bytes",
        "oldest_age_us",
        "processing_us",
    }
    queue = {
        field: _median_integer([phase(record, name)["queue"][field] for record in records])
        for field in queue_fields
    }
    drain_p50_ns = _median_integer(
        [phase(record, name)["queue"]["drain_time"]["p50"] for record in records]
    )
    drain_p95_ns = _median_integer(
        [phase(record, name)["queue"]["drain_time"]["p95"] for record in records]
    )
    order_digests_comparable = all(
        phase(record, name)["queue"]["order_digests_comparable"] for record in records
    )
    map_records = [phase(record, name)["map"] for record in records]
    allocation_available = all(
        item["renderer_allocation_statistics_available"] for item in map_records
    )
    draw_reason_fields = {
        "external",
        "packet",
        "scroll",
        "animation",
        "lighting",
        "resize",
        "ui",
    }
    return {
        "runs": len(records),
        "ticks_per_run": representative["samples"],
        "update_cadence_hz": representative["main_loop"]["update_cadence_hz"],
        "render_reference_fps": DISPLAY_REFERENCE_FPS,
        "render_reference_budget_ms": round(DISPLAY_REFERENCE_BUDGET_MS, 3),
        "work_capacity_fps_p50": round(1_000_000_000 / p50_ns, 2),
        "work_capacity_fps_p95": round(1_000_000_000 / p95_ns, 2),
        "update_interval_ms": int(records[0]["tick_ms"]),
        "work_p50_ms": round(p50_ns / 1_000_000, 2),
        "work_p95_ms": round(p95_ns / 1_000_000, 2),
        "map_p50_ms": round(map_p50_ns / 1_000_000, 2),
        "map_p95_ms": round(map_p95_ns / 1_000_000, 2),
        "animation_p50_ms": round(animation_p50_ns / 1_000_000, 2),
        "animation_p95_ms": round(animation_p95_ns / 1_000_000, 2),
        "local_minimap_p50_ms": round(minimap_p50_ns / 1_000_000, 2),
        "local_minimap_p95_ms": round(minimap_p95_ns / 1_000_000, 2),
        "lighting_work_available": attribution_available,
        "lighting_work_p50_ms": round(lighting_work_p50_ns / 1_000_000, 3),
        "lighting_work_p95_ms": round(lighting_work_p95_ns / 1_000_000, 3),
        "work_p99_ms": round(_phase_median(records, name, "p99_ns") / 1_000_000, 2),
        "work_max_ms": round(_phase_median(records, name, "max_ns") / 1_000_000, 2),
        "first_window_p95_ms": round(
            _phase_median(records, name, "first_window_p95_ns") / 1_000_000, 2
        ),
        "last_window_p95_ms": round(
            _phase_median(records, name, "last_window_p95_ns") / 1_000_000, 2
        ),
        "map": {
            **{
                field: _median_integer(
                    [
                        (
                            phase(record, name)[field]
                            if field in phase(record, name)
                            else phase(record, name)["map"].get(field, 0)
                        )
                        for record in records
                    ]
                )
                for field in (
                    "map_packets",
                    "changed_map_packets",
                    "noop_map_packets",
                    "full_map_draws",
                    "animation_draws",
                )
            },
            **{
                field: _median_integer([item.get(field, 0) for item in map_records])
                for field in (
                    "primary_map_draws",
                    "auxiliary_map_draws",
                    "animation_level_draws",
                    "presents",
                )
            },
            "local_minimap_update_interval_ms": representative["local_minimap"][
                "update_interval_ms"
            ],
            "local_minimap_surface_width": representative["local_minimap"][
                "surface_width"
            ],
            "local_minimap_surface_height": representative["local_minimap"][
                "surface_height"
            ],
            "renderer_allocation_statistics_available": allocation_available,
            "renderer_allocations": (
                _median_integer([item["renderer_allocations"] for item in map_records])
                if allocation_available
                else 0
            ),
            "renderer_allocation_bytes": (
                _median_integer([item["renderer_allocation_bytes"] for item in map_records])
                if allocation_available
                else 0
            ),
            "draw_reasons": {
                field: _median_integer(
                    [
                        phase(record, name).get("draw_reasons", {}).get(field, 0)
                        for record in records
                    ]
                )
                for field in draw_reason_fields
            },
        },
        "render_stages": _render_stage_summary(records, name),
        "queue": {
            **{field: queue[field] for field in ("enqueued", "dequeued", "budget_yields", "recoveries", "peak_depth", "peak_bytes")},
            "oldest_age_ms": round(queue["oldest_age_us"] / 1_000, 2),
            "processing_ms": round(queue["processing_us"] / 1_000, 2),
            "drain_p50_ms": round(drain_p50_ns / 1_000_000, 3),
            "drain_p95_ms": round(drain_p95_ns / 1_000_000, 3),
            "service_clock": "simulated",
            "simulated_command_us": 5_000,
            "order_digests_comparable": order_digests_comparable,
        },
        "lighting": {
            **lighting,
            "hit_rate_percent": hit_rate,
            **(
                {
                    "timings": _lighting_timing_summary(records, name),
                    "levels": _lighting_level_summary(records, name),
                }
                if attribution_available
                else {}
            ),
        },
    }


def resource_summary(records: list[dict[str, object]]) -> dict[str, object]:
    """Summarize process and transformed/effects sprite-cache resources per run."""
    if not records:
        raise BenchmarkError("movement benchmark has no records to summarize")
    rss_available = all(record["process_peak_rss_available"] for record in records)
    rss_values = [record["process_peak_rss_bytes"] for record in records]
    final_entries: list[int] = []
    final_bytes: list[int] = []
    peak_entries: list[int] = []
    peak_bytes: list[int] = []
    gc_removals: list[int] = []
    for record in records:
        phases = record["phases"]
        final_cache = phases[-1]["sprite_cache"]["end"]
        final_entries.append(final_cache["entries"])
        final_bytes.append(final_cache["estimated_bytes"])
        peak_entries.append(
            max(item["sprite_cache"]["peak"]["entries"] for item in phases)
        )
        peak_bytes.append(
            max(item["sprite_cache"]["peak"]["estimated_bytes"] for item in phases)
        )
        gc_removals.append(
            sum(item["sprite_cache"]["counters"]["gc_removals"] for item in phases)
        )
    return {
        "runs": len(records),
        "process_peak_rss_available": rss_available,
        "process_peak_rss_median_bytes": _median_integer(rss_values) if rss_available else 0,
        "process_peak_rss_max_bytes": max(rss_values) if rss_available else 0,
        "sprite_cache_end_entries": _median_integer(final_entries),
        "sprite_cache_peak_entries": _median_integer(peak_entries),
        "sprite_cache_end_bytes": _median_integer(final_bytes),
        "sprite_cache_peak_bytes": _median_integer(peak_bytes),
        "sprite_cache_gc_removals": _median_integer(gc_removals),
    }


def _median_phase_value(
    records: list[dict[str, object]], phase_name: str, field: str
) -> int:
    return _phase_median(records, phase_name, field)


def _window_check(records: list[dict[str, object]]) -> dict[str, object]:
    first = _median_phase_value(records, "sustained", "first_window_p95_ns")
    last = _median_phase_value(records, "sustained", "last_window_p95_ns")
    limit = first * (100 + RELATIVE_LIMIT_PERCENT) // 100
    return {
        "first_window_ns": first,
        "last_window_ns": last,
        "limit_ns": limit,
        "passed": last <= limit,
    }


def _checkpoint_check(
    baseline: list[dict[str, object]], candidate: list[dict[str, object]]
) -> dict[str, object]:
    baseline_digests = {record["checkpoint_sha256"] for record in baseline}
    candidate_digests = {record["checkpoint_sha256"] for record in candidate}
    baseline_states = {record.get("final_state_digest") for record in baseline}
    candidate_states = {
        record.get("final_state_digest")
        for record in candidate
    }

    def visual_lifecycle(record: dict[str, object]) -> str:
        return json.dumps(
            [
                {
                    key: checkpoint[key]
                    for key in (
                        "name", "pixels_sha256", "map_x", "map_y",
                        "viewport_width", "viewport_height",
                    )
                }
                for checkpoint in record["checkpoints"]
            ],
            sort_keys=True,
        )

    baseline_lifecycle = {visual_lifecycle(record) for record in baseline}
    candidate_lifecycle = {visual_lifecycle(record) for record in candidate}
    baseline_consistent = len(baseline_digests) <= 1
    candidate_consistent = len(candidate_digests) == 1
    baseline_digest = next(iter(baseline_digests), None)
    candidate_digest = next(iter(candidate_digests), None)
    checkpoint_matched = baseline_consistent and (
        baseline_digest is None or baseline_digest == candidate_digest
    )
    state_consistent = len(baseline_states) <= 1 and len(candidate_states) <= 1
    lifecycle_consistent = len(baseline_lifecycle) <= 1 and len(candidate_lifecycle) == 1
    lifecycle_matched = (
        not baseline_lifecycle
        or not candidate_lifecycle
        or next(iter(baseline_lifecycle)) == next(iter(candidate_lifecycle))
    )
    return {
        "baseline_sha256": baseline_digest,
        "candidate_sha256": candidate_digest,
        "baseline_consistent": baseline_consistent,
        "candidate_consistent": candidate_consistent,
        "base_candidate_match": checkpoint_matched,
        "baseline_state_consistent": len(baseline_states) <= 1,
        "candidate_state_consistent": len(candidate_states) == 1,
        "base_candidate_visual_lifecycle_match": lifecycle_matched,
        "passed": (
            baseline_consistent
            and candidate_consistent
            and checkpoint_matched
            and state_consistent
            and lifecycle_consistent
            and lifecycle_matched
        ),
    }


def _identity_check(
    baseline: list[dict[str, object]], candidate: list[dict[str, object]]
) -> dict[str, object]:
    records = baseline + candidate
    if not records:
        return {"passed": True}
    if len({record.get("schema_version") for record in records}) != 1:
        return {"passed": False}
    if records[0].get("schema_version") != NATIVE_SCHEMA_VERSION:
        return {"passed": True}
    reference = records[0]
    instrumentation = reference["identity"]["instrumentation"]
    run_identity = reference["identity"]["run"]
    fixture = reference["fixture"]
    implementation = {
        key: value
        for key, value in reference["identity"]["implementation"].items()
        if key not in ("revision", "dirty", "dirty_known")
    }
    passed = all(
        record["identity"]["instrumentation"] == instrumentation
        and record["identity"]["run"] == run_identity
        and record["fixture"] == fixture
        and {
            key: value
            for key, value in record["identity"]["implementation"].items()
            if key not in ("revision", "dirty", "dirty_known")
        }
        == implementation
        for record in records
    )
    return {"passed": passed}


def _guard_native_record(record: dict[str, object]) -> dict[str, dict[str, object]]:
    """Return explicit correctness/resource guards for one validated native record."""
    if record.get("schema_version") != NATIVE_SCHEMA_VERSION:
        return {}
    phases = {item["name"]: item for item in record["phases"]}
    sustained = phases["sustained"]
    idle = phases["idle"]
    resumed = phases["resumed"]
    guards: dict[str, dict[str, object]] = {}

    map_failures = sum(
        phase_record["map"][field]
        for phase_record in phases.values()
        for field in ("present_failures", "render_failures", "fault_injections")
    )
    map_draws_match = all(
        phase_record["map"]["map_draws"]
        == phase_record["full_map_draws"] + phase_record["local_minimap"]["map_draws"]
        and phase_record["map"]["primary_map_draws"] == phase_record["full_map_draws"]
        and phase_record["map"]["auxiliary_map_draws"]
        == phase_record["local_minimap"]["map_draws"]
        and phase_record["map"]["animation_draws"] == phase_record["animation_draws"]
        and sum(phase_record["draw_reasons"].values())
        >= phase_record["full_map_draws"] + phase_record["animation_draws"]
        for phase_record in phases.values()
    )
    noop_redraws = idle["full_map_draws"]
    guards["full_redraw_accounting"] = {
        "unexpected_or_failed_draws": map_failures,
        "passed": map_failures == 0 and map_draws_match,
    }
    guards["noop_redraw_avoidance"] = {
        "noop_packet_full_redraws": noop_redraws,
        "passed": noop_redraws == 0 and idle["draw_reasons"]["packet"] == 0,
    }
    animation_isolated = (
        0 <= idle["animation_draws"] <= idle["samples"]
        and idle["map"]["animation_level_draws"]
        == idle["animation_draws"] * idle["map"]["peak_active_levels"]
        and all(
            idle["render_stages"][stage]["calls"] == 0
            for stage in ("map_scratch_clear", "ground", "ground_composite", "lighting")
        )
    )
    guards["animation_isolation"] = {
        "animation_draws": idle["animation_draws"],
        "ground_calls": idle["render_stages"]["ground"]["calls"],
        "lighting_calls": idle["render_stages"]["lighting"]["calls"],
        "passed": animation_isolated,
    }
    idle_wait_recovered = (
        idle["main_loop"]["work_time"]["max"] < idle["main_loop"]["update_interval_ns"]
        and idle["main_loop"]["simulated_wait_time"]["p50"] > 0
    )
    guards["idle_wait_recovery"] = {
        "work_max_ns": idle["main_loop"]["work_time"]["max"],
        "update_interval_ns": idle["main_loop"]["update_interval_ns"],
        "passed": idle_wait_recovered,
    }

    queue_valid = all(
        phase["queue"]["enqueued"] == phase["map_packets"]
        and phase["queue"]["dequeued"] == phase["map_packets"]
        and phase["queue"]["start_depth"] == 0
        and phase["queue"]["end_depth"] == 0
        and phase["queue"]["start_bytes"] == 0
        and phase["queue"]["end_bytes"] == 0
        and phase["queue"]["order_digests_comparable"]
        and phase["queue"]["enqueued_order_digest"]
        == phase["queue"]["dequeued_order_digest"]
        for phase in phases.values()
    )
    recovery_valid = (
        sustained["queue"]["peak_depth"] == 1
        and sustained["queue"]["budget_yields"] == 0
        and resumed["queue"]["peak_depth"] >= 5
        and resumed["queue"]["budget_yields"] > 0
        and resumed["queue"]["recoveries"] == 1
        and resumed["queue"]["oldest_age_us"] > 0
    )
    guards["queue_plateau_recovery"] = {
        "sustained_peak_depth": sustained["queue"]["peak_depth"],
        "resumed_peak_depth": resumed["queue"]["peak_depth"],
        "resumed_oldest_age_us": resumed["queue"]["oldest_age_us"],
        "passed": queue_valid and recovery_valid,
    }

    lighting = sustained["lighting"]
    cache_valid = True
    dirty_marks = 0
    rebuilds = 0
    reuses = 0
    translations = 0
    partial_rebuilds = 0
    dirty_pixels = 0
    if lighting["available"] and record["identity"]["run"]["mode"] == "smooth":
        counters = lighting["counters"]
        dirty_marks = counters["field_dirty_marks"]
        rebuilds = counters["field_rebuilds"]
        reuses = counters["field_reuses"]
        translations = counters["field_translations"]
        partial_rebuilds = counters["field_partial_rebuilds"]
        dirty_pixels = counters["field_dirty_pixels"]
        maximum_updates = sustained["changed_map_packets"] * max(
            1, sustained["map"]["peak_active_levels"]
        )
        viewport = record["identity"]["run"]["viewport"]
        maximum_dirty_pixels = maximum_updates * viewport["width"] * viewport["height"]
        if record["identity"]["run"]["reconstruction"] == "full":
            reconstruction_valid = (
                rebuilds == maximum_updates
                and counters["field_full_rebuilds"] == maximum_updates
                and counters["field_translation_fallback_control"] == maximum_updates
                and translations == partial_rebuilds == 0
                and dirty_pixels > 0
            )
        else:
            reconstruction_valid = (
                rebuilds > 0
                and translations == rebuilds
                and 0 < partial_rebuilds <= rebuilds
                and partial_rebuilds + counters["field_full_rebuilds"] == rebuilds
                and counters["field_full_rebuilds"]
                <= 4 * lighting["peak"]["allocated_levels"]
                and counters["field_translation_fallback_active"] == 0
                and counters["field_translation_fallback_bounds"] == 0
                and counters["field_translation_fallback_control"] == 0
                and 0 < dirty_pixels
                and dirty_pixels * 4 <= maximum_dirty_pixels * 3
            )
        cache_valid = (
            dirty_marks <= maximum_updates
            and rebuilds <= maximum_updates
            and rebuilds + reuses == maximum_updates
            and reconstruction_valid
        )
    guards["lighting_cache_churn"] = {
        "field_dirty_marks": dirty_marks,
        "field_rebuilds": rebuilds,
        "field_reuses": reuses,
        "field_translations": translations,
        "field_partial_rebuilds": partial_rebuilds,
        "field_dirty_pixels": dirty_pixels,
        "passed": cache_valid,
    }
    ordered_phases = [phases[name] for name in REQUIRED_PHASES]
    lighting_continuous = all(
        previous["lighting"]["end"] == current["lighting"]["start"]
        for previous, current in zip(ordered_phases, ordered_phases[1:])
    )
    sprite_continuous = all(
        previous["sprite_cache"]["end"] == current["sprite_cache"]["start"]
        for previous, current in zip(ordered_phases, ordered_phases[1:])
    )
    lighting_plateau = all(
        resumed["lighting"]["peak"][field]
        <= sustained["lighting"]["peak"][field] * (100 + RELATIVE_LIMIT_PERCENT) // 100
        for field in ("lit_sprite_entries", "lit_sprite_bytes", "retained_field_bytes")
    )
    sprite_plateau = all(
        resumed["sprite_cache"]["peak"][field]
        <= sustained["sprite_cache"]["peak"][field] * (100 + RELATIVE_LIMIT_PERCENT) // 100
        for field in ("entries", "estimated_bytes")
    )
    guards["cache_memory_plateau"] = {
        "lighting_peak_bytes": sustained["lighting"]["peak"]["lit_sprite_bytes"],
        "resumed_lighting_peak_bytes": resumed["lighting"]["peak"]["lit_sprite_bytes"],
        "sprite_peak_bytes": sustained["sprite_cache"]["peak"]["estimated_bytes"],
        "resumed_sprite_peak_bytes": resumed["sprite_cache"]["peak"]["estimated_bytes"],
        "passed": lighting_continuous
        and sprite_continuous
        and lighting_plateau
        and sprite_plateau,
    }
    guards["ordered_final_state"] = {
        "final_state_digest": record["final_state_digest"],
        "passed": record["final_state_digest"] == record["same_process_final_state_digest"],
    }
    return guards


def _aggregate_native_guards(records: list[dict[str, object]]) -> dict[str, dict[str, object]]:
    per_record = [_guard_native_record(record) for record in records]
    if not per_record or not per_record[0]:
        return {}
    result: dict[str, dict[str, object]] = {}
    for name in per_record[0]:
        result[name] = {
            "failed_runs": sum(not guards[name]["passed"] for guards in per_record),
            "runs": len(per_record),
            "passed": all(guards[name]["passed"] for guards in per_record),
        }
    return result


def _context_consistency(records: list[dict[str, object]]) -> dict[str, object]:
    if not records or records[0].get("schema_version") != NATIVE_SCHEMA_VERSION:
        return {"fresh_process_runs": len(records), "passed": False}
    checkpoints = {record["checkpoint_sha256"] for record in records}
    final_states = {record["final_state_digest"] for record in records}
    lifecycle = {json.dumps(record["checkpoints"], sort_keys=True) for record in records}
    identities = {
        json.dumps(
            {
                "instrumentation": record["identity"]["instrumentation"],
                "run": record["identity"]["run"],
                "fixture": record["fixture"],
            },
            sort_keys=True,
        )
        for record in records
    }
    return {
        "checkpoints": len(checkpoints),
        "final_states": len(final_states),
        "identities": len(identities),
        "lifecycle_checkpoints": len(lifecycle),
        "fresh_process_runs": len(records),
        "passed": len(records) >= 2
        and len(checkpoints) == len(final_states) == len(identities) == len(lifecycle) == 1,
    }


def _reconstruction_equivalence(
    translated: list[dict[str, object]], full: list[dict[str, object]]
) -> dict[str, object]:
    translated_checkpoints = {record["checkpoint_sha256"] for record in translated}
    full_checkpoints = {record["checkpoint_sha256"] for record in full}
    translated_states = {record["final_state_digest"] for record in translated}
    full_states = {record["final_state_digest"] for record in full}
    translated_offsets = {
        (
            record["phases"][1]["lighting"]["counters"]["field_scroll_x_pixels"],
            record["phases"][1]["lighting"]["counters"]["field_scroll_y_pixels"],
        )
        for record in translated
    }
    full_offsets = {
        (
            record["phases"][1]["lighting"]["counters"]["field_scroll_x_pixels"],
            record["phases"][1]["lighting"]["counters"]["field_scroll_y_pixels"],
        )
        for record in full
    }

    def contract_identity(record: dict[str, object]) -> str:
        run = dict(record["identity"]["run"])
        del run["reconstruction"]
        return json.dumps(
            {
                "instrumentation": record["identity"]["instrumentation"],
                "implementation": record["identity"]["implementation"],
                "run": run,
                "fixture": record["fixture"],
            },
            sort_keys=True,
        )

    translated_identities = {contract_identity(record) for record in translated}
    full_identities = {contract_identity(record) for record in full}
    identities_match = (
        len(translated_identities) == len(full_identities) == 1
        and translated_identities == full_identities
    )
    checkpoints_match = (
        len(translated_checkpoints) == len(full_checkpoints) == 1
        and translated_checkpoints == full_checkpoints
    )
    final_states_match = (
        len(translated_states) == len(full_states) == 1
        and translated_states == full_states
    )
    offsets_match = (
        len(translated_offsets) == len(full_offsets) == 1
        and translated_offsets == full_offsets
    )
    return {
        "translated_runs": len(translated),
        "full_runs": len(full),
        "checkpoint_sha256": next(iter(translated_checkpoints), None),
        "identities_match": identities_match,
        "checkpoints_match": checkpoints_match,
        "final_states_match": final_states_match,
        "scroll_offsets_match": offsets_match,
        "passed": len(translated) >= 1
        and len(full) >= 2
        and checkpoints_match
        and final_states_match
        and offsets_match
        and identities_match,
    }


def _require_native_context(
    records: list[dict[str, object]],
    context: str,
    viewport: str,
    lighting_mode: str,
    *,
    exact_runs: int | None = None,
    reconstruction: str | None = "translated",
    workload_variant: str | None = "production",
) -> None:
    if not records or (exact_runs is not None and len(records) != exact_runs):
        raise BenchmarkError(f"movement regression {context} has an invalid run count")
    for record in records:
        try:
            run = record["identity"]["run"]
            actual_viewport = run["viewport"]["name"]
            actual_mode = run["mode"]
            actual_reconstruction = run.get("reconstruction")
            actual_workload = run.get("workload_variant")
        except (KeyError, TypeError) as error:
            raise BenchmarkError(f"movement regression {context} has an invalid identity") from error
        if actual_viewport != viewport or actual_mode != lighting_mode or (
            reconstruction is not None and actual_reconstruction != reconstruction
        ) or (
            workload_variant is not None and actual_workload != workload_variant
        ):
            raise BenchmarkError(f"movement regression {context} has an invalid identity")


def _validate_enforcement_policy(
    has_baseline: bool, enforce_performance: bool, comparison_note: str | None
) -> None:
    if has_baseline:
        informational = comparison_note in INFORMATIONAL_COMPARISON_NOTES
        if informational != (not enforce_performance) or comparison_note not in {
            None,
            *INFORMATIONAL_COMPARISON_NOTES,
        }:
            raise BenchmarkError("movement comparison performance policy is inconsistent")
        return
    candidate_notes = set(COMPARISON_NOTES) - {COMPARE_FOUNDATION_NOTE}
    if comparison_note == COMPARE_FOUNDATION_NOTE or (
        enforce_performance and comparison_note is not None
    ) or (not enforce_performance and comparison_note not in candidate_notes):
        raise BenchmarkError("candidate-only movement performance policy is inconsistent")


def _build_evidence(
    baseline: list[dict[str, object]],
    candidate: list[dict[str, object]],
    candidate_large: list[dict[str, object]],
    additional_contexts: dict[str, list[dict[str, object]]] | None = None,
    *,
    enforce_performance: bool = True,
    comparison_note: str | None = None,
) -> dict[str, object]:
    _validate_enforcement_policy(bool(baseline), enforce_performance, comparison_note)
    _require_native_context(candidate, "candidate standard", "standard", "smooth")
    if baseline:
        _require_native_context(
            baseline,
            "baseline standard",
            "standard",
            "smooth",
            reconstruction=None,
            workload_variant=None,
        )
    if candidate_large:
        _require_native_context(
            candidate_large, "candidate large", "large", "smooth", exact_runs=2
        )
    contexts = additional_contexts or {}
    expected_contexts = {
        STANDARD_DISCRETE_CONTEXT,
        STANDARD_TRANSLATED_CONTEXT,
        STANDARD_FULL_CONTEXT,
    }
    if candidate_large:
        expected_contexts.add(LARGE_DISCRETE_CONTEXT)
        expected_contexts.add(LARGE_TRANSLATED_CONTEXT)
        expected_contexts.add(LARGE_FULL_CONTEXT)
    if set(contexts) != expected_contexts:
        raise BenchmarkError("movement regression has an incomplete context matrix")
    _require_native_context(
        contexts[STANDARD_DISCRETE_CONTEXT],
        "candidate standard discrete",
        "standard",
        "discrete",
        exact_runs=2,
    )
    _require_native_context(
        contexts[STANDARD_TRANSLATED_CONTEXT],
        "candidate standard isolated translated reconstruction",
        "standard",
        "smooth",
        workload_variant="isolated-lighting",
    )
    _require_native_context(
        contexts[STANDARD_FULL_CONTEXT],
        "candidate standard full reconstruction",
        "standard",
        "smooth",
        reconstruction="full",
        workload_variant="isolated-lighting",
    )
    if candidate_large:
        _require_native_context(
            contexts[LARGE_DISCRETE_CONTEXT],
            "candidate large discrete",
            "large",
            "discrete",
            exact_runs=2,
        )
        _require_native_context(
            contexts[LARGE_TRANSLATED_CONTEXT],
            "candidate large isolated translated reconstruction",
            "large",
            "smooth",
            exact_runs=2,
            workload_variant="isolated-lighting",
        )
        _require_native_context(
            contexts[LARGE_FULL_CONTEXT],
            "candidate large full reconstruction",
            "large",
            "smooth",
            exact_runs=2,
            reconstruction="full",
            workload_variant="isolated-lighting",
        )

    candidate_p95 = _median_phase_value(candidate, "sustained", "p95_ns")
    checks: dict[str, dict[str, object]] = {
        "candidate_sustained_p95": {
            "value_ns": candidate_p95,
            "limit_ns": SUSTAINED_P95_LIMIT_NS,
            "passed": candidate_p95 <= SUSTAINED_P95_LIMIT_NS,
        },
        "candidate_sustained_window_p95": _window_check(candidate),
    }
    if candidate_large:
        large_p95 = _median_phase_value(candidate_large, "sustained", "p95_ns")
        checks.update(
            {
                "candidate_large_sustained_p95": {
                    "value_ns": large_p95,
                    "limit_ns": LARGE_SUSTAINED_P95_LIMIT_NS,
                    "passed": large_p95 <= LARGE_SUSTAINED_P95_LIMIT_NS,
                },
                "candidate_large_sustained_window_p95": _window_check(candidate_large),
            }
        )
    if baseline:
        baseline_p95 = _median_phase_value(baseline, "sustained", "p95_ns")
        relative_limit = baseline_p95 * (100 + RELATIVE_LIMIT_PERCENT) // 100
        checks["base_candidate_sustained_p95"] = {
            "baseline_ns": baseline_p95,
            "candidate_ns": candidate_p95,
            "limit_ns": relative_limit,
            "passed": candidate_p95 <= relative_limit,
        }
    checks["checkpoint"] = _checkpoint_check(baseline, candidate)
    checks["instrumentation_identity"] = _identity_check(baseline, candidate)
    checks.update(_aggregate_native_guards(candidate + candidate_large))
    checks["candidate_standard_determinism"] = _context_consistency(candidate)
    checks["candidate_standard_reconstruction_equivalence"] = _reconstruction_equivalence(
        contexts[STANDARD_TRANSLATED_CONTEXT], contexts[STANDARD_FULL_CONTEXT]
    )
    if candidate_large:
        checks["large_instrumentation_identity"] = _identity_check([], candidate_large)
        checks["candidate_large_determinism"] = _context_consistency(candidate_large)
        checks["candidate_large_reconstruction_equivalence"] = _reconstruction_equivalence(
            contexts[LARGE_TRANSLATED_CONTEXT], contexts[LARGE_FULL_CONTEXT]
        )
    if contexts:
        for context, context_records in contexts.items():
            checks[f"{context}_determinism"] = _context_consistency(context_records)
            for name, check in _aggregate_native_guards(context_records).items():
                checks[f"{context}_{name}"] = check
    cross_contract = comparison_note == CROSS_CONTRACT_NOTE
    for name, check in checks.items():
        policy_follows_performance = name in PERFORMANCE_CHECK_NAMES or name.endswith(
            INFORMATIONAL_OPTIMIZATION_SUFFIXES
        )
        base_dependent = name in {"checkpoint", "instrumentation_identity"}
        check["enforced"] = (
            False
            if cross_contract and base_dependent
            else enforce_performance if policy_follows_performance else True
        )
    failed = any(check["enforced"] and not check["passed"] for check in checks.values())
    return {
        "schema_version": EVIDENCE_SCHEMA_VERSION,
        "status": "failed" if failed else "passed",
        "mode": "comparison" if baseline else "candidate-only",
        "enforced": enforce_performance,
        "comparison_note": comparison_note,
        "failed": failed,
        "samples": {
            "baseline_standard": len(baseline),
            "candidate_standard": len(candidate),
            "candidate_large": len(candidate_large),
            "additional_contexts": {
                context: len(context_records)
                for context, context_records in contexts.items()
            },
        },
        "checks": checks,
        "phases": {
            "baseline_standard": (
                {name: phase_summary(baseline, name) for name in REQUIRED_PHASES}
                if baseline
                else None
            ),
            "candidate_standard": {
                name: phase_summary(candidate, name) for name in REQUIRED_PHASES
            },
            "candidate_large": {
                name: phase_summary(candidate_large, name) for name in REQUIRED_PHASES
            }
            if candidate_large
            else None,
            "additional_contexts": {
                context: {
                    name: phase_summary(context_records, name) for name in REQUIRED_PHASES
                }
                for context, context_records in contexts.items()
            },
        },
        "resources": {
            "candidate_standard": resource_summary(candidate),
            "candidate_large": resource_summary(candidate_large) if candidate_large else None,
            "additional_contexts": {
                context: resource_summary(context_records)
                for context, context_records in contexts.items()
            },
        },
        "records": {
            "baseline_standard": baseline,
            "candidate_standard": candidate,
            "candidate_large": candidate_large,
            "additional_contexts": contexts,
        },
    }


def compare(
    baseline_client: Path,
    baseline_manifest: Path,
    candidate_client: Path,
    candidate_manifest: Path,
    discrete_manifest: Path | None,
    lighting_manifest: Path | None,
    samples: int,
    baseline_revision: str | None = None,
    candidate_revision: str | None = None,
    enforce_performance: bool = True,
    comparison_note: str | None = None,
    checkpoint_root: Path | None = None,
    baseline_validator: RecordValidator = validate_record,
) -> dict[str, object]:
    if discrete_manifest is None or lighting_manifest is None:
        raise BenchmarkError("movement comparison requires discrete and lighting manifests")
    records: dict[str, list[dict[str, object]]] = {
        "baseline": [],
        "candidate": [],
        "candidate_lighting_translated": [],
        "candidate_full": [],
    }
    for sample in range(samples):
        order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
        for implementation in order:
            records[implementation].append(
                run_benchmark(
                    baseline_client if implementation == "baseline" else candidate_client,
                    baseline_manifest if implementation == "baseline" else candidate_manifest,
                    "standard",
                    baseline_revision if implementation == "baseline" else candidate_revision,
                    (
                        checkpoint_root / f"{implementation}-standard-{sample + 1}"
                        if checkpoint_root is not None
                        else None
                    ),
                    baseline_validator if implementation == "baseline" else validate_record,
                    reconstruction=None if implementation == "baseline" else "translated",
                )
            )
        modes = ("translated", "full") if sample % 2 == 0 else ("full", "translated")
        for reconstruction in modes:
            target = (
                records["candidate_lighting_translated"]
                if reconstruction == "translated"
                else records["candidate_full"]
            )
            target.append(
                run_benchmark(
                    candidate_client,
                    lighting_manifest,
                    "standard",
                    candidate_revision,
                    checkpoint_root / f"candidate-standard-lighting-{reconstruction}-{sample + 1}"
                    if checkpoint_root is not None
                    else None,
                    reconstruction=reconstruction,
                    workload_variant="isolated",
                )
            )
    standard_discrete = [
        run_benchmark(
            candidate_client,
            discrete_manifest,
            "standard",
            candidate_revision,
            checkpoint_root / f"candidate-standard-discrete-{sample + 1}"
            if checkpoint_root is not None
            else None,
            reconstruction="translated",
        )
        for sample in range(2)
    ]
    return _build_evidence(
        records["baseline"],
        records["candidate"],
        [],
        {
            STANDARD_DISCRETE_CONTEXT: standard_discrete,
            STANDARD_TRANSLATED_CONTEXT: records["candidate_lighting_translated"],
            STANDARD_FULL_CONTEXT: records["candidate_full"],
        },
        enforce_performance=enforce_performance,
        comparison_note=comparison_note,
    )


def candidate_only(
    candidate_client: Path,
    candidate_manifest: Path,
    candidate_revision: str | None = None,
    discrete_manifest: Path | None = None,
    lighting_manifest: Path | None = None,
    full_matrix: bool = False,
    enforce_performance: bool = True,
    comparison_note: str | None = None,
    checkpoint_root: Path | None = None,
) -> dict[str, object]:
    if discrete_manifest is None or lighting_manifest is None:
        raise BenchmarkError(
            "candidate-only movement validation requires discrete and lighting manifests"
        )
    standard = [
        run_benchmark(
            candidate_client,
            candidate_manifest,
            "standard",
            candidate_revision,
            checkpoint_root / f"candidate-standard-production-{sample + 1}"
            if checkpoint_root is not None
            else None,
            reconstruction="translated",
            workload_variant="production",
        )
        for sample in range(2)
    ]
    standard_lighting_translated: list[dict[str, object]] = []
    standard_full: list[dict[str, object]] = []
    for sample in range(2):
        modes = ("translated", "full") if sample % 2 == 0 else ("full", "translated")
        for reconstruction in modes:
            target = (
                standard_lighting_translated
                if reconstruction == "translated"
                else standard_full
            )
            target.append(
                run_benchmark(
                    candidate_client,
                    lighting_manifest,
                    "standard",
                    candidate_revision,
                    checkpoint_root / f"candidate-standard-lighting-{reconstruction}-{sample + 1}"
                    if checkpoint_root is not None
                    else None,
                    reconstruction=reconstruction,
                    workload_variant="isolated",
                )
            )
    standard_discrete = [
        run_benchmark(
            candidate_client,
            discrete_manifest,
            "standard",
            candidate_revision,
            checkpoint_root / f"candidate-standard-discrete-{sample + 1}"
            if checkpoint_root is not None
            else None,
            reconstruction="translated",
        )
        for sample in range(2)
    ]
    large: list[dict[str, object]] = []
    additional_contexts: dict[str, list[dict[str, object]]] = {
        STANDARD_DISCRETE_CONTEXT: standard_discrete,
        STANDARD_TRANSLATED_CONTEXT: standard_lighting_translated,
        STANDARD_FULL_CONTEXT: standard_full,
    }
    if full_matrix:
        large = [
            run_benchmark(
                candidate_client,
                candidate_manifest,
                "large",
                candidate_revision,
                checkpoint_root / f"candidate-large-production-{sample + 1}"
                if checkpoint_root is not None
                else None,
                reconstruction="translated",
                workload_variant="production",
            )
            for sample in range(2)
        ]
        large_lighting_translated: list[dict[str, object]] = []
        large_full: list[dict[str, object]] = []
        for sample in range(2):
            modes = ("translated", "full") if sample % 2 == 0 else ("full", "translated")
            for reconstruction in modes:
                target = (
                    large_lighting_translated
                    if reconstruction == "translated"
                    else large_full
                )
                target.append(
                    run_benchmark(
                        candidate_client,
                        lighting_manifest,
                        "large",
                        candidate_revision,
                        checkpoint_root / f"candidate-large-lighting-{reconstruction}-{sample + 1}"
                        if checkpoint_root is not None
                        else None,
                        reconstruction=reconstruction,
                        workload_variant="isolated",
                    )
                )
        additional_contexts[LARGE_FULL_CONTEXT] = large_full
        additional_contexts[LARGE_TRANSLATED_CONTEXT] = large_lighting_translated
        additional_contexts[LARGE_DISCRETE_CONTEXT] = [
            run_benchmark(
                candidate_client,
                discrete_manifest,
                "large",
                candidate_revision,
                checkpoint_root / f"candidate-large-discrete-{sample + 1}"
                if checkpoint_root is not None
                else None,
                reconstruction="translated",
            )
            for sample in range(2)
        ]
    return _build_evidence(
        [],
        standard,
        large,
        additional_contexts,
        enforce_performance=enforce_performance,
        comparison_note=comparison_note,
    )


def skipped_evidence(reason: str) -> dict[str, object]:
    return {
        "schema_version": EVIDENCE_SCHEMA_VERSION,
        "status": "skipped",
        "reason": reason,
    }


def error_evidence(error: BaseException) -> dict[str, object]:
    detail = " ".join(str(error).split())[:500]
    return {
        "schema_version": EVIDENCE_SCHEMA_VERSION,
        "status": "error",
        "error": detail or type(error).__name__,
    }


def initial_error_evidence(reason: str) -> dict[str, object]:
    if reason not in INITIAL_ERROR_REASONS:
        raise BenchmarkError("unsupported movement regression initialization reason")
    return {
        "schema_version": EVIDENCE_SCHEMA_VERSION,
        "status": "error",
        "error": reason,
    }


def write_evidence(path: Path, evidence: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")


def _milliseconds(value: int) -> str:
    return f"{value / 1_000_000:.2f} ms"


def _percent_change(before: float, after: float) -> str:
    return "n/a" if before == 0 else f"{(after / before - 1) * 100:+.1f}%"


def _human_bytes(value: int) -> str:
    amount = float(value)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if amount < 1024 or unit == "GiB":
            return f"{amount:.0f} {unit}" if unit == "B" else f"{amount:.1f} {unit}"
        amount /= 1024
    raise AssertionError("unreachable")


def _fallback_comment(message: str) -> str:
    return "\n".join(
        [
            "<!-- atrinik-movement-regression-summary -->",
            "## Movement regression summary",
            "",
            message,
            "",
        ]
    )


def _append_lighting_ab_report(
    lines: list[str],
    contexts: list[tuple[str, dict[str, object], dict[str, object]]],
) -> None:
    """Render the focused same-contract lighting reconstruction comparison."""
    lines.extend(
        [
            "### Attributable lighting movement A/B",
            "",
            "Translated and full reconstruction replay the same sustained movement stream. "
            "`Lighting work` sums the non-overlapping instrumented lighting scopes accumulated "
            "from before queued MAP decode through the primary map draw; it excludes the "
            "separately measured local minimap. The broader "
            "total-work and render-profiler rows remain production-like parent measurements and "
            "must not be added to these operation timings.",
            "",
            "| Viewport | Mode | Lighting work p50/p95 | Total work p95 | "
            "Full/partial/reuse decisions | Dirty pixels (ratio) | Translated pixels/bytes | "
            "Full causes cache/active/bounds/control/other | Scroll offset X/Y |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for viewport, translated, full in contexts:
        for mode, summary in (("translated", translated), ("full control", full)):
            lighting = summary["lighting"]
            active_levels = [level for level in lighting["levels"] if level["width"] > 0]
            dirty_pixels = sum(level["counters"]["field_dirty_pixels"] for level in active_levels)
            eligible_pixels = sum(
                level["counters"]["field_begins"] * level["width"] * level["height"]
                for level in active_levels
            )
            dirty_ratio = dirty_pixels / eligible_pixels * 100 if eligible_pixels else 0.0
            lines.append(
                f"| {viewport} | {mode} | {summary['lighting_work_p50_ms']:.3f}/"
                f"{summary['lighting_work_p95_ms']:.3f} ms | {summary['work_p95_ms']:.2f} ms | "
                f"{lighting['field_full_rebuilds']}/{lighting['field_partial_rebuilds']}/"
                f"{lighting['field_reuses']} | {dirty_pixels:,} ({dirty_ratio:.2f}%) | "
                f"{lighting['field_translated_pixels']:,}/"
                f"{_human_bytes(lighting['field_translated_bytes'])} | "
                f"{lighting['field_full_rebuild_cache']}/"
                f"{lighting['field_full_rebuild_active']}/"
                f"{lighting['field_full_rebuild_bounds']}/"
                f"{lighting['field_full_rebuild_control']}/"
                f"{lighting['field_full_rebuild_other']} | "
                f"{lighting['field_scroll_x_pixels']}/{lighting['field_scroll_y_pixels']} px |"
            )

    standard_translated = contexts[0][1]["lighting"]
    standard_full = contexts[0][2]["lighting"]
    lines.extend(
        [
            "",
            "#### Standard sustained operation attribution",
            "",
            "| Operation | Translated calls / total / average | Full calls / total / average |",
            "| --- | ---: | ---: |",
        ]
    )
    for timing_name in LIGHTING_TIMING_FIELDS:
        translated_timing = standard_translated["timings"][timing_name]
        full_timing = standard_full["timings"][timing_name]

        def timing_text(value: dict[str, object]) -> str:
            average = value["avg_us_per_call"]
            return (
                f"{value['calls_per_run']} / {value['elapsed_ms_per_run']:.3f} ms / "
                f"{average:.3f} µs" if average is not None else "0 / 0.000 ms / n/a"
            )

        lines.append(
            f"| `{timing_name}` | {timing_text(translated_timing)} | "
            f"{timing_text(full_timing)} |"
        )

    lines.extend(
        [
            "",
            "<details>",
            "<summary>Standard sustained per-depth attribution</summary>",
            "",
            "Offsets are cumulative absolute translated pixels. Dirty ratio is dirty pixels "
            "divided by eligible field pixels across field-begin decisions.",
            "",
            "| Depth | Mode | Field | Full/partial/reuse | Dirty ratio | Translated bytes | "
            "Offset X/Y | Full causes cache/active/bounds/control/other |",
            "| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for mode, lighting in (("translated", standard_translated), ("full", standard_full)):
        for level in lighting["levels"]:
            if level["width"] == 0:
                continue
            counters = level["counters"]
            ratio = level["dirty_ratio_percent"]
            lines.append(
                f"| {level['depth']} | {mode} | {level['width']}×{level['height']} | "
                f"{counters['field_full_rebuilds']}/{counters['field_partial_rebuilds']}/"
                f"{counters['field_reuses']} | {ratio:.2f}% | "
                f"{_human_bytes(counters['field_translated_bytes'])} | "
                f"{counters['field_scroll_x_pixels']}/{counters['field_scroll_y_pixels']} px | "
                f"{counters['field_full_rebuild_cache']}/"
                f"{counters['field_full_rebuild_active']}/"
                f"{counters['field_full_rebuild_bounds']}/"
                f"{counters['field_full_rebuild_control']}/"
                f"{counters['field_full_rebuild_other']} |"
            )
    lines.extend(["", "</details>", ""])


def _phase_summary_for_report(summary: object, context: str) -> dict[str, object]:
    expected = {
        "runs",
        "ticks_per_run",
        "update_cadence_hz",
        "render_reference_fps",
        "render_reference_budget_ms",
        "work_capacity_fps_p50",
        "work_capacity_fps_p95",
        "update_interval_ms",
        "work_p50_ms",
        "work_p95_ms",
        "map_p50_ms",
        "map_p95_ms",
        "animation_p50_ms",
        "animation_p95_ms",
        "local_minimap_p50_ms",
        "local_minimap_p95_ms",
        "lighting_work_available",
        "lighting_work_p50_ms",
        "lighting_work_p95_ms",
        "work_p99_ms",
        "work_max_ms",
        "first_window_p95_ms",
        "last_window_p95_ms",
        "map",
        "render_stages",
        "queue",
        "lighting",
    }
    if not isinstance(summary, dict) or set(summary) != expected:
        raise BenchmarkError(f"invalid phase summary: {context}")
    for field in ("runs", "ticks_per_run", "update_interval_ms", "render_reference_fps"):
        if type(summary[field]) is not int or summary[field] <= 0:
            raise BenchmarkError(f"invalid phase summary: {context}")
    for field in (
        "update_cadence_hz",
        "render_reference_budget_ms",
        "work_capacity_fps_p50",
        "work_capacity_fps_p95",
        "work_p50_ms",
        "work_p95_ms",
        "work_p99_ms",
        "work_max_ms",
        "first_window_p95_ms",
        "last_window_p95_ms",
    ):
        if type(summary[field]) not in (int, float) or summary[field] <= 0:
            raise BenchmarkError(f"invalid phase summary: {context}")
    for field in (
        "map_p50_ms",
        "map_p95_ms",
        "animation_p50_ms",
        "animation_p95_ms",
        "local_minimap_p50_ms",
        "local_minimap_p95_ms",
        "lighting_work_p50_ms",
        "lighting_work_p95_ms",
    ):
        if type(summary[field]) not in (int, float) or summary[field] < 0:
            raise BenchmarkError(f"invalid phase summary: {context}")
    if type(summary["lighting_work_available"]) is not bool or (
        not summary["lighting_work_available"]
        and (summary["lighting_work_p50_ms"] != 0 or summary["lighting_work_p95_ms"] != 0)
    ):
        raise BenchmarkError(f"invalid phase summary: {context}")
    if (
        summary["update_cadence_hz"] != 8
        or summary["update_interval_ms"] != 125
        or summary["render_reference_fps"] != DISPLAY_REFERENCE_FPS
        or summary["render_reference_budget_ms"] != round(DISPLAY_REFERENCE_BUDGET_MS, 3)
    ):
        raise BenchmarkError(f"invalid phase summary: {context}")
    stages = _require_exact_fields(
        summary["render_stages"], set(RENDER_STAGES), f"{context} render stages"
    )
    for stage_name, stage in stages.items():
        stage = _require_exact_fields(
            stage, {"scope", "calls_per_run", "avg_ms_per_call"},
            f"{context} render stage {stage_name}",
        )
        if stage["scope"] != RENDER_STAGES[stage_name]:
            raise BenchmarkError(f"invalid render stage scope: {context} {stage_name}")
        _require_integer(stage["calls_per_run"], f"{context} {stage_name} calls")
        if stage["avg_ms_per_call"] is not None and (
            type(stage["avg_ms_per_call"]) not in (int, float)
            or stage["avg_ms_per_call"] < 0
        ):
            raise BenchmarkError(f"invalid render stage timing: {context} {stage_name}")
        if (stage["calls_per_run"] == 0) != (stage["avg_ms_per_call"] is None):
            raise BenchmarkError(f"invalid zero-call render stage: {context} {stage_name}")
    return summary


def _resource_summary_for_report(summary: object, context: str) -> dict[str, object]:
    expected = {
        "runs",
        "process_peak_rss_available",
        "process_peak_rss_median_bytes",
        "process_peak_rss_max_bytes",
        "sprite_cache_end_entries",
        "sprite_cache_peak_entries",
        "sprite_cache_end_bytes",
        "sprite_cache_peak_bytes",
        "sprite_cache_gc_removals",
    }
    resource = _require_exact_fields(summary, expected, f"{context} resources")
    _require_integer(resource["runs"], f"{context} resource runs", positive=True)
    if type(resource["process_peak_rss_available"]) is not bool:
        raise BenchmarkError(f"invalid resource summary: {context}")
    for field in expected - {"runs", "process_peak_rss_available"}:
        _require_integer(resource[field], f"{context} resource {field}")
    if resource["process_peak_rss_available"] != (
        resource["process_peak_rss_median_bytes"] > 0
        and resource["process_peak_rss_max_bytes"] >= resource["process_peak_rss_median_bytes"]
    ):
        raise BenchmarkError(f"invalid resource summary: {context}")
    if resource["sprite_cache_peak_entries"] < resource["sprite_cache_end_entries"] or (
        resource["sprite_cache_peak_bytes"] < resource["sprite_cache_end_bytes"]
    ):
        raise BenchmarkError(f"invalid resource summary: {context}")
    return resource


def _stage_text(value: object | None) -> str:
    return "n/a" if value is None else f"{value:.3f} ms"


def _stage_delta(before: object | None, after: object | None) -> str:
    if before is None or after is None or before == 0:
        return "n/a"
    return f"{(after / before - 1) * 100:+.1f}%"


def _append_render_stage_table(
    lines: list[str],
    title: str,
    candidate: dict[str, object],
    baseline: dict[str, object] | None = None,
) -> None:
    lines.extend(
        [
            title,
            "",
            "Timing is the median of each fresh run's average `elapsed / calls`, "
            "reported as milliseconds per invocation; it is not a p50/p95 distribution. "
            "Parent and child scopes overlap and are not additive.",
            "",
            "| Stage | Scope | Calls/run (base → candidate) | Average ms/call (base → candidate) | Change |",
            "| --- | --- | ---: | ---: | ---: |",
        ]
    )
    candidate_stages = candidate["render_stages"]
    baseline_stages = baseline["render_stages"] if baseline is not None else None
    for stage_name in RENDER_STAGES:
        after = candidate_stages[stage_name]
        before = baseline_stages[stage_name] if baseline_stages is not None else None
        before_calls = "n/a" if before is None else str(before["calls_per_run"])
        before_time = "n/a" if before is None else _stage_text(before["avg_ms_per_call"])
        lines.append(
            f"| `{stage_name}` | `{after['scope']}` | {before_calls} → "
            f"{after['calls_per_run']} | {before_time} → "
            f"{_stage_text(after['avg_ms_per_call'])} | "
            f"{_stage_delta(None if before is None else before['avg_ms_per_call'], after['avg_ms_per_call'])} |"
        )
    lines.append("")


def _expected_evidence_checks(
    *, comparison: bool, large: bool, contexts: set[str]
) -> set[str]:
    expected = {
        "candidate_sustained_p95",
        "candidate_sustained_window_p95",
        "checkpoint",
        "instrumentation_identity",
        "candidate_standard_determinism",
        "candidate_standard_reconstruction_equivalence",
        *NATIVE_GUARD_NAMES,
    }
    if comparison:
        expected.add("base_candidate_sustained_p95")
    if large:
        expected.update(
            {
                "candidate_large_sustained_p95",
                "candidate_large_sustained_window_p95",
                "large_instrumentation_identity",
                "candidate_large_determinism",
                "candidate_large_reconstruction_equivalence",
            }
        )
    for context in contexts:
        expected.add(f"{context}_determinism")
        expected.update(f"{context}_{name}" for name in NATIVE_GUARD_NAMES)
    return expected


def _validate_evidence_check(
    name: str,
    value: object,
    enforce_performance: bool,
    comparison_note: str | None,
) -> None:
    common = {"passed", "enforced"}
    if name in ("candidate_sustained_p95", "candidate_large_sustained_p95"):
        expected = common | {"value_ns", "limit_ns"}
        integer_fields = expected - common
        boolean_fields = common
    elif name == "base_candidate_sustained_p95":
        expected = common | {"baseline_ns", "candidate_ns", "limit_ns"}
        integer_fields = expected - common
        boolean_fields = common
    elif name in (
        "candidate_sustained_window_p95",
        "candidate_large_sustained_window_p95",
    ):
        expected = common | {"first_window_ns", "last_window_ns", "limit_ns"}
        integer_fields = expected - common
        boolean_fields = common
    elif name == "checkpoint":
        expected = common | {
            "baseline_sha256",
            "candidate_sha256",
            "baseline_consistent",
            "candidate_consistent",
            "base_candidate_match",
            "baseline_state_consistent",
            "candidate_state_consistent",
            "base_candidate_visual_lifecycle_match",
        }
        integer_fields: set[str] = set()
        boolean_fields = expected - {"baseline_sha256", "candidate_sha256"}
    elif name in ("instrumentation_identity", "large_instrumentation_identity"):
        expected = common
        integer_fields = set()
        boolean_fields = common
    elif name.endswith("_determinism"):
        expected = common | {
            "checkpoints",
            "final_states",
            "identities",
            "lifecycle_checkpoints",
            "fresh_process_runs",
        }
        integer_fields = expected - common
        boolean_fields = common
    elif name.endswith("_reconstruction_equivalence"):
        expected = common | {
            "translated_runs",
            "full_runs",
            "checkpoint_sha256",
            "identities_match",
            "checkpoints_match",
            "final_states_match",
            "scroll_offsets_match",
        }
        integer_fields = {"translated_runs", "full_runs"}
        boolean_fields = common | {
            "identities_match",
            "checkpoints_match",
            "final_states_match",
            "scroll_offsets_match",
        }
    elif any(name == guard or name.endswith(f"_{guard}") for guard in NATIVE_GUARD_NAMES):
        expected = common | {"failed_runs", "runs"}
        integer_fields = {"failed_runs", "runs"}
        boolean_fields = common
    else:
        raise BenchmarkError(f"unknown movement regression guard: {name}")
    check = _require_exact_fields(value, expected, f"check {name}")
    for field in boolean_fields:
        if type(check[field]) is not bool:
            raise BenchmarkError(f"invalid check: {name}")
    for field in integer_fields:
        _require_integer(check[field], f"check {name} {field}", positive=field != "failed_runs")
    if name == "checkpoint":
        for field in ("baseline_sha256", "candidate_sha256"):
            digest = check[field]
            if digest is not None and (
                not isinstance(digest, str)
                or len(digest) != 64
                or any(character not in "0123456789abcdef" for character in digest)
            ):
                raise BenchmarkError(f"invalid check: {name}")
    if name.endswith("_reconstruction_equivalence"):
        digest = check["checkpoint_sha256"]
        if not isinstance(digest, str) or len(digest) != 64 or any(
            character not in "0123456789abcdef" for character in digest
        ):
            raise BenchmarkError(f"invalid check: {name}")
    if "runs" in check and check["failed_runs"] > check["runs"]:
        raise BenchmarkError(f"invalid check: {name}")
    if "failed_runs" in check and check["passed"] != (check["failed_runs"] == 0):
        raise BenchmarkError(f"inconsistent check result: {name}")
    if name in ("candidate_sustained_p95", "candidate_large_sustained_p95") and (
        check["passed"] != (check["value_ns"] <= check["limit_ns"])
    ):
        raise BenchmarkError(f"inconsistent check result: {name}")
    if name == "base_candidate_sustained_p95" and (
        check["passed"] != (check["candidate_ns"] <= check["limit_ns"])
    ):
        raise BenchmarkError(f"inconsistent check result: {name}")
    if name in (
        "candidate_sustained_window_p95",
        "candidate_large_sustained_window_p95",
    ) and check["passed"] != (check["last_window_ns"] <= check["limit_ns"]):
        raise BenchmarkError(f"inconsistent check result: {name}")
    if name == "checkpoint" and check["passed"] != all(
        check[field] for field in boolean_fields - {"passed", "enforced"}
    ):
        raise BenchmarkError(f"inconsistent check result: {name}")
    if name.endswith("_determinism") and check["passed"] != (
        check["checkpoints"]
        == check["final_states"]
        == check["identities"]
        == check["lifecycle_checkpoints"]
        == 1
        and check["fresh_process_runs"] >= 2
    ):
        raise BenchmarkError(f"inconsistent check result: {name}")
    if name.endswith("_reconstruction_equivalence") and check["passed"] != (
        check["translated_runs"] >= 1
        and check["full_runs"] >= 2
        and check["identities_match"]
        and check["checkpoints_match"]
        and check["final_states_match"]
    ):
        raise BenchmarkError(f"inconsistent check result: {name}")
    follows_performance = name in PERFORMANCE_CHECK_NAMES or name.endswith(
        INFORMATIONAL_OPTIMIZATION_SUFFIXES
    )
    expected_enforced = (
        False
        if comparison_note == CROSS_CONTRACT_NOTE
        and name in {"checkpoint", "instrumentation_identity"}
        else enforce_performance if follows_performance else True
    )
    if check["enforced"] != expected_enforced:
        raise BenchmarkError(f"invalid enforcement policy for check: {name}")


def _render_complete_evidence(
    evidence: dict[str, object],
    client_result: str,
    baseline_validator: RecordValidator = validate_record,
) -> str:
    expected_fields = {
        "schema_version",
        "status",
        "mode",
        "enforced",
        "comparison_note",
        "failed",
        "samples",
        "checks",
        "phases",
        "resources",
        "records",
    }
    if set(evidence) != expected_fields or evidence["schema_version"] != EVIDENCE_SCHEMA_VERSION:
        raise BenchmarkError("movement regression evidence has an invalid schema")
    if evidence["status"] not in ("passed", "failed"):
        raise BenchmarkError("movement regression evidence has an invalid status")
    if type(evidence["failed"]) is not bool or evidence["failed"] != (
        evidence["status"] == "failed"
    ):
        raise BenchmarkError("movement regression evidence has an invalid result")
    if evidence["mode"] not in ("comparison", "candidate-only"):
        raise BenchmarkError("movement regression evidence has an invalid mode")
    if type(evidence["enforced"]) is not bool:
        raise BenchmarkError("movement regression evidence has an invalid enforcement mode")
    if evidence["comparison_note"] is not None and evidence["comparison_note"] not in COMPARISON_NOTES:
        raise BenchmarkError("movement regression evidence has an invalid comparison note")
    _validate_enforcement_policy(
        evidence["mode"] == "comparison", evidence["enforced"], evidence["comparison_note"]
    )
    samples = _require_exact_fields(
        evidence["samples"],
        {"baseline_standard", "candidate_standard", "candidate_large", "additional_contexts"},
        "evidence samples",
    )
    for field in ("baseline_standard", "candidate_standard", "candidate_large"):
        value = samples[field]
        _require_integer(value, f"evidence {field}")
    if samples["candidate_large"] not in (0, 2):
        raise BenchmarkError("movement regression evidence has invalid samples")
    if evidence["mode"] == "comparison":
        if (
            samples["baseline_standard"] != samples["candidate_standard"]
            or samples["candidate_standard"] not in range(3, 10, 2)
        ):
            raise BenchmarkError("movement regression evidence has invalid comparison samples")
    elif samples["baseline_standard"] != 0 or samples["candidate_standard"] != 2:
        raise BenchmarkError("movement regression evidence has invalid candidate-only samples")
    expected_contexts = {
        STANDARD_DISCRETE_CONTEXT,
        STANDARD_TRANSLATED_CONTEXT,
        STANDARD_FULL_CONTEXT,
    }
    if samples["candidate_large"] == 2:
        expected_contexts.add(LARGE_DISCRETE_CONTEXT)
        expected_contexts.add(LARGE_TRANSLATED_CONTEXT)
        expected_contexts.add(LARGE_FULL_CONTEXT)
    context_samples = _require_exact_fields(
        samples["additional_contexts"], expected_contexts, "evidence context samples"
    )
    if context_samples[STANDARD_DISCRETE_CONTEXT] != 2 \
            or context_samples[STANDARD_TRANSLATED_CONTEXT] != samples["candidate_standard"] \
            or context_samples[STANDARD_FULL_CONTEXT] != samples["candidate_standard"] \
            or (samples["candidate_large"] == 2 and (
                context_samples[LARGE_DISCRETE_CONTEXT] != 2
                or context_samples[LARGE_TRANSLATED_CONTEXT] != 2
                or context_samples[LARGE_FULL_CONTEXT] != 2
            )):
        raise BenchmarkError("movement regression evidence has invalid context samples")

    phases = _require_exact_fields(
        evidence["phases"],
        {"baseline_standard", "candidate_standard", "candidate_large", "additional_contexts"},
        "evidence phases",
    )
    candidate_sets: list[tuple[str, object, int]] = [
        ("Standard smooth", phases["candidate_standard"], samples["candidate_standard"])
    ]
    if samples["candidate_large"] == 2:
        candidate_sets.append(
            ("Large smooth", phases["candidate_large"], samples["candidate_large"])
        )
    elif phases["candidate_large"] is not None:
        raise BenchmarkError("movement regression large phases do not match its samples")
    context_phases = _require_exact_fields(
        phases["additional_contexts"], expected_contexts, "evidence context phases"
    )
    candidate_sets.append(
        (
            "Standard isolated smooth translated reconstruction",
            context_phases[STANDARD_TRANSLATED_CONTEXT],
            context_samples[STANDARD_TRANSLATED_CONTEXT],
        )
    )
    candidate_sets.append(
        (
            "Standard discrete",
            context_phases[STANDARD_DISCRETE_CONTEXT],
            context_samples[STANDARD_DISCRETE_CONTEXT],
        )
    )
    candidate_sets.append(
        (
            "Standard smooth full reconstruction",
            context_phases[STANDARD_FULL_CONTEXT],
            context_samples[STANDARD_FULL_CONTEXT],
        )
    )
    if samples["candidate_large"] == 2:
        candidate_sets.append(
            (
                "Large isolated smooth translated reconstruction",
                context_phases[LARGE_TRANSLATED_CONTEXT],
                context_samples[LARGE_TRANSLATED_CONTEXT],
            )
        )
        candidate_sets.append(
            (
                "Large discrete",
                context_phases[LARGE_DISCRETE_CONTEXT],
                context_samples[LARGE_DISCRETE_CONTEXT],
            )
        )
        candidate_sets.append(
            (
                "Large smooth full reconstruction",
                context_phases[LARGE_FULL_CONTEXT],
                context_samples[LARGE_FULL_CONTEXT],
            )
        )
    records = evidence["records"]
    records = _require_exact_fields(
        records,
        {"baseline_standard", "candidate_standard", "candidate_large", "additional_contexts"},
        "raw records",
    )
    for field in ("baseline_standard", "candidate_standard", "candidate_large"):
        if not isinstance(records[field], list) or len(records[field]) != samples[field]:
            raise BenchmarkError("movement regression raw records do not match its samples")
    additional_contexts = records.get("additional_contexts", {})
    additional_contexts = _require_exact_fields(
        additional_contexts, expected_contexts, "raw context records"
    )
    for context, context_records in additional_contexts.items():
        if not isinstance(context_records, list) or len(context_records) != context_samples[context]:
            raise BenchmarkError("movement regression context records do not match its samples")
    for record in records["baseline_standard"]:
        baseline_validator(record)
    for field in ("candidate_standard", "candidate_large"):
        for record in records[field]:
            validate_record(record)
    for context_records in additional_contexts.values():
        for record in context_records:
            validate_record(record)
    rebuilt = _build_evidence(
        records["baseline_standard"],
        records["candidate_standard"],
        records["candidate_large"],
        additional_contexts,
        enforce_performance=evidence["enforced"],
        comparison_note=evidence["comparison_note"],
    )
    for field in (
        "schema_version",
        "status",
        "mode",
        "enforced",
        "comparison_note",
        "failed",
        "samples",
        "checks",
        "phases",
        "resources",
    ):
        if evidence[field] != rebuilt[field]:
            raise BenchmarkError(
                f"movement regression evidence {field} does not match its raw records"
            )
    resources = _require_exact_fields(
        evidence["resources"],
        {"candidate_standard", "candidate_large", "additional_contexts"},
        "evidence resources",
    )
    candidate_resources: list[tuple[str, object, int]] = [
        ("Standard smooth", resources["candidate_standard"], samples["candidate_standard"])
    ]
    if samples["candidate_large"] == 2:
        candidate_resources.append(
            ("Large smooth", resources["candidate_large"], samples["candidate_large"])
        )
    elif resources["candidate_large"] is not None:
        raise BenchmarkError("movement regression large resources do not match its samples")
    context_resources = _require_exact_fields(
        resources["additional_contexts"], expected_contexts, "evidence context resources"
    )
    candidate_resources.append(
        (
            "Standard isolated smooth translated reconstruction",
            context_resources[STANDARD_TRANSLATED_CONTEXT],
            context_samples[STANDARD_TRANSLATED_CONTEXT],
        )
    )
    candidate_resources.append(
        (
            "Standard discrete",
            context_resources[STANDARD_DISCRETE_CONTEXT],
            context_samples[STANDARD_DISCRETE_CONTEXT],
        )
    )
    candidate_resources.append(
        (
            "Standard smooth full reconstruction",
            context_resources[STANDARD_FULL_CONTEXT],
            context_samples[STANDARD_FULL_CONTEXT],
        )
    )
    if samples["candidate_large"] == 2:
        candidate_resources.append(
            (
                "Large isolated smooth translated reconstruction",
                context_resources[LARGE_TRANSLATED_CONTEXT],
                context_samples[LARGE_TRANSLATED_CONTEXT],
            )
        )
        candidate_resources.append(
            (
                "Large discrete",
                context_resources[LARGE_DISCRETE_CONTEXT],
                context_samples[LARGE_DISCRETE_CONTEXT],
            )
        )
        candidate_resources.append(
            (
                "Large smooth full reconstruction",
                context_resources[LARGE_FULL_CONTEXT],
                context_samples[LARGE_FULL_CONTEXT],
            )
        )
    checks = _require_exact_fields(
        evidence["checks"],
        _expected_evidence_checks(
            comparison=evidence["mode"] == "comparison",
            large=samples["candidate_large"] == 2,
            contexts=expected_contexts,
        ),
        "checks",
    )
    informational_failures = 0
    enforced_failures = 0
    for name, check in checks.items():
        _validate_evidence_check(
            name, check, evidence["enforced"], evidence["comparison_note"]
        )
        if not check["passed"]:
            if check["enforced"]:
                enforced_failures += 1
            else:
                informational_failures += 1
    if evidence["failed"] != (enforced_failures > 0):
        raise BenchmarkError("movement regression evidence result is inconsistent")
    if enforced_failures:
        headline = f"❌ {enforced_failures} enforced movement regression check(s) failed."
    elif informational_failures:
        headline = (
            "✅ All enforced checks passed; "
            f"{informational_failures} performance/optimization finding(s) are informational."
        )
    else:
        headline = "✅ All movement regression checks passed."
    lines = [
        "<!-- atrinik-movement-regression-summary -->",
        "## Movement regression summary",
        "",
        headline,
        "",
    ]
    if client_result != "success":
        lines.extend(
            [
                f"⚠️ **Overall Client validation status: `{client_result}`.** Movement evidence "
                "below is preserved independently; inspect the remaining client checks.",
                "",
            ]
        )
    if evidence["mode"] == "comparison":
        lines.append(
            f"Release measurements: `{samples['baseline_standard']}` base/"
            f"`{samples['candidate_standard']}` candidate standard runs."
        )
        lines.append(
            "Base and candidate runs were alternated on the same runner; positive timing deltas "
            "mean the candidate was slower."
        )
        if evidence["comparison_note"] == CROSS_CONTRACT_NOTE:
            lines.append(
                "This is a cross-contract comparison: timing deltas and base-dependent checks "
                "are informational, while candidate correctness and resource guards remain "
                "enforced."
            )
    else:
        lines.append(
            f"Candidate-only validation measured `{samples['candidate_standard']}` candidate "
            "standard runs."
        )
        lines.append(
            "This bootstrap result establishes the hosted-runner baseline. No before/after delta "
            "is claimed because the selected base predates the compatible movement instrumentation; "
            "later schema-compatible PRs alternate base and candidate runs on the same runner."
        )
    lines.append(
        "Both bounded paths include two fresh standard-discrete runs for correctness, "
        "determinism, and telemetry."
    )
    if samples["candidate_large"] == 2:
        lines.append("The requested full matrix also measured two fresh large-viewport runs.")
    else:
        lines.append(
            "The multi-minute large viewport is deferred to the explicit full matrix so the "
            "PR subset remains bounded; no large-viewport result is claimed here."
        )
    if evidence["comparison_note"] is not None:
        note = evidence["comparison_note"].replace("-", " ")
        lines.extend(["", f"Comparison context: **{note}**."])
    lines.extend(
        [
            "",
            "The replay injects MAP state at 8 Hz (one 125 ms simulation tick); that update "
            "cadence is not the client display frame rate. Measured replay-work capacity is the "
            "unslept throughput implied by decode, full-map or animation-only rendering, "
            "bounded local minimap map-core draws when due, and maintenance work. "
            "The local minimap uses its real 250 ms refresh cadence and retains the 1700×1200 "
            "surface required by the production zoom/crop path; "
            "widget masking/zooming and "
            "the otherwise small UI/widget work are outside this map-focused measurement. The "
            "144 FPS reference (6.944 ms/frame) is informational only and is not an enforced "
            "server threshold.",
            "",
        ]
    )
    candidate_standard = phases["candidate_standard"]
    if not isinstance(candidate_standard, dict):
        raise BenchmarkError("candidate standard phase summaries are incomplete")
    candidate_sustained = _phase_summary_for_report(
        candidate_standard["sustained"], "candidate Standard smooth sustained"
    )
    lighting_ab_contexts = [
        (
            "Standard",
            _phase_summary_for_report(
                context_phases[STANDARD_TRANSLATED_CONTEXT]["sustained"],
                "candidate Standard isolated translated reconstruction sustained",
            ),
            _phase_summary_for_report(
                context_phases[STANDARD_FULL_CONTEXT]["sustained"],
                "candidate Standard full reconstruction sustained",
            ),
        )
    ]
    if samples["candidate_large"] == 2:
        lighting_ab_contexts.append(
            (
                "Large",
                _phase_summary_for_report(
                    context_phases[LARGE_TRANSLATED_CONTEXT]["sustained"],
                    "candidate Large isolated translated reconstruction sustained",
                ),
                _phase_summary_for_report(
                    context_phases[LARGE_FULL_CONTEXT]["sustained"],
                    "candidate Large full reconstruction sustained",
                ),
            )
        )
    _append_lighting_ab_report(lines, lighting_ab_contexts)
    lines.extend(
        [
            "### Before/after summary (standard smooth sustained)",
            "",
            "| Metric | Before (base) | After (candidate) | Change |",
            "| --- | ---: | ---: | ---: |",
        ]
    )
    if evidence["mode"] == "comparison":
        baseline_phases = phases["baseline_standard"]
        candidate_phases = phases["candidate_standard"]
        if not isinstance(baseline_phases, dict) or not isinstance(candidate_phases, dict):
            raise BenchmarkError("movement comparison phase summaries are incomplete")
        baseline_sustained = _phase_summary_for_report(
            baseline_phases["sustained"], "baseline Standard smooth sustained"
        )
        map_metric_label = (
            "Map render path p95 (contract-specific)"
            if evidence["comparison_note"] == CROSS_CONTRACT_NOTE
            else "Full map p95"
        )
        summary_metrics = (
            ("Total map-focused update work p95", "work_p95_ms", "ms"),
            (map_metric_label, "map_p95_ms", "ms"),
            ("Local minimap map-core p95", "local_minimap_p95_ms", "ms"),
            ("Slow-tail work capacity", "work_capacity_fps_p95", "FPS"),
        )
        for label, field, unit in summary_metrics:
            before = float(baseline_sustained[field])
            after = float(candidate_sustained[field])
            lines.append(
                f"| {label} | {before:.2f} {unit} | {after:.2f} {unit} | "
                f"{_percent_change(before, after)} |"
            )
        lines.extend(
            [
                "",
                "Positive timing changes are slower; positive capacity changes are faster.",
                "",
            ]
        )
        if evidence["comparison_note"] == CROSS_CONTRACT_NOTE:
            lines.extend(
                [
                    "The total-work and capacity rows remain end-to-end measurements. "
                    "Map-component buckets may have changed meaning with the benchmark contract "
                    "and are shown only as diagnostic context.",
                    "",
                ]
            )
        lines.extend(
            [
                "### Base → candidate change (standard smooth)",
                "",
                "| Phase | Total update work p95 | Change | "
                f"{map_metric_label} | Change | "
                "Local minimap map-core p95 | Change | Work-capacity FPS (slow tail) |",
                "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for name in REQUIRED_PHASES:
            baseline_summary = _phase_summary_for_report(
                baseline_phases[name], f"baseline Standard smooth {name}"
            )
            candidate_summary = _phase_summary_for_report(
                candidate_phases[name], f"candidate Standard smooth {name}"
            )
            lines.append(
                f"| `{name}` | {baseline_summary['work_p95_ms']:.2f} → "
                f"{candidate_summary['work_p95_ms']:.2f} ms | "
                f"{_percent_change(baseline_summary['work_p95_ms'], candidate_summary['work_p95_ms'])} | "
                f"{baseline_summary['map_p95_ms']:.2f} → "
                f"{candidate_summary['map_p95_ms']:.2f} ms | "
                f"{_percent_change(baseline_summary['map_p95_ms'], candidate_summary['map_p95_ms'])} | "
                f"{baseline_summary['local_minimap_p95_ms']:.2f} → "
                f"{candidate_summary['local_minimap_p95_ms']:.2f} ms | "
                f"{_percent_change(baseline_summary['local_minimap_p95_ms'], candidate_summary['local_minimap_p95_ms'])} | "
                f"{baseline_summary['work_capacity_fps_p95']:.2f} → "
                f"{candidate_summary['work_capacity_fps_p95']:.2f} |"
            )
        lines.append("")
    else:
        unavailable = "Unavailable (base predates compatible instrumentation)"
        lines.extend(
            [
                f"| Total map-focused update work p95 | {unavailable} | "
                f"{candidate_sustained['work_p95_ms']:.2f} ms | Not computed |",
                f"| Full map p95 | {unavailable} | "
                f"{candidate_sustained['map_p95_ms']:.2f} ms | Not computed |",
                f"| Local minimap map-core p95 | {unavailable} | "
                f"{candidate_sustained['local_minimap_p95_ms']:.2f} ms | Not computed |",
                f"| Slow-tail work capacity | {unavailable} | "
                f"{candidate_sustained['work_capacity_fps_p95']:.2f} FPS | Not computed |",
                "",
                "This candidate result becomes the initial hosted-runner baseline; a numeric "
                "delta requires a base with the same benchmark contract.",
                "",
            ]
        )
    baseline_sustained_stages = None
    if evidence["mode"] == "comparison" and _render_stages_available(
        records["baseline_standard"], "sustained"
    ):
        baseline_sustained_stages = baseline_phases["sustained"]
    _append_render_stage_table(
        lines,
        "### Render-profiler stages (standard smooth sustained)",
        candidate_sustained,
        baseline_sustained_stages,
    )
    for phase_name in REQUIRED_PHASES:
        if phase_name == "sustained":
            continue
        baseline_phase = None
        if evidence["mode"] == "comparison" and _render_stages_available(
            records["baseline_standard"], phase_name
        ):
            baseline_phase = baseline_phases[phase_name]
        _append_render_stage_table(
            lines,
            f"### Render-profiler stages (standard smooth {phase_name})",
            candidate_standard[phase_name],
            baseline_phase,
        )
    lines.extend(
        [
            "Other viewport, discrete-control, and additional-context render-stage detail "
            "remains available in the uploaded JSON artifact so the GitHub comment stays "
            "within its publication limit. Those candidate-only contexts do not collect a "
            "baseline, and live profiler buckets unavailable to the offline replay are never "
            "fabricated.",
            "",
        ]
    )
    lines.extend(
        [
            "### Candidate hosted baseline timing",
            "",
            "| Context | Phase | Ticks/run | Runs | MAP update rate | "
            "Measured replay-work capacity FPS (p50/slow-tail) | Display reference | "
            "Total work p50/p95 | Full map p50/p95 | Animation pass p50/p95 | "
            "Local minimap map-core p50/p95 | "
            "Work p95 / 144 FPS budget | "
            "First → last work-window p95 |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    summaries: list[tuple[str, str, dict[str, object]]] = []
    for viewport, phase_set, expected_runs in candidate_sets:
        if not isinstance(phase_set, dict) or set(phase_set) != set(REQUIRED_PHASES):
            raise BenchmarkError(f"invalid candidate phase set: {viewport}")
        for name in REQUIRED_PHASES:
            summary = _phase_summary_for_report(phase_set[name], f"{viewport} {name}")
            if summary["runs"] != expected_runs:
                raise BenchmarkError(f"invalid candidate phase run count: {viewport}")
            summaries.append((viewport, name, summary))
            budget_percent = (
                summary["work_p95_ms"] / summary["render_reference_budget_ms"] * 100
            )
            lines.append(
                f"| {viewport} | `{name}` | {summary['ticks_per_run']} | {summary['runs']} | "
                f"{summary['update_cadence_hz']:.0f} Hz | "
                f"{summary['work_capacity_fps_p50']:.2f}/"
                f"{summary['work_capacity_fps_p95']:.2f} | "
                f"{summary['render_reference_fps']} FPS "
                f"({summary['render_reference_budget_ms']:.3f} ms) | "
                f"{summary['work_p50_ms']:.2f}/{summary['work_p95_ms']:.2f} ms | "
                f"{summary['map_p50_ms']:.2f}/{summary['map_p95_ms']:.2f} ms | "
                f"{summary['animation_p50_ms']:.2f}/"
                f"{summary['animation_p95_ms']:.2f} ms | "
                f"{summary['local_minimap_p50_ms']:.2f}/"
                f"{summary['local_minimap_p95_ms']:.2f} ms | "
                f"{budget_percent:.1f}% | "
                f"{summary['first_window_p95_ms']:.2f} → "
                f"{summary['last_window_p95_ms']:.2f} ms |"
            )

    lines.extend(
        [
            "",
            "### Candidate MAP and queue activity",
            "",
            "Values are medians per run when a viewport has multiple runs.",
            "Reason columns are packet, scroll, animation, lighting, resize, UI, and external.",
            "",
            "| Context | Phase | MAP packets (changed/no-op) | Full-map calls | "
            "Animation-only calls | Reasons P/S/A/L/R/U/E | Local-minimap calls | Presents | "
            "Queue peak | Peak bytes | Budget yields/recoveries | Oldest queued | "
            "Simulated queue service | Actual drain p50/p95 | Queue order proof | "
            "Renderer allocations |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
            "---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for viewport, name, summary in summaries:
        map_stats = _require_exact_fields(
            summary["map"],
            {
                "map_packets", "changed_map_packets", "noop_map_packets", "full_map_draws",
                "animation_draws", "primary_map_draws", "auxiliary_map_draws",
                "animation_level_draws", "presents", "draw_reasons",
                "local_minimap_update_interval_ms", "local_minimap_surface_width",
                "local_minimap_surface_height",
                "renderer_allocation_statistics_available", "renderer_allocations",
                "renderer_allocation_bytes",
            },
            f"{viewport} {name} map summary",
        )
        queue = _require_exact_fields(
            summary["queue"],
            {
                "enqueued", "dequeued", "budget_yields", "recoveries", "peak_depth",
                "peak_bytes", "oldest_age_ms", "processing_ms", "drain_p50_ms",
                "drain_p95_ms", "service_clock", "simulated_command_us",
                "order_digests_comparable",
            },
            f"{viewport} {name} queue summary",
        )
        if (
            type(queue["order_digests_comparable"]) is not bool
            or queue["service_clock"] != "simulated"
            or queue["simulated_command_us"] != 5_000
        ):
            raise BenchmarkError(f"invalid queue summary: {viewport} {name}")
        allocations = (
            f"{map_stats['renderer_allocations']} / "
            f"{_human_bytes(map_stats['renderer_allocation_bytes'])}"
            if map_stats["renderer_allocation_statistics_available"]
            else "unavailable"
        )
        reasons = _require_exact_fields(
            map_stats["draw_reasons"],
            {"packet", "scroll", "animation", "lighting", "resize", "ui", "external"},
            f"{viewport} {name} redraw reasons",
        )
        lines.append(
            f"| {viewport} | `{name}` | {map_stats['map_packets']} "
            f"({map_stats['changed_map_packets']}/{map_stats['noop_map_packets']}) | "
            f"{map_stats['primary_map_draws']} | {map_stats['animation_draws']} | "
            f"{reasons['packet']}/{reasons['scroll']}/{reasons['animation']}/"
            f"{reasons['lighting']}/{reasons['resize']}/{reasons['ui']}/"
            f"{reasons['external']} | {map_stats['auxiliary_map_draws']} | "
            f"{map_stats['presents']} | {queue['peak_depth']} | "
            f"{_human_bytes(queue['peak_bytes'])} | "
            f"{queue['budget_yields']}/{queue['recoveries']} | "
            f"{queue['oldest_age_ms']:.2f} ms | {queue['processing_ms']:.2f} ms | "
            f"{queue['drain_p50_ms']:.3f}/{queue['drain_p95_ms']:.3f} ms | "
            f"{'verified' if queue['order_digests_comparable'] else 'not comparable'} | "
            f"{allocations} |"
        )

    lines.extend(
        [
            "",
            "### Candidate lighting activity",
            "",
            "| Context | Phase | Fields rebuilt/reused | Translated/partial | Dirty pixels | "
            "Sprite hits/lookups (rate) | Misses | Evictions | Entries | Sprite memory | "
            "Field memory |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for viewport, name, summary in summaries:
        lighting = _require_exact_fields(
            summary["lighting"], LIGHTING_FIELDS | {"hit_rate_percent", "timings", "levels"},
            f"{viewport} {name} lighting summary",
        )
        hit_rate = lighting["hit_rate_percent"]
        hit_rate_text = "n/a" if hit_rate is None else f"{hit_rate:.1f}%"
        lines.append(
            f"| {viewport} | `{name}` | {lighting['field_rebuilds']}/"
            f"{lighting['field_reuses']} | {lighting['field_translations']}/"
            f"{lighting['field_partial_rebuilds']} | {lighting['field_dirty_pixels']:,} | "
            f"{lighting['lit_sprite_hits']}/"
            f"{lighting['lit_sprite_lookups']} ({hit_rate_text}) | "
            f"{lighting['lit_sprite_misses']} | {lighting['lit_sprite_evictions']} | "
            f"{lighting['entries']} | {_human_bytes(lighting['bytes'])} | "
            f"{_human_bytes(lighting['retained_field_bytes'])} |"
        )

    lines.extend(
        [
            "",
            "### Candidate process and transformed/effects sprite resources",
            "",
            "Process RSS is shown as median/max across fresh runs. Sprite-cache values are "
            "median end/peak per run; GC removals are the median total across phases.",
            "",
            "| Context | Runs | Process peak RSS median/max | Sprite entries end/peak | "
            "Sprite memory end/peak | GC removals |",
            "| --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for context, resource_value, expected_runs in candidate_resources:
        resource = _resource_summary_for_report(resource_value, context)
        if resource["runs"] != expected_runs:
            raise BenchmarkError(f"invalid resource run count: {context}")
        rss = (
            f"{_human_bytes(resource['process_peak_rss_median_bytes'])}/"
            f"{_human_bytes(resource['process_peak_rss_max_bytes'])}"
            if resource["process_peak_rss_available"]
            else "unavailable"
        )
        lines.append(
            f"| {context} | {resource['runs']} | {rss} | "
            f"{resource['sprite_cache_end_entries']}/"
            f"{resource['sprite_cache_peak_entries']} | "
            f"{_human_bytes(resource['sprite_cache_end_bytes'])}/"
            f"{_human_bytes(resource['sprite_cache_peak_bytes'])} | "
            f"{resource['sprite_cache_gc_removals']} |"
        )

    lines.extend(
        [
            "",
            "### Regression guards",
            "",
            "| Guard | Policy | Result | Human-readable evidence |",
            "| --- | --- | --- | --- |",
        ]
    )
    for name, check in checks.items():
        result = "pass" if check["passed"] else "fail"
        policy = "enforced" if check["enforced"] else "informational"
        if name in ("candidate_sustained_p95", "candidate_large_sustained_p95"):
            detail = (
                f"candidate {_milliseconds(check['value_ns'])}; "
                f"budget {_milliseconds(check['limit_ns'])}"
            )
        elif name == "base_candidate_sustained_p95":
            baseline = check["baseline_ns"]
            candidate = check["candidate_ns"]
            change = (candidate / baseline - 1) * 100
            detail = (
                f"base {_milliseconds(baseline)}; candidate {_milliseconds(candidate)} "
                f"({change:+.1f}%); 10% limit {_milliseconds(check['limit_ns'])}"
            )
        elif name in (
            "candidate_sustained_window_p95",
            "candidate_large_sustained_window_p95",
        ):
            first = check["first_window_ns"]
            last = check["last_window_ns"]
            change = (last / first - 1) * 100
            detail = (
                f"first {_milliseconds(first)}; last {_milliseconds(last)} "
                f"({change:+.1f}%); 10% limit {_milliseconds(check['limit_ns'])}"
            )
        elif name == "checkpoint":
            candidate = check["candidate_sha256"]
            if evidence["mode"] == "comparison":
                detail = (
                    "base and candidate checkpoints match and each is repeatable"
                    if check["base_candidate_match"]
                    else "base and candidate checkpoints differ"
                )
            else:
                detail = "candidate checkpoint is repeatable across fresh processes"
            if isinstance(candidate, str):
                detail += f" (`{candidate[:12]}…`)"
        elif name in ("instrumentation_identity", "large_instrumentation_identity"):
            if evidence["mode"] == "candidate-only":
                detail = (
                    "candidate fresh runs used identical instrumentation, fixture, host, and "
                    "viewport"
                    if check["passed"]
                    else "candidate fresh-run identities are inconsistent"
                )
            else:
                detail = (
                    "base and candidate used identical instrumentation, fixture, host, and viewport"
                    if check["passed"]
                    else "base and candidate identities are not comparable"
                )
        elif name.endswith("_determinism"):
            detail = "checkpoint, final state, fixture, and run identity are repeatable"
        elif name.endswith("_reconstruction_equivalence"):
            detail = (
                f"{check['translated_runs']} translated and {check['full_runs']} full runs "
                + (
                    "used one candidate contract and produced identical ordered checkpoints "
                    "and final state"
                    if check["passed"]
                    else "did not preserve the same candidate contract, checkpoints, and final state"
                )
            )
        elif any(
            name == suffix or name.endswith(f"_{suffix}")
            for suffix in (
                "full_redraw_accounting",
                "noop_redraw_avoidance",
                "animation_isolation",
                "idle_wait_recovery",
                "queue_plateau_recovery",
                "lighting_cache_churn",
                "cache_memory_plateau",
                "ordered_final_state",
            )
        ):
            detail = f"{check['runs'] - check['failed_runs']}/{check['runs']} runs passed"
        else:
            raise BenchmarkError(f"unknown movement regression guard: {name}")
        lines.append(f"| `{name}` | {policy} | {result} | {detail} |")
    lines.extend(
        [
            "",
            "The artifact retains every closed native JSON record and the complete versioned "
            "evidence document.",
            "",
        ]
    )
    return "\n".join(lines)


def validate_complete_evidence(
    evidence: dict[str, object],
    baseline_validator: RecordValidator = validate_record,
) -> dict[str, object]:
    """Validate complete evidence and its raw-record-derived summaries."""
    _render_complete_evidence(evidence, "success", baseline_validator)
    return evidence


def render_comment(
    evidence: object,
    client_result: str,
    baseline_validator: RecordValidator = validate_record,
) -> str:
    if evidence is None:
        return _fallback_comment(
            f"Regression evidence is unavailable because client validation finished with "
            f"`{client_result}`."
        )
    if not isinstance(evidence, dict) or evidence.get("schema_version") != EVIDENCE_SCHEMA_VERSION:
        return _fallback_comment(
            "Regression evidence has an invalid schema; see the client validation logs."
        )
    status = evidence.get("status")
    if status == "skipped":
        return _fallback_comment(
            "The base/candidate replay was skipped because no movement-sensitive implementation "
            "files changed."
        )
    if status == "error":
        return _fallback_comment(
            "Regression evidence generation failed before a complete record was available; see "
            "the client validation logs and artifact."
        )
    try:
        return _render_complete_evidence(evidence, client_result, baseline_validator)
    except (BenchmarkError, KeyError, TypeError, ValueError, ZeroDivisionError):
        return _fallback_comment(
            "Regression evidence has an invalid schema; see the client validation logs."
        )


def regular_file(path: str) -> Path:
    resolved = Path(path).resolve()
    if not resolved.is_file():
        raise argparse.ArgumentTypeError(f"not a regular file: {path}")
    return resolved


def _add_candidate_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--candidate-client", required=True, type=regular_file)
    parser.add_argument("--candidate-manifest", required=True, type=regular_file)
    parser.add_argument("--output", required=True, type=Path)


def _fail_with_evidence(output: Path, error: BaseException) -> int:
    write_evidence(output, error_evidence(error))
    print(f"movement regression error: {error}", file=sys.stderr)
    return 2


def _parser_error(message: str) -> NoReturn:
    raise BenchmarkError(message)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)

    compare_parser = commands.add_parser("compare")
    compare_parser.add_argument("--baseline-client", required=True, type=regular_file)
    compare_parser.add_argument("--baseline-manifest", required=True, type=regular_file)
    compare_parser.add_argument("--baseline-schema", required=True, type=regular_file)
    compare_parser.add_argument("--baseline-revision")
    _add_candidate_arguments(compare_parser)
    compare_parser.add_argument("--candidate-revision")
    compare_parser.add_argument("--discrete-manifest", required=True, type=regular_file)
    compare_parser.add_argument("--lighting-manifest", required=True, type=regular_file)
    compare_parser.add_argument("--samples", type=int, default=3, choices=range(3, 10, 2))
    compare_parser.add_argument("--informational-performance", action="store_true")
    compare_parser.add_argument("--comparison-note", choices=COMPARISON_NOTES)

    candidate_parser = commands.add_parser("candidate-only")
    _add_candidate_arguments(candidate_parser)
    candidate_parser.add_argument("--candidate-revision")
    candidate_parser.add_argument("--discrete-manifest", required=True, type=regular_file)
    candidate_parser.add_argument("--lighting-manifest", required=True, type=regular_file)
    candidate_parser.add_argument("--full-matrix", action="store_true")
    candidate_parser.add_argument("--comparison-note", choices=COMPARISON_NOTES)

    skip_parser = commands.add_parser("skip")
    skip_parser.add_argument("--reason", required=True)
    skip_parser.add_argument("--output", required=True, type=Path)

    error_parser = commands.add_parser("error")
    error_parser.add_argument("--reason", required=True, choices=INITIAL_ERROR_REASONS)
    error_parser.add_argument("--output", required=True, type=Path)

    render_parser = commands.add_parser("render-comment")
    render_parser.add_argument("--input", required=True, type=Path)
    render_parser.add_argument("--client-result", required=True)
    render_parser.add_argument("--baseline-schema", type=Path)
    render_parser.add_argument("--output", required=True, type=Path)

    arguments = parser.parse_args(argv)
    if arguments.command == "skip":
        write_evidence(arguments.output, skipped_evidence(arguments.reason))
        return 0
    if arguments.command == "error":
        write_evidence(arguments.output, initial_error_evidence(arguments.reason))
        return 0
    if arguments.command == "render-comment":
        evidence: object = None
        if arguments.input.is_file():
            try:
                evidence = json.loads(
                    arguments.input.read_text(), object_pairs_hook=_reject_duplicate_keys
                )
            except (BenchmarkError, json.JSONDecodeError, OSError):
                evidence = {}
        baseline_validator = validate_record
        if arguments.baseline_schema is not None and arguments.baseline_schema.is_file():
            baseline_validator = load_record_validator(arguments.baseline_schema.resolve())
        arguments.output.write_text(
            render_comment(evidence, arguments.client_result, baseline_validator)
        )
        return 0

    try:
        if arguments.command == "compare":
            expected_informational = (
                arguments.comparison_note in INFORMATIONAL_COMPARISON_NOTES
            )
            if arguments.informational_performance != expected_informational:
                raise BenchmarkError(
                    "--informational-performance and its comparison note must be used together"
                )
            evidence = compare(
                arguments.baseline_client,
                arguments.baseline_manifest,
                arguments.candidate_client,
                arguments.candidate_manifest,
                arguments.discrete_manifest,
                arguments.lighting_manifest,
                arguments.samples,
                arguments.baseline_revision,
                arguments.candidate_revision,
                enforce_performance=not arguments.informational_performance,
                comparison_note=arguments.comparison_note,
                checkpoint_root=arguments.output.parent / "movement-checkpoints",
                baseline_validator=load_record_validator(arguments.baseline_schema),
            )
        elif arguments.command == "candidate-only":
            evidence = candidate_only(
                arguments.candidate_client,
                arguments.candidate_manifest,
                arguments.candidate_revision,
                arguments.discrete_manifest,
                arguments.lighting_manifest,
                arguments.full_matrix,
                enforce_performance=arguments.comparison_note is None,
                comparison_note=arguments.comparison_note,
                checkpoint_root=arguments.output.parent / "movement-checkpoints",
            )
        else:
            _parser_error(f"unsupported command: {arguments.command}")
    except (BenchmarkError, subprocess.TimeoutExpired) as error:
        return _fail_with_evidence(arguments.output, error)
    write_evidence(arguments.output, evidence)
    return 1 if evidence["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
