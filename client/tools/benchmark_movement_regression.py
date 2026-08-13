#!/usr/bin/env python3
"""Run, compare, and report deterministic sustained-map replay results."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time
from typing import NoReturn

from movement_benchmark_schema import validate_record


EVIDENCE_SCHEMA_VERSION = 3
SUSTAINED_P95_LIMIT_NS = 33_300_000
LARGE_SUSTAINED_P95_LIMIT_NS = 125_000_000
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
STANDARD_DISCRETE_CONTEXT = "standard_discrete"
LARGE_DISCRETE_CONTEXT = "large_discrete"
INITIAL_ERROR_REASONS = ("client-validation-ended-before-movement-evidence",)
LIGHTING_FIELDS = {
    "field_rebuilds",
    "field_reuses",
    "field_dirty_pixels",
    "lit_sprite_lookups",
    "lit_sprite_hits",
    "lit_sprite_misses",
    "lit_sprite_evictions",
    "entries",
    "bytes",
    "retained_field_bytes",
}
V3_GUARD_NAMES = (
    "full_redraw_accounting",
    "noop_redraw_avoidance",
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
    "noop_redraw_avoidance",
    "lighting_cache_churn",
)


class BenchmarkError(RuntimeError):
    """A movement benchmark command or its JSON contract failed."""


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


def parse_result(output: str) -> dict[str, object]:
    lines = [line for line in output.splitlines() if line.strip()]
    if len(lines) != 1:
        raise BenchmarkError("movement benchmark must emit exactly one JSON record")
    try:
        result = json.loads(lines[0], object_pairs_hook=_reject_duplicate_keys)
    except json.JSONDecodeError as error:
        raise BenchmarkError("movement benchmark emitted invalid JSON") from error
    try:
        return validate_record(result)
    except ValueError as error:
        raise BenchmarkError(str(error)) from error


def run_benchmark(
    client: Path,
    manifest: Path,
    viewport: str,
    expected_revision: str | None = None,
    checkpoint_directory: Path | None = None,
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
    result = subprocess.run(
        [str(client), "--player-view-movement-benchmark", str(manifest), viewport],
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
    record = parse_result(result.stdout)
    if record.get("schema_version") == 3 and expected_revision is not None:
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
        if record.get("schema_version") == 3 and field in (
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
            if record.get("schema_version") == 3 and section == "lighting":
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
            elif record.get("schema_version") == 3 and section == "queue" and field == "depth":
                values.append(nested["peak_depth"])
            elif record.get("schema_version") == 3 and section == "queue" and field == "bytes":
                values.append(nested["peak_bytes"])
            else:
                values.append(nested[field])
        result[field] = _median_integer(values)
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
    lighting = _nested_medians(records, name, "lighting", LIGHTING_FIELDS)
    lookups = lighting["lit_sprite_lookups"]
    hit_rate = None if lookups == 0 else round(lighting["lit_sprite_hits"] / lookups * 100, 1)
    achieved_p50 = statistics.median(
        phase(record, name)["main_loop"]["achieved_fps"]["p50"] for record in records
    )
    achieved_p95 = statistics.median(
        phase(record, name)["main_loop"]["achieved_fps"]["p95"] for record in records
    )
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
    return {
        "runs": len(records),
        "ticks_per_run": representative["samples"],
        "target_fps": representative["main_loop"]["target_fps"],
        "achieved_fps_p50": round(achieved_p50, 2),
        "achieved_fps_p95": round(achieved_p95, 2),
        "work_capacity_fps_p50": round(1_000_000_000 / p50_ns, 2),
        "work_capacity_fps_p95": round(1_000_000_000 / p95_ns, 2),
        "tick_budget_ms": int(records[0]["tick_ms"]),
        "work_p50_ms": round(p50_ns / 1_000_000, 2),
        "work_p95_ms": round(p95_ns / 1_000_000, 2),
        "map_p50_ms": round(map_p50_ns / 1_000_000, 2),
        "map_p95_ms": round(map_p95_ns / 1_000_000, 2),
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
                            else phase(record, name)["map"][field]
                        )
                        for record in records
                    ]
                )
                for field in (
                    "map_packets",
                    "changed_map_packets",
                    "noop_map_packets",
                    "full_map_draws",
                )
            },
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
        },
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
        "lighting": {**lighting, "hit_rate_percent": hit_rate},
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
    if not records or records[0].get("schema_version") != 3:
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


def _guard_v3_record(record: dict[str, object]) -> dict[str, dict[str, object]]:
    """Return explicit correctness/resource guards for one validated v3 record."""
    if record.get("schema_version") != 3:
        return {}
    phases = {item["name"]: item for item in record["phases"]}
    sustained = phases["sustained"]
    resumed = phases["resumed"]
    guards: dict[str, dict[str, object]] = {}

    map_failures = sum(
        phase_record["map"][field]
        for phase_record in phases.values()
        for field in ("auxiliary_map_draws", "present_failures", "render_failures", "fault_injections")
    )
    map_draws_match = all(
        phase_record["map"]["map_draws"] == phase_record["full_map_draws"]
        and phase_record["map"]["primary_map_draws"] == phase_record["full_map_draws"]
        and sum(phase_record["full_draw_reasons"].values()) >= phase_record["full_map_draws"]
        for phase_record in phases.values()
    )
    noop_redraws = sum(
        phase_record["full_draw_reasons"]["noop_map_packet"]
        for phase_record in phases.values()
    )
    guards["full_redraw_accounting"] = {
        "unexpected_or_failed_draws": map_failures,
        "passed": map_failures == 0 and map_draws_match,
    }
    guards["noop_redraw_avoidance"] = {
        "noop_packet_redraws": noop_redraws,
        "passed": noop_redraws == 0,
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
    if lighting["available"] and record["identity"]["run"]["mode"] == "smooth":
        counters = lighting["counters"]
        dirty_marks = counters["field_dirty_marks"]
        rebuilds = counters["field_rebuilds"]
        reuses = counters["field_reuses"]
        maximum_updates = sustained["changed_map_packets"] * max(
            1, sustained["map"]["peak_active_levels"]
        )
        cache_valid = dirty_marks <= maximum_updates and rebuilds <= maximum_updates and reuses > 0
    guards["lighting_cache_churn"] = {
        "field_dirty_marks": dirty_marks,
        "field_rebuilds": rebuilds,
        "field_reuses": reuses,
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


def _aggregate_v3_guards(records: list[dict[str, object]]) -> dict[str, dict[str, object]]:
    per_record = [_guard_v3_record(record) for record in records]
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
    if not records or records[0].get("schema_version") != 3:
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


def _require_native_context(
    records: list[dict[str, object]],
    context: str,
    viewport: str,
    lighting_mode: str,
    *,
    exact_runs: int | None = None,
) -> None:
    if not records or (exact_runs is not None and len(records) != exact_runs):
        raise BenchmarkError(f"movement regression {context} has an invalid run count")
    for record in records:
        try:
            run = record["identity"]["run"]
            actual_viewport = run["viewport"]["name"]
            actual_mode = run["mode"]
        except (KeyError, TypeError) as error:
            raise BenchmarkError(f"movement regression {context} has an invalid identity") from error
        if actual_viewport != viewport or actual_mode != lighting_mode:
            raise BenchmarkError(f"movement regression {context} has an invalid identity")


def _validate_enforcement_policy(
    has_baseline: bool, enforce_performance: bool, comparison_note: str | None
) -> None:
    if has_baseline:
        informational = comparison_note == COMPARE_FOUNDATION_NOTE
        if informational != (not enforce_performance) or comparison_note not in (
            None,
            COMPARE_FOUNDATION_NOTE,
        ):
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
        _require_native_context(baseline, "baseline standard", "standard", "smooth")
    if candidate_large:
        _require_native_context(
            candidate_large, "candidate large", "large", "smooth", exact_runs=2
        )
    contexts = additional_contexts or {}
    expected_contexts = {STANDARD_DISCRETE_CONTEXT}
    if candidate_large:
        expected_contexts.add(LARGE_DISCRETE_CONTEXT)
    if set(contexts) != expected_contexts:
        raise BenchmarkError("movement regression has an incomplete context matrix")
    _require_native_context(
        contexts[STANDARD_DISCRETE_CONTEXT],
        "candidate standard discrete",
        "standard",
        "discrete",
        exact_runs=2,
    )
    if candidate_large:
        _require_native_context(
            contexts[LARGE_DISCRETE_CONTEXT],
            "candidate large discrete",
            "large",
            "discrete",
            exact_runs=2,
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
    checks.update(_aggregate_v3_guards(candidate + candidate_large))
    checks["candidate_standard_determinism"] = _context_consistency(candidate)
    if candidate_large:
        checks["large_instrumentation_identity"] = _identity_check([], candidate_large)
        checks["candidate_large_determinism"] = _context_consistency(candidate_large)
    if contexts:
        for context, context_records in contexts.items():
            checks[f"{context}_determinism"] = _context_consistency(context_records)
            for name, check in _aggregate_v3_guards(context_records).items():
                checks[f"{context}_{name}"] = check
    for name, check in checks.items():
        policy_follows_performance = name in PERFORMANCE_CHECK_NAMES or name.endswith(
            INFORMATIONAL_OPTIMIZATION_SUFFIXES
        )
        check["enforced"] = enforce_performance if policy_follows_performance else True
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
    samples: int,
    baseline_revision: str | None = None,
    candidate_revision: str | None = None,
    enforce_performance: bool = True,
    comparison_note: str | None = None,
    checkpoint_root: Path | None = None,
) -> dict[str, object]:
    if discrete_manifest is None:
        raise BenchmarkError("movement comparison requires a discrete manifest")
    records: dict[str, list[dict[str, object]]] = {"baseline": [], "candidate": []}
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
        )
        for sample in range(2)
    ]
    return _build_evidence(
        records["baseline"],
        records["candidate"],
        [],
        {STANDARD_DISCRETE_CONTEXT: standard_discrete},
        enforce_performance=enforce_performance,
        comparison_note=comparison_note,
    )


def candidate_only(
    candidate_client: Path,
    candidate_manifest: Path,
    candidate_revision: str | None = None,
    discrete_manifest: Path | None = None,
    full_matrix: bool = False,
    enforce_performance: bool = True,
    comparison_note: str | None = None,
    checkpoint_root: Path | None = None,
) -> dict[str, object]:
    if discrete_manifest is None:
        raise BenchmarkError("candidate-only movement validation requires a discrete manifest")
    standard = [
        run_benchmark(
            candidate_client,
            candidate_manifest,
            "standard",
            candidate_revision,
            checkpoint_root / f"candidate-standard-{sample + 1}"
            if checkpoint_root is not None
            else None,
        )
        for sample in range(2)
    ]
    standard_discrete = [
        run_benchmark(
            candidate_client,
            discrete_manifest,
            "standard",
            candidate_revision,
            checkpoint_root / f"candidate-standard-discrete-{sample + 1}"
            if checkpoint_root is not None
            else None,
        )
        for sample in range(2)
    ]
    large: list[dict[str, object]] = []
    additional_contexts: dict[str, list[dict[str, object]]] = {
        STANDARD_DISCRETE_CONTEXT: standard_discrete,
    }
    if full_matrix:
        large = [
            run_benchmark(
                candidate_client,
                candidate_manifest,
                "large",
                candidate_revision,
                checkpoint_root / f"candidate-large-smooth-{sample + 1}"
                if checkpoint_root is not None
                else None,
            )
            for sample in range(2)
        ]
        additional_contexts[LARGE_DISCRETE_CONTEXT] = [
            run_benchmark(
                candidate_client,
                discrete_manifest,
                "large",
                candidate_revision,
                checkpoint_root / f"candidate-large-discrete-{sample + 1}"
                if checkpoint_root is not None
                else None,
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


def _phase_summary_for_report(summary: object, context: str) -> dict[str, object]:
    expected = {
        "runs",
        "ticks_per_run",
        "target_fps",
        "achieved_fps_p50",
        "achieved_fps_p95",
        "work_capacity_fps_p50",
        "work_capacity_fps_p95",
        "tick_budget_ms",
        "work_p50_ms",
        "work_p95_ms",
        "map_p50_ms",
        "map_p95_ms",
        "work_p99_ms",
        "work_max_ms",
        "first_window_p95_ms",
        "last_window_p95_ms",
        "map",
        "queue",
        "lighting",
    }
    if not isinstance(summary, dict) or set(summary) != expected:
        raise BenchmarkError(f"invalid phase summary: {context}")
    for field in ("runs", "ticks_per_run", "tick_budget_ms"):
        if type(summary[field]) is not int or summary[field] <= 0:
            raise BenchmarkError(f"invalid phase summary: {context}")
    for field in (
        "target_fps",
        "achieved_fps_p50",
        "achieved_fps_p95",
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
    for field in ("map_p50_ms", "map_p95_ms"):
        if type(summary[field]) not in (int, float) or summary[field] < 0:
            raise BenchmarkError(f"invalid phase summary: {context}")
    if summary["target_fps"] != 8 or summary["tick_budget_ms"] != 125:
        raise BenchmarkError(f"invalid phase summary: {context}")
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


def _expected_evidence_checks(
    *, comparison: bool, large: bool, contexts: set[str]
) -> set[str]:
    expected = {
        "candidate_sustained_p95",
        "candidate_sustained_window_p95",
        "checkpoint",
        "instrumentation_identity",
        "candidate_standard_determinism",
        *V3_GUARD_NAMES,
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
            }
        )
    for context in contexts:
        expected.add(f"{context}_determinism")
        expected.update(f"{context}_{name}" for name in V3_GUARD_NAMES)
    return expected


def _validate_evidence_check(name: str, value: object, enforce_performance: bool) -> None:
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
    elif any(name == guard or name.endswith(f"_{guard}") for guard in V3_GUARD_NAMES):
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
    follows_performance = name in PERFORMANCE_CHECK_NAMES or name.endswith(
        INFORMATIONAL_OPTIMIZATION_SUFFIXES
    )
    expected_enforced = enforce_performance if follows_performance else True
    if check["enforced"] != expected_enforced:
        raise BenchmarkError(f"invalid enforcement policy for check: {name}")


def _render_complete_evidence(evidence: dict[str, object], client_result: str) -> str:
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
    expected_contexts = {STANDARD_DISCRETE_CONTEXT}
    if samples["candidate_large"] == 2:
        expected_contexts.add(LARGE_DISCRETE_CONTEXT)
    context_samples = _require_exact_fields(
        samples["additional_contexts"], expected_contexts, "evidence context samples"
    )
    if any(value != 2 for value in context_samples.values()):
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
            "Standard discrete",
            context_phases[STANDARD_DISCRETE_CONTEXT],
            context_samples[STANDARD_DISCRETE_CONTEXT],
        )
    )
    if samples["candidate_large"] == 2:
        candidate_sets.append(
            (
                "Large discrete",
                context_phases[LARGE_DISCRETE_CONTEXT],
                context_samples[LARGE_DISCRETE_CONTEXT],
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
    for field in ("baseline_standard", "candidate_standard", "candidate_large"):
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
            "Standard discrete",
            context_resources[STANDARD_DISCRETE_CONTEXT],
            context_samples[STANDARD_DISCRETE_CONTEXT],
        )
    )
    if samples["candidate_large"] == 2:
        candidate_resources.append(
            (
                "Large discrete",
                context_resources[LARGE_DISCRETE_CONTEXT],
                context_samples[LARGE_DISCRETE_CONTEXT],
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
        _validate_evidence_check(name, check, evidence["enforced"])
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
    else:
        lines.append(
            f"Candidate-only validation measured `{samples['candidate_standard']}` candidate "
            "standard runs."
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
            "FPS at p50/p95 frame time is derived from the complete simulated main-loop duration "
            "(work plus scheduled wait). Render capacity is the unslept throughput implied by "
            "measured work time; target FPS is the requested 125 ms cadence.",
            "",
            "### Candidate replay timing",
            "",
            "| Context | Phase | Ticks/run | Runs | FPS at p50/p95 frame time | "
            "Render capacity FPS (p50/p95) | Target FPS | Work p50/p95 | MAP p50/p95 | "
            "p95 budget | First → last work-window p95 |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
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
            budget_percent = summary["work_p95_ms"] / summary["tick_budget_ms"] * 100
            lines.append(
                f"| {viewport} | `{name}` | {summary['ticks_per_run']} | {summary['runs']} | "
                f"{summary['achieved_fps_p50']:.2f}/{summary['achieved_fps_p95']:.2f} | "
                f"{summary['work_capacity_fps_p50']:.2f}/"
                f"{summary['work_capacity_fps_p95']:.2f} | {summary['target_fps']} | "
                f"{summary['work_p50_ms']:.2f}/{summary['work_p95_ms']:.2f} ms | "
                f"{summary['map_p50_ms']:.2f}/{summary['map_p95_ms']:.2f} ms | "
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
            "",
            "| Context | Phase | MAP packets (changed/no-op) | Full draws | "
            "Queue peak | Peak bytes | Budget yields/recoveries | Oldest queued | "
            "Simulated queue service | Actual drain p50/p95 | Queue order proof | "
            "Renderer allocations |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
            "---: |",
        ]
    )
    for viewport, name, summary in summaries:
        map_stats = _require_exact_fields(
            summary["map"],
            {
                "map_packets", "changed_map_packets", "noop_map_packets", "full_map_draws",
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
        lines.append(
            f"| {viewport} | `{name}` | {map_stats['map_packets']} "
            f"({map_stats['changed_map_packets']}/{map_stats['noop_map_packets']}) | "
            f"{map_stats['full_map_draws']} | {queue['peak_depth']} | "
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
            "| Context | Phase | Fields rebuilt/reused | Dirty pixels | "
            "Sprite hits/lookups (rate) | Misses | Evictions | Entries | Sprite memory | "
            "Field memory |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for viewport, name, summary in summaries:
        lighting = _require_exact_fields(
            summary["lighting"], LIGHTING_FIELDS | {"hit_rate_percent"},
            f"{viewport} {name} lighting summary",
        )
        hit_rate = lighting["hit_rate_percent"]
        hit_rate_text = "n/a" if hit_rate is None else f"{hit_rate:.1f}%"
        lines.append(
            f"| {viewport} | `{name}` | {lighting['field_rebuilds']}/"
            f"{lighting['field_reuses']} | {lighting['field_dirty_pixels']:,} | "
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
        elif any(
            name == suffix or name.endswith(f"_{suffix}")
            for suffix in (
                "full_redraw_accounting",
                "noop_redraw_avoidance",
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


def render_comment(evidence: object, client_result: str) -> str:
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
        return _render_complete_evidence(evidence, client_result)
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
    compare_parser.add_argument("--baseline-revision")
    _add_candidate_arguments(compare_parser)
    compare_parser.add_argument("--candidate-revision")
    compare_parser.add_argument("--discrete-manifest", required=True, type=regular_file)
    compare_parser.add_argument("--samples", type=int, default=3, choices=range(3, 10, 2))
    compare_parser.add_argument("--informational-performance", action="store_true")
    compare_parser.add_argument("--comparison-note", choices=COMPARISON_NOTES)

    candidate_parser = commands.add_parser("candidate-only")
    _add_candidate_arguments(candidate_parser)
    candidate_parser.add_argument("--candidate-revision")
    candidate_parser.add_argument("--discrete-manifest", required=True, type=regular_file)
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
        arguments.output.write_text(render_comment(evidence, arguments.client_result))
        return 0

    try:
        if arguments.command == "compare":
            expected_informational = arguments.comparison_note == COMPARE_FOUNDATION_NOTE
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
                arguments.samples,
                arguments.baseline_revision,
                arguments.candidate_revision,
                enforce_performance=not arguments.informational_performance,
                comparison_note=arguments.comparison_note,
                checkpoint_root=arguments.output.parent / "movement-checkpoints",
            )
        elif arguments.command == "candidate-only":
            evidence = candidate_only(
                arguments.candidate_client,
                arguments.candidate_manifest,
                arguments.candidate_revision,
                arguments.discrete_manifest,
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
