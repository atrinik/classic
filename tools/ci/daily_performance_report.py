#!/usr/bin/env python3
"""Validate and project the Classic movement benchmark into durable trend data.

The benchmark evidence remains the source of truth.  This module deliberately
contains no GitHub client: the workflow owns the small, authenticated mutation
boundary while these functions remain deterministic and easy to test offline.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import statistics
import sys
from typing import Any


SCHEMA_VERSION = 1
TREND_RETENTION = 90
PHASES = ("cold", "sustained", "idle", "resumed")
REQUIRED_CONTEXTS = ("standard_discrete", "large_discrete")

CLIENT_TOOLS = Path(__file__).resolve().parents[2] / "client" / "tools"
sys.path.insert(0, str(CLIENT_TOOLS))
_BENCHMARK_SPEC = importlib.util.spec_from_file_location(
    "daily_performance_benchmark_contract",
    CLIENT_TOOLS / "benchmark_movement_regression.py",
)
if _BENCHMARK_SPEC is None or _BENCHMARK_SPEC.loader is None:
    raise RuntimeError("cannot load the movement benchmark evidence contract")
benchmark_contract = importlib.util.module_from_spec(_BENCHMARK_SPEC)
_BENCHMARK_SPEC.loader.exec_module(benchmark_contract)


class ReportError(ValueError):
    """Raised when benchmark evidence or trend data is not closed."""


def _mapping(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ReportError(f"{context} must be an object")
    return value


def _integer(value: Any, context: str) -> int:
    if type(value) is not int or value < 0:
        raise ReportError(f"{context} must be a non-negative integer")
    return value


def _number(value: Any, context: str) -> int | float:
    if type(value) not in (int, float) or value < 0:
        raise ReportError(f"{context} must be a non-negative number")
    return value


def _median(values: list[int]) -> int:
    if not values:
        raise ReportError("cannot summarize an empty sample")
    return int(statistics.median(values))


def _record_summary(records: list[dict[str, Any]], name: str) -> dict[str, Any]:
    summary = benchmark_contract.phase_summary(records, name)
    map_summary = _mapping(summary.get("map"), f"{name} map")
    queue_summary = _mapping(summary.get("queue"), f"{name} queue")
    lighting_summary = _mapping(summary.get("lighting"), f"{name} lighting")
    sprite_counters = [
        _mapping(
            _mapping(
                benchmark_contract.phase(record, name).get("sprite_cache"),
                f"{name} sprite cache",
            ).get("counters"),
            f"{name} sprite counters",
        )
        for record in records
    ]

    return {
        "runs": len(records),
        "work_ms": {
            short: _number(summary.get(field), f"{name}.{field}")
            for short, field in (("p50", "work_p50_ms"), ("p95", "work_p95_ms"),
                                 ("p99", "work_p99_ms"), ("max", "work_max_ms"))
        },
        "window_p95_ms": {
            "first": _number(summary.get("first_window_p95_ms"), f"{name}.first window"),
            "last": _number(summary.get("last_window_p95_ms"), f"{name}.last window"),
        },
        "map": {
            field: _integer(map_summary.get(source), f"{name}.map.{source}")
            for field, source in (("map_draws", "full_map_draws"),
                                  ("primary_map_draws", "primary_map_draws"),
                                  ("auxiliary_map_draws", "auxiliary_map_draws"),
                                  ("presents", "presents"),
                                  ("changed_map_packets", "changed_map_packets"),
                                  ("noop_map_packets", "noop_map_packets"))
        },
        "draw_reasons": _mapping(map_summary.get("draw_reasons"), "draw reasons"),
        "queue": {
            field: _integer(queue_summary.get(field), f"{name}.queue.{field}")
            for field in ("peak_depth", "peak_bytes", "budget_yields", "recoveries")
        } | {
            "oldest_age_us": int(
                _number(queue_summary.get("oldest_age_ms"), f"{name}.queue.oldest_age_ms")
                * 1_000
            )
        },
        "lighting": {
            field: _integer(lighting_summary.get(field), f"{name}.lighting.{field}")
            for field in ("field_rebuilds", "field_reuses", "field_translations",
                          "field_partial_rebuilds", "field_dirty_pixels", "lit_sprite_lookups",
                          "lit_sprite_hits", "lit_sprite_misses", "lit_sprite_evictions")
        },
        "sprite_cache": {
            field: _median([_integer(item.get(field), f"{name}.sprite.{field}") for item in sprite_counters])
            for field in ("lookups", "hits", "misses", "gc_removals")
        },
    }


def validate_evidence(evidence: Any) -> dict[str, Any]:
    evidence = _mapping(evidence, "evidence")
    try:
        benchmark_contract.validate_complete_evidence(evidence)
    except (benchmark_contract.BenchmarkError, KeyError, TypeError, ValueError) as error:
        raise ReportError(f"invalid movement evidence: {error}") from error
    samples = _mapping(evidence.get("samples"), "evidence samples")
    contexts = _mapping(samples.get("additional_contexts"), "evidence contexts")
    if (
        evidence.get("mode") != "candidate-only"
        or samples.get("candidate_standard") != 2
        or samples.get("candidate_large") != 2
        or set(contexts) != set(REQUIRED_CONTEXTS)
        or any(contexts[name] != 2 for name in REQUIRED_CONTEXTS)
    ):
        raise ReportError("daily report requires the complete candidate full matrix")
    return evidence


def build_point(evidence: Any, *, commit: str, run_id: str, recorded_at: str,
                environment: dict[str, Any]) -> dict[str, Any]:
    evidence = validate_evidence(evidence)
    if (
        len(commit) != 40
        or any(character not in "0123456789abcdefABCDEF" for character in commit)
    ):
        raise ReportError("commit must be a full hexadecimal revision")
    if not run_id.isdigit() or int(run_id) <= 0:
        raise ReportError("run ID must be a positive integer")
    records = _mapping(evidence["records"], "evidence records")
    candidate = records["candidate_standard"]
    first = candidate[0]
    identity = _mapping(first["identity"], "record identity")
    run = _mapping(identity["run"], "run identity")
    fixture = _mapping(first["fixture"], "fixture identity")
    instrumentation = _mapping(identity["instrumentation"], "instrumentation identity")
    viewport = _mapping(run["viewport"], "viewport identity")
    implementation = _mapping(identity["implementation"], "implementation identity")
    candidate_sets = [records["candidate_standard"], records["candidate_large"]]
    candidate_sets.extend(records["additional_contexts"].values())
    for record_set in candidate_sets:
        for record in record_set:
            record_implementation = record["identity"]["implementation"]
            if (
                record_implementation["revision"].lower() != commit.lower()
                or record_implementation["dirty_known"] is not True
                or record_implementation["dirty"] is not False
            ):
                raise ReportError("movement evidence does not identify the published commit")
    cohort_material = {
        "instrumentation": instrumentation,
        "fixture": fixture,
        "implementation": {key: value for key, value in implementation.items()
                           if key not in ("revision", "dirty", "dirty_known")},
        "run": run,
        "runner_image": environment.get("runner_image"),
    }
    cohort = json.dumps(cohort_material, sort_keys=True, separators=(",", ":"))
    import hashlib
    cohort_id = hashlib.sha256(cohort.encode()).hexdigest()[:16]
    contexts = _mapping(records.get("additional_contexts"), "additional contexts")
    context_points = {
        name: _record_summary(items, "sustained")
        for name, items in contexts.items()
        if isinstance(items, list) and items
    }
    checks = _mapping(evidence["checks"], "evidence checks")
    large_records = records.get("candidate_large")
    return {
        "schema_version": SCHEMA_VERSION,
        "id": f"run-{run_id}",
        "recorded_at": recorded_at,
        "commit": commit,
        "run_id": run_id,
        "cohort": cohort_id,
        "environment": environment,
        "viewport": viewport,
        "mode": run.get("mode"),
        "status": evidence["status"],
        "fixture": {"manifest_sha256": fixture.get("manifest_sha256"),
                    "snapshot_sha256": fixture.get("snapshot_sha256")},
        "phases": {name: _record_summary(candidate, name) for name in PHASES},
        "large_phases": ({name: _record_summary(large_records, name) for name in PHASES}
                         if isinstance(large_records, list) and large_records else None),
        "contexts": context_points,
        "checks": checks,
        "resources": evidence["resources"].get("candidate_standard"),
    }


def merge_trend(trend: Any, point: dict[str, Any]) -> dict[str, Any]:
    if trend is None:
        trend = {"schema_version": SCHEMA_VERSION, "cohorts": {}}
    trend = _mapping(trend, "trend")
    if trend.get("schema_version") != SCHEMA_VERSION:
        raise ReportError("unsupported trend schema")
    cohorts = trend.setdefault("cohorts", {})
    if not isinstance(cohorts, dict):
        raise ReportError("trend cohorts must be an object")
    for cohort_points in cohorts.values():
        if not isinstance(cohort_points, list):
            raise ReportError("trend cohort points must be an array")
        cohort_points[:] = [
            item
            for item in cohort_points
            if isinstance(item, dict) and item.get("id") != point["id"]
        ]
    points = cohorts.setdefault(point["cohort"], [])
    if not isinstance(points, list):
        raise ReportError("trend cohort points must be an array")
    points[:] = [item for item in points if isinstance(item, dict) and item.get("id") != point["id"]]
    points.append(point)
    points.sort(key=lambda item: (item.get("recorded_at", ""), item.get("id", "")))
    del points[:-TREND_RETENTION]
    alerts = trend.setdefault("alerts", {})
    if not isinstance(alerts, dict):
        raise ReportError("trend alerts must be an object")
    for state in alerts.values():
        if not isinstance(state, dict) or not isinstance(state.get("history"), list):
            raise ReportError("alert state is malformed")
        state["last_transition"] = "none"
        state["history"] = [
            item
            for item in state["history"]
            if isinstance(item, dict) and item.get("id") != point["id"]
        ]
    for metric, check_name in (("standard:sustained_p95", "candidate_sustained_p95"),
                               ("large:sustained_p95", "candidate_large_sustained_p95")):
        check = point["checks"].get(check_name)
        if not isinstance(check, dict):
            continue
        key = f"{point['cohort']}:{metric}"
        state = alerts.setdefault(key, {"active": False, "history": []})
        if not isinstance(state, dict) or not isinstance(state.get("history"), list):
            raise ReportError("alert state is malformed")
        state["history"] = [item for item in state["history"] if item.get("id") != point["id"]]
        result = "failed" if check.get("passed") is not True else "passed"
        state["history"].append({"id": point["id"], "state": result})
        state["history"] = state["history"][-5:]
        recent = [item["state"] for item in state["history"][-2:]]
        transition = "none"
        if recent == ["failed", "failed"] and not state["active"]:
            state["active"] = True
            transition = "regressed"
        elif recent == ["passed", "passed"] and state["active"]:
            state["active"] = False
            transition = "recovered"
        state["last_transition"] = transition
    return trend


def render_summary(point: dict[str, Any], trend: dict[str, Any]) -> str:
    sustained = point["phases"]["sustained"]
    lines = ["## Classic client performance report", "",
             f"- Commit: `{point['commit']}`", f"- Run: `{point['run_id']}`",
             f"- Cohort: `{point['cohort']}`", "",
             "| Phase | p50 | p95 | p99 | first→last p95 |", "| --- | ---: | ---: | ---: | ---: |"]
    for name, phase in point["phases"].items():
        work = phase["work_ms"]
        window = phase["window_p95_ms"]
        lines.append(f"| `{name}` | {work['p50']:.2f} ms | {work['p95']:.2f} ms | "
                     f"{work['p99']:.2f} ms | {window['first']:.2f} → {window['last']:.2f} ms |")
    if point.get("large_phases"):
        lines.extend(["", "### Large viewport", "",
                      "| Phase | p50 | p95 | p99 |", "| --- | ---: | ---: | ---: |"])
        for name, phase in point["large_phases"].items():
            work = phase["work_ms"]
            lines.append(f"| `{name}` | {work['p50']:.2f} ms | {work['p95']:.2f} ms | {work['p99']:.2f} ms |")
    lighting = sustained["lighting"]
    queue = sustained["queue"]
    sprite = sustained["sprite_cache"]
    lines.extend(["", "### Sustained telemetry", "",
                  f"- MAP changed/no-op packets: `{sustained['map']['changed_map_packets']}` / `{sustained['map']['noop_map_packets']}`",
                  f"- Queue peak depth/oldest age: `{queue['peak_depth']}` / `{queue['oldest_age_us']} µs`",
                  f"- Lighting rebuild/reuse: `{lighting['field_rebuilds']}` / `{lighting['field_reuses']}`",
                  f"- Lighting translated/partial rebuild: `{lighting['field_translations']}` / `{lighting['field_partial_rebuilds']}`",
                  f"- Lighting dirty pixels: `{lighting['field_dirty_pixels']}`",
                  f"- Sprite cache hits/misses/evictions: `{sprite['hits']}` / `{sprite['misses']}` / `{sprite['gc_removals']}`",
                  "", "### Retention", "",
                  f"This cohort retains `{len(trend['cohorts'][point['cohort']])}` points (up to {TREND_RETENTION})."])
    transitions = [key for key, value in trend.get("alerts", {}).items()
                   if value.get("last_transition") in ("regressed", "recovered")]
    if transitions:
        lines.extend(["", "### Alert transitions", "", *[f"- `{key}`" for key in transitions]])
    if point["environment"].get("workflow_url"):
        lines.extend(["", f"[Workflow run]({point['environment']['workflow_url']})",
                      "Raw JSON and checkpoint images are attached to this run."])
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--recorded-at", default=datetime.now(timezone.utc).isoformat())
    parser.add_argument("--environment", type=json.loads, default="{}")
    parser.add_argument("--point", type=Path, required=True)
    parser.add_argument("--trend", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()
    point = build_point(json.loads(args.evidence.read_text()), commit=args.commit,
                        run_id=args.run_id, recorded_at=args.recorded_at,
                        environment=args.environment)
    trend = json.loads(args.trend.read_text()) if args.trend.is_file() else None
    trend = merge_trend(trend, point)
    args.point.parent.mkdir(parents=True, exist_ok=True)
    args.trend.parent.mkdir(parents=True, exist_ok=True)
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.point.write_text(json.dumps(point, indent=2, sort_keys=True) + "\n")
    args.trend.write_text(json.dumps(trend, indent=2, sort_keys=True) + "\n")
    args.summary.write_text(render_summary(point, trend))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
