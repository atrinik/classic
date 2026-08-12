#!/usr/bin/env python3
"""Compare deterministic sustained-map replay results on one Release host."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys


SCHEMA_VERSION = 1
SUSTAINED_P95_LIMIT_NS = 33_300_000
RELATIVE_LIMIT_PERCENT = 10
REQUIRED_PHASES = {"cold": 1, "sustained": 480, "idle": 16, "resumed": 80}


class BenchmarkError(RuntimeError):
    """A movement benchmark command or its JSON contract failed."""


def parse_result(output: str) -> dict[str, object]:
    lines = [line for line in output.splitlines() if line]
    if len(lines) != 1:
        raise BenchmarkError("movement benchmark must emit exactly one JSON record")
    try:
        result = json.loads(lines[0])
    except json.JSONDecodeError as error:
        raise BenchmarkError("movement benchmark emitted invalid JSON") from error
    if result.get("schema_version") != SCHEMA_VERSION or result.get("tick_ms") != 125:
        raise BenchmarkError("movement benchmark emitted an incompatible schema")
    phases = {phase.get("name"): phase for phase in result.get("phases", [])}
    if set(phases) != set(REQUIRED_PHASES):
        raise BenchmarkError("movement benchmark phases are incomplete")
    for name, samples in REQUIRED_PHASES.items():
        phase = phases[name]
        if phase.get("samples") != samples or any(
            not isinstance(phase.get(field), int) or phase[field] <= 0
            for field in ("p50_ns", "p95_ns", "p99_ns", "max_ns")
        ):
            raise BenchmarkError(f"movement benchmark phase {name} is invalid")
    if not isinstance(result.get("checkpoint_sha256"), str) or len(result["checkpoint_sha256"]) != 64:
        raise BenchmarkError("movement benchmark checkpoint is invalid")
    return result


def run_benchmark(client: Path, manifest: Path) -> dict[str, object]:
    result = subprocess.run(
        [str(client), "--player-view-movement-benchmark", str(manifest), "standard"],
        check=False,
        capture_output=True,
        text=True,
        timeout=180,
    )
    if result.returncode != 0:
        raise BenchmarkError(f"{client.name} movement benchmark failed: {result.stderr.strip()}")
    return parse_result(result.stdout)


def phase(record: dict[str, object], name: str) -> dict[str, object]:
    return next(item for item in record["phases"] if item["name"] == name)


def compare(
    baseline_client: Path,
    baseline_manifest: Path,
    candidate_client: Path,
    candidate_manifest: Path,
    samples: int,
) -> dict[str, object]:
    records: dict[str, list[dict[str, object]]] = {"baseline": [], "candidate": []}
    for sample in range(samples):
        order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
        for implementation in order:
            records[implementation].append(
                run_benchmark(
                    baseline_client if implementation == "baseline" else candidate_client,
                    baseline_manifest if implementation == "baseline" else candidate_manifest,
                )
            )
    baseline_p95 = int(
        statistics.median(phase(record, "sustained")["p95_ns"] for record in records["baseline"])
    )
    candidate_p95 = int(
        statistics.median(phase(record, "sustained")["p95_ns"] for record in records["candidate"])
    )
    candidate_initial = int(
        statistics.median(phase(record, "sustained")["p50_ns"] for record in records["candidate"])
    )
    candidate_resumed = int(
        statistics.median(phase(record, "resumed")["p50_ns"] for record in records["candidate"])
    )
    relative_limit = candidate_initial * (100 + RELATIVE_LIMIT_PERCENT) // 100
    checks = {
        "candidate_sustained_p95": {
            "value_ns": candidate_p95,
            "limit_ns": SUSTAINED_P95_LIMIT_NS,
            "passed": candidate_p95 <= SUSTAINED_P95_LIMIT_NS,
        },
        "base_candidate_sustained_p95": {
            "baseline_ns": baseline_p95,
            "candidate_ns": candidate_p95,
            "limit_ns": baseline_p95 * (100 + RELATIVE_LIMIT_PERCENT) // 100,
            "passed": candidate_p95 <= baseline_p95 * (100 + RELATIVE_LIMIT_PERCENT) // 100,
        },
        "candidate_resumed_p50": {
            "initial_ns": candidate_initial,
            "resumed_ns": candidate_resumed,
            "limit_ns": relative_limit,
            "passed": candidate_resumed <= relative_limit,
        },
        "checkpoint": {
            "passed": all(
                record["checkpoint_sha256"] == records["candidate"][0]["checkpoint_sha256"]
                for record in records["candidate"]
            )
        },
    }
    return {"schema_version": SCHEMA_VERSION, "failed": not all(check["passed"] for check in checks.values()), "samples": samples, "checks": checks}


def regular_file(path: str) -> Path:
    resolved = Path(path).resolve()
    if not resolved.is_file():
        raise argparse.ArgumentTypeError(f"not a regular file: {path}")
    return resolved


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-client", required=True, type=regular_file)
    parser.add_argument("--baseline-manifest", required=True, type=regular_file)
    parser.add_argument("--candidate-client", required=True, type=regular_file)
    parser.add_argument("--candidate-manifest", required=True, type=regular_file)
    parser.add_argument("--samples", type=int, default=3, choices=range(3, 10, 2))
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    try:
        evidence = compare(
            arguments.baseline_client,
            arguments.baseline_manifest,
            arguments.candidate_client,
            arguments.candidate_manifest,
            arguments.samples,
        )
    except (BenchmarkError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    return 1 if evidence["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
