#!/usr/bin/env python3
"""Verify the closed JSON contract of the offline movement benchmark."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


EXPECTED_SAMPLES = {"cold": 1, "sustained": 480, "idle": 16, "resumed": 80}


def run(client: Path, manifest: Path, viewport: str) -> dict[str, object]:
    result = subprocess.run(
        [client, "--player-view-movement-benchmark", manifest, viewport],
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise SystemExit(result.stderr)
    return json.loads(result.stdout)


def verify(record: dict[str, object]) -> None:
    if record.get("schema_version") != 1 or record.get("tick_ms") != 125:
        raise SystemExit("movement benchmark emitted an incompatible schema")
    phases = {phase["name"]: phase for phase in record.get("phases", [])}
    if set(phases) != set(EXPECTED_SAMPLES):
        raise SystemExit("movement benchmark phases are incomplete")
    for name, samples in EXPECTED_SAMPLES.items():
        phase = phases[name]
        if phase.get("samples") != samples or any(phase.get(key, 0) <= 0 for key in
                                                   ("p50_ns", "p95_ns", "p99_ns", "max_ns")):
            raise SystemExit(f"movement benchmark phase {name} is invalid")
        if phase.get("map_packets") != samples or phase.get("full_map_draws") != samples:
            raise SystemExit(f"movement benchmark phase {name} has incomplete map accounting")
        if phase.get("changed_map_packets", 0) + phase.get("noop_map_packets", 0) != samples:
            raise SystemExit(f"movement benchmark phase {name} has invalid packet accounting")
        if phase.get("queue") != {"depth": 0, "bytes": 0, "oldest_age_ms": 0, "processing_ns": 0}:
            raise SystemExit(f"movement benchmark phase {name} has invalid synchronous queue data")
    if len(record.get("checkpoint_sha256", "")) != 64:
        raise SystemExit("movement benchmark checkpoint is invalid")


def main() -> int:
    client = Path(sys.argv[1])
    manifest = Path(sys.argv[2])
    for viewport in ("standard", "large"):
        first = run(client, manifest, viewport)
        second = run(client, manifest, viewport)
        verify(first)
        verify(second)
        if first["checkpoint_sha256"] != second["checkpoint_sha256"]:
            raise SystemExit(f"{viewport} checkpoint is not deterministic across fresh processes")
        if [phase["samples"] for phase in first["phases"]] != [
            phase["samples"] for phase in second["phases"]
        ]:
            raise SystemExit(f"{viewport} sample counts differ across fresh processes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
