#!/usr/bin/env python3
"""Prove smooth-lighting operation probes are optional and count-stable."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

from movement_benchmark_schema import validate_record


def require_legacy_player_view() -> None:
    if not (Path(__file__).resolve().parents[1] / "src/client/player_view.c").is_file():
        raise SystemExit(
            "legacy player-view benchmark is unavailable on GPU-only revisions; "
            "use the production GPU conformance harness"
        )


def run(client: Path, manifest: Path, probe_mode: str) -> dict[str, object]:
    result = subprocess.run(
        [
            client,
            "--player-view-movement-benchmark",
            manifest,
            "standard",
            "translated",
            "isolated",
            probe_mode,
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=180,
    )
    if result.returncode != 0:
        raise SystemExit(result.stderr)
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise SystemExit("probe control did not emit exactly one movement record")
    try:
        return validate_record(json.loads(lines[0]))
    except (json.JSONDecodeError, ValueError) as error:
        raise SystemExit(str(error)) from error


def verify(timed: dict[str, object], untimed: dict[str, object]) -> None:
    timed_run = dict(timed["identity"]["run"])
    untimed_run = dict(untimed["identity"]["run"])
    if timed_run.pop("fine_timing") is not True or untimed_run.pop("fine_timing") is not False:
        raise SystemExit("probe control identities do not describe their timing modes")
    if timed_run != untimed_run:
        raise SystemExit("probe control changed the workload identity")
    for field in (
        "fixture",
        "checkpoints",
        "lifecycle",
        "checkpoint_sha256",
        "final_state_digest",
    ):
        if timed[field] != untimed[field]:
            raise SystemExit(f"probe control changed deterministic {field}")
    if timed["identity"]["instrumentation"] != untimed["identity"]["instrumentation"]:
        raise SystemExit("probe control changed the instrumentation identity")

    timed_calls = 0
    for timed_phase, untimed_phase in zip(timed["phases"], untimed["phases"], strict=True):
        if timed_phase["name"] != untimed_phase["name"]:
            raise SystemExit("probe control changed phase order")
        timed_lighting = timed_phase["lighting"]
        untimed_lighting = untimed_phase["lighting"]
        if timed_lighting["counters"] != untimed_lighting["counters"]:
            raise SystemExit(f"probe control changed {timed_phase['name']} operation counts")
        for timed_level, untimed_level in zip(
            timed_lighting["levels"], untimed_lighting["levels"], strict=True
        ):
            if timed_level["counters"] != untimed_level["counters"]:
                raise SystemExit(
                    f"probe control changed {timed_phase['name']} per-depth counts"
                )
        timed_calls += sum(item["calls"] for item in timed_lighting["timings"].values())
        if any(
            item[field] != 0
            for item in untimed_lighting["timings"].values()
            for field in ("calls", "elapsed")
        ):
            raise SystemExit("disabled fine probes still sampled the operation clock")

    sustained = timed["phases"][1]["lighting"]["counters"]
    if (
        sustained["whole_field_compositions"] == 0
        and (
            sustained["lit_sprite_lookups"] == 0
            or sustained["lit_sprite_hits"] * 10 < sustained["lit_sprite_lookups"] * 9
            or sustained["lit_sprite_misses"] * 10 > sustained["lit_sprite_lookups"]
            or sustained["lit_sprite_invalidation_scroll"] != 0
            or sustained["lit_sprite_invalidation_field"] != 0
        )
    ):
        raise SystemExit("lighting composition guard is not reproducible")
    if timed_calls == 0:
        raise SystemExit("enabled fine probes recorded no operation timing")


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify_movement_probe_control.py CLIENT MANIFEST")
    require_legacy_player_view()
    client = Path(sys.argv[1])
    manifest = Path(sys.argv[2])
    verify(run(client, manifest, "timed"), run(client, manifest, "untimed"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
