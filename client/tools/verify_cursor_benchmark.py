#!/usr/bin/env python3
"""Verify bounded cursor/animation redraws on the frozen dense fixture."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def run(client: Path, manifest: Path, viewport: str) -> dict[str, object]:
    result = subprocess.run(
        [client, "--player-view-cursor-benchmark", manifest, viewport],
        check=False,
        capture_output=True,
        text=True,
        timeout=180,
        cwd=manifest.parents[4],
    )
    if result.returncode != 0:
        raise SystemExit(result.stderr)
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise SystemExit("cursor benchmark must emit exactly one JSON record")
    try:
        return json.loads(lines[0])
    except json.JSONDecodeError as error:
        raise SystemExit(f"cursor benchmark emitted invalid JSON: {error}") from error


def verify(record: dict[str, object], viewport: str) -> None:
    if record.get("schema") != 1 or record.get("benchmark") != "cursor-redraw":
        raise SystemExit("unexpected cursor benchmark schema")
    if record.get("fixture") != "dense-multi-depth-v1":
        raise SystemExit("cursor benchmark did not use the dense multi-depth fixture")
    if record.get("viewport") != viewport or record.get("completed_world_reused") is not True:
        raise SystemExit("cursor benchmark viewport/world-retention contract failed")
    phases = record.get("phases")
    if not isinstance(phases, list) or [phase.get("name") for phase in phases] != [
        "stationary",
        "world_pointer",
        "ui_pointer",
        "animation",
        "movement",
    ]:
        raise SystemExit("cursor benchmark phase order changed")

    for phase in phases:
        if phase["frame"]["samples"] != 16 or phase["wait"]["samples"] != 16:
            raise SystemExit(f"{phase['name']} did not record the complete frame/wait matrix")
        if phase["pixels_sha256"] == "0" * 64:
            raise SystemExit(f"{phase['name']} did not produce a pixel checkpoint")

    for name in ("stationary", "world_pointer", "ui_pointer"):
        phase = next(item for item in phases if item["name"] == name)
        if phase["redraw_reasons"] != 0 or phase["map"]["map_draws"] != 0:
            raise SystemExit(f"{name} reconstructed the static world")
        if phase["map"]["animation_draws"] != 0:
            raise SystemExit(f"{name} performed an animation map pass")

    animation = next(item for item in phases if item["name"] == "animation")
    if animation["redraw_reasons"] != 4 or animation["map"]["map_draws"] != 0:
        raise SystemExit("animation phase did not stay on the reusable world")
    if animation["map"]["animation_draws"] != 16:
        raise SystemExit("animation phase lost its object-only redraws")

    movement = next(item for item in phases if item["name"] == "movement")
    if movement["redraw_reasons"] != 18 or movement["map"]["map_draws"] != 16:
        raise SystemExit("movement phase did not retain its full redraw reasons")


def deterministic_projection(record: dict[str, object]) -> object:
    return {
        "fixture": record["fixture"],
        "viewport": record["viewport"],
        "phases": [
            {
                "name": phase["name"],
                "redraw_reasons": phase["redraw_reasons"],
                "dirty_pixels": phase["dirty_pixels"],
                "map": phase["map"],
                "lighting_counters": phase["lighting"]["counters"],
                "pixels_sha256": phase["pixels_sha256"],
            }
            for phase in record["phases"]
        ],
    }


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[3] not in ("standard", "large"):
        raise SystemExit("usage: verify_cursor_benchmark.py CLIENT MANIFEST standard|large")
    client = Path(sys.argv[1])
    manifest = Path(sys.argv[2])
    viewport = sys.argv[3]
    first = run(client, manifest, viewport)
    second = run(client, manifest, viewport)
    verify(first, viewport)
    verify(second, viewport)
    if deterministic_projection(first) != deterministic_projection(second):
        raise SystemExit("cursor benchmark pixel/counter projection is not deterministic")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
