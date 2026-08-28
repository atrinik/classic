#!/usr/bin/env python3
"""Verify deterministic fresh-process movement benchmark records."""

from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

from movement_benchmark_schema import validate_record


def require_legacy_player_view() -> None:
    if not (Path(__file__).resolve().parents[1] / "src/client/player_view.c").is_file():
        raise SystemExit(
            "legacy player-view benchmark is unavailable on GPU-only revisions; "
            "use the production GPU conformance harness"
        )


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"movement benchmark repeated JSON field: {key}")
        result[key] = value
    return result


def parse_record(output: str) -> dict[str, object]:
    lines = [line for line in output.splitlines() if line.strip()]
    if len(lines) != 1:
        raise ValueError("movement benchmark must emit exactly one JSON record")
    return validate_record(json.loads(lines[0], object_pairs_hook=_reject_duplicate_keys))


def run(client: Path, manifest: Path, viewport: str) -> dict[str, object]:
    started = time.monotonic()
    print(f"movement benchmark: starting fresh {viewport} process", file=sys.stderr, flush=True)
    result = subprocess.run(
        [client, "--player-view-movement-benchmark", manifest, viewport],
        check=False,
        capture_output=True,
        text=True,
        timeout=900 if viewport in ("large", "brynknot") else 180,
    )
    print(
        f"movement benchmark: completed fresh {viewport} process in "
        f"{time.monotonic() - started:.1f}s",
        file=sys.stderr,
        flush=True,
    )
    if result.returncode != 0:
        raise SystemExit(result.stderr)
    try:
        return parse_record(result.stdout)
    except (json.JSONDecodeError, ValueError) as error:
        raise SystemExit(str(error)) from error


def verify_fresh_process_pair(
    first: dict[str, object], second: dict[str, object], viewport: str
) -> None:
    if first["identity"]["run"] != second["identity"]["run"]:
        raise SystemExit(f"{viewport} run identity differs across fresh processes")
    if first["identity"]["instrumentation"] != second["identity"]["instrumentation"]:
        raise SystemExit(f"{viewport} instrumentation differs across fresh processes")
    if first["fixture"] != second["fixture"]:
        raise SystemExit(f"{viewport} fixture identity differs across fresh processes")
    if first["movement_route"] != second["movement_route"]:
        raise SystemExit(f"{viewport} movement route differs across fresh processes")
    if first["checkpoint_sha256"] != second["checkpoint_sha256"]:
        raise SystemExit(f"{viewport} checkpoint is not deterministic across fresh processes")
    if first["final_state_digest"] != second["final_state_digest"]:
        raise SystemExit(f"{viewport} final state is not deterministic across fresh processes")
    if first["checkpoints"] != second["checkpoints"]:
        raise SystemExit(f"{viewport} lifecycle checkpoints differ across fresh processes")
    if first["lifecycle"] != second["lifecycle"]:
        raise SystemExit(f"{viewport} lifecycle accounting differs across fresh processes")
    if [phase["samples"] for phase in first["phases"]] != [
        phase["samples"] for phase in second["phases"]
    ]:
        raise SystemExit(f"{viewport} sample counts differ across fresh processes")


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[3] not in ("standard", "large", "brynknot"):
        raise SystemExit(
            "usage: verify_movement_benchmark.py CLIENT MANIFEST standard|large|brynknot"
        )
    require_legacy_player_view()
    client = Path(sys.argv[1])
    manifest = Path(sys.argv[2])
    viewport = sys.argv[3]
    first = run(client, manifest, viewport)
    second = run(client, manifest, viewport)
    verify_fresh_process_pair(first, second, viewport)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
