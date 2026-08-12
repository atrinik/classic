#!/usr/bin/env python3
"""Verify the closed JSON contract of the offline movement benchmark."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    client = Path(sys.argv[1])
    manifest = Path(sys.argv[2])
    result = subprocess.run(
        [client, "--player-view-movement-benchmark", manifest, "standard"],
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode:
        raise SystemExit(result.stderr)
    record = json.loads(result.stdout)
    if record.get("schema_version") != 1 or record.get("tick_ms") != 125:
        raise SystemExit("movement benchmark emitted an incompatible schema")
    phases = {phase["name"]: phase for phase in record.get("phases", [])}
    expected = {"cold": 1, "sustained": 480, "idle": 16, "resumed": 80}
    if set(phases) != set(expected):
        raise SystemExit("movement benchmark phases are incomplete")
    for name, samples in expected.items():
        phase = phases[name]
        if phase.get("samples") != samples or any(phase.get(key, 0) <= 0 for key in
                                                   ("p50_ns", "p95_ns", "p99_ns", "max_ns")):
            raise SystemExit(f"movement benchmark phase {name} is invalid")
    if len(record.get("checkpoint_sha256", "")) != 64:
        raise SystemExit("movement benchmark checkpoint is invalid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
