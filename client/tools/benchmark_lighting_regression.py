#!/usr/bin/env python3
"""Compare frozen player-view lighting medians on one host."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys


MODES = {"standard": 10, "large": 15}
PREFIX = "player-view-benchmark"


class BenchmarkError(RuntimeError):
    """A benchmark command or its closed output contract failed."""


def parse_result(output: str, expected_mode: str) -> int:
    lines = [line for line in output.splitlines() if line]
    if len(lines) != 1:
        raise BenchmarkError("benchmark must emit exactly one non-empty line")
    fields = lines[0].split("\t")
    if len(fields) != 4 or fields[0] != PREFIX or fields[1] != expected_mode:
        raise BenchmarkError("benchmark emitted an incompatible result record")
    try:
        iterations = int(fields[2], 10)
        median_ns = int(fields[3], 10)
    except ValueError as error:
        raise BenchmarkError("benchmark result contains a non-integer value") from error
    if iterations < 3 or iterations % 2 == 0 or median_ns <= 0:
        raise BenchmarkError("benchmark result contains invalid bounds")
    return median_ns


def run_benchmark(client: Path, manifest: Path, mode: str) -> int:
    result = subprocess.run(
        [str(client), "--player-view-benchmark", str(manifest), mode],
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise BenchmarkError(
            f"{client.name} {mode} benchmark failed ({result.returncode}): "
            f"{result.stderr.strip()}"
        )
    return parse_result(result.stdout, mode)


def compare(
    baseline_client: Path,
    baseline_manifest: Path,
    candidate_client: Path,
    candidate_manifest: Path,
    samples: int,
) -> dict[str, object]:
    measurements: dict[str, dict[str, list[int]]] = {
        mode: {"baseline": [], "candidate": []} for mode in MODES
    }
    for sample in range(samples):
        order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
        for mode in MODES:
            for implementation in order:
                if implementation == "baseline":
                    duration = run_benchmark(baseline_client, baseline_manifest, mode)
                else:
                    duration = run_benchmark(candidate_client, candidate_manifest, mode)
                measurements[mode][implementation].append(duration)

    comparisons: dict[str, object] = {}
    failed = False
    for mode, limit_percent in MODES.items():
        baseline = int(statistics.median(measurements[mode]["baseline"]))
        candidate = int(statistics.median(measurements[mode]["candidate"]))
        limit = baseline * (100 + limit_percent) // 100
        passed = candidate <= limit
        failed = failed or not passed
        comparisons[mode] = {
            "baseline_median_ns": baseline,
            "candidate_median_ns": candidate,
            "limit_percent": limit_percent,
            "limit_ns": limit,
            "passed": passed,
            "samples": measurements[mode],
        }
    return {"schema_version": 1, "failed": failed, "comparisons": comparisons}


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
    for mode, result in evidence["comparisons"].items():
        regression = (result["candidate_median_ns"] / result["baseline_median_ns"] - 1) * 100
        print(
            f"{mode}: baseline={result['baseline_median_ns']}ns "
            f"candidate={result['candidate_median_ns']}ns regression={regression:.2f}% "
            f"limit={result['limit_percent']}%"
        )
    return 1 if evidence["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
