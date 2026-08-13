#!/usr/bin/env python3
"""Validate and project the Classic movement benchmark into durable trend data.

The benchmark evidence remains the source of truth.  This module deliberately
contains no GitHub client: the workflow owns the small, authenticated mutation
boundary while these functions remain deterministic and easy to test offline.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import statistics
from typing import Any


SCHEMA_VERSION = 1
EVIDENCE_SCHEMA_VERSION = 4
TREND_RETENTION = 90
PHASES = ("cold", "sustained", "idle", "resumed")


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


def _median(values: list[int]) -> int:
    if not values:
        raise ReportError("cannot summarize an empty sample")
    return int(statistics.median(values))


def _phase_values(records: list[dict[str, Any]], name: str, field: str) -> list[int]:
    values = []
    for record in records:
        phases = _mapping(record.get("phases"), "record phases")
        phase = _mapping(phases.get(name), f"{name} phase")
        values.append(_integer(phase.get(field), f"{name}.{field}"))
    return values


def _record_summary(records: list[dict[str, Any]], name: str) -> dict[str, Any]:
    phases = [_mapping(_mapping(r["phases"], "record phases")[name], f"{name} phase") for r in records]

    def values(field: str) -> list[int]:
        return [_integer(phase.get(field), f"{name}.{field}") for phase in phases]

    map_records = [_mapping(phase.get("map"), f"{name} map") for phase in phases]
    queue = [_mapping(phase.get("queue"), f"{name} queue") for phase in phases]
    lighting = [_mapping(phase.get("lighting"), f"{name} lighting") for phase in phases]
    counters = [_mapping(item.get("counters"), f"{name} lighting counters") for item in lighting]
    sprite = [_mapping(phase.get("sprite_cache"), f"{name} sprite cache") for phase in phases]
    sprite_counters = [_mapping(item.get("counters"), f"{name} sprite counters") for item in sprite]
    reasons: dict[str, int] = {}
    for phase in phases:
        for key, value in _mapping(phase.get("full_draw_reasons"), "draw reasons").items():
            reasons[key] = reasons.get(key, 0) + _integer(value, f"draw reason {key}")

    return {
        "runs": len(records),
        "work_ms": {short: round(_median(_phase_values(records, name, field)) / 1_000_000, 2)
                    for short, field in (("p50", "p50_ns"), ("p95", "p95_ns"),
                                         ("p99", "p99_ns"), ("max", "max_ns"))},
        "window_p95_ms": {
            "first": round(_median(_phase_values(records, name, "first_window_p95_ns")) / 1_000_000, 2),
            "last": round(_median(_phase_values(records, name, "last_window_p95_ns")) / 1_000_000, 2),
        },
        "map": {
            field: _median([
                _integer(
                    phase.get(field, item.get(field)),
                    f"{name}.map.{field}",
                )
                for phase, item in zip(phases, map_records)
            ])
            for field in ("map_draws", "primary_map_draws", "auxiliary_map_draws", "presents",
                          "changed_map_packets", "noop_map_packets")
        },
        "draw_reasons": reasons,
        "queue": {
            field: _median([_integer(item.get(field), f"{name}.queue.{field}") for item in queue])
            for field in ("peak_depth", "peak_bytes", "oldest_age_us", "budget_yields", "recoveries")
        },
        "lighting": {
            field: _median([_integer(item.get(field), f"{name}.lighting.{field}") for item in counters])
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
    if evidence.get("schema_version") != EVIDENCE_SCHEMA_VERSION:
        raise ReportError("unsupported movement evidence schema")
    if evidence.get("status") not in ("passed", "failed"):
        raise ReportError("daily report requires complete movement evidence")
    if type(evidence.get("failed")) is not bool:
        raise ReportError("movement evidence has an invalid result")
    records = _mapping(evidence.get("records"), "evidence records")
    candidate = records.get("candidate_standard")
    if not isinstance(candidate, list) or not candidate:
        raise ReportError("evidence has no standard candidate records")
    if not all(isinstance(record, dict) for record in candidate):
        raise ReportError("candidate records must be objects")
    for record in candidate:
        identity = _mapping(record.get("identity"), "record identity")
        _mapping(identity.get("instrumentation"), "instrumentation identity")
        _mapping(identity.get("implementation"), "implementation identity")
        _mapping(identity.get("run"), "run identity")
        _mapping(record.get("fixture"), "fixture identity")
        _mapping(record.get("phases"), "record phases")
    return evidence


def build_point(evidence: Any, *, commit: str, run_id: str, recorded_at: str,
                environment: dict[str, Any]) -> dict[str, Any]:
    evidence = validate_evidence(evidence)
    records = _mapping(evidence["records"], "evidence records")
    candidate = records["candidate_standard"]
    first = candidate[0]
    identity = _mapping(first["identity"], "record identity")
    run = _mapping(identity["run"], "run identity")
    fixture = _mapping(first["fixture"], "fixture identity")
    instrumentation = _mapping(identity["instrumentation"], "instrumentation identity")
    viewport = _mapping(run["viewport"], "viewport identity")
    implementation = _mapping(identity["implementation"], "implementation identity")
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
        "id": f"{recorded_at[:10]}-{commit[:12]}",
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
