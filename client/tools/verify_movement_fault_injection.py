#!/usr/bin/env python3
"""Prove movement-only renderer and deterministic-cache fault seams."""

from __future__ import annotations

import os
import json
from pathlib import Path
import subprocess
import sys


FAULT = "mutable-rle"
FAULT_DIAGNOSTIC = "player-view: movement fault mutable-rle was injected and detected"
CLOCK_FAULT = "sprite-cache-clock"
CLOCK_FAULT_DIAGNOSTIC = (
    "player-view: movement fault sprite-cache-clock was injected and detected"
)
# The unified scene compositor no longer constructs or locks per-sprite lit
# surfaces, so the former per-sprite construction fault matrix is obsolete.


def verify_cursor(client: Path, manifest: Path) -> None:
    """Run one dense cursor matrix in the normal coverage test set."""
    cursor_manifest = manifest.with_name("dense-cursor.xml")
    environment = os.environ.copy()
    environment.pop("ATRINIK_MOVEMENT_FAULT", None)
    result = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).with_name("verify_cursor_benchmark.py")),
            str(client),
            str(cursor_manifest),
            "standard",
        ],
        check=False,
        capture_output=True,
        env=environment,
        text=True,
        timeout=180,
    )
    if result.returncode != 0:
        raise SystemExit(f"dense cursor coverage guard failed:\n{result.stderr}")


def invoke(
    client: Path, arguments: list[object], fault: str = FAULT
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["ATRINIK_MOVEMENT_FAULT"] = fault
    return subprocess.run(
        [str(client), *(str(argument) for argument in arguments)],
        check=False,
        capture_output=True,
        env=environment,
        text=True,
        timeout=20,
    )


def verify_frozen(client: Path, manifest: Path) -> None:
    result = invoke(client, ["--player-view-benchmark", manifest, "standard"])
    if result.returncode != 0:
        raise SystemExit(f"frozen benchmark unexpectedly consumed movement fault:\n{result.stderr}")
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    fields = lines[0].split("\t") if len(lines) == 1 else []
    if len(fields) != 4 or fields[:2] != ["player-view-benchmark", "standard"]:
        raise SystemExit("frozen benchmark did not emit its one expected result")
    if FAULT_DIAGNOSTIC in result.stderr or "mutable-surface failure" in result.stderr:
        raise SystemExit("frozen benchmark activated the movement-only fault")


def verify_movement(client: Path, manifest: Path, viewport: str = "standard") -> None:
    verify_movement_fault(client, manifest, FAULT, FAULT_DIAGNOSTIC, viewport)


def verify_movement_fault(
    client: Path,
    manifest: Path,
    fault: str,
    diagnostic: str,
    viewport: str = "standard",
) -> None:
    result = invoke(
        client,
        ["--player-view-movement-benchmark", manifest, viewport],
        fault,
    )
    if result.returncode == 0:
        raise SystemExit(f"movement benchmark did not reject the injected {fault} failure")
    if diagnostic not in result.stderr:
        raise SystemExit(
            f"movement benchmark failed without proving the {fault} fault was injected "
            f"and detected:\n{result.stderr}"
        )
    for line in result.stdout.splitlines():
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(record, dict) and record.get("benchmark") == "player-view-movement":
            raise SystemExit("failed movement benchmark emitted a consumable JSON record")


def main() -> int:
    if len(sys.argv) not in (4, 5) or (len(sys.argv) == 5 and sys.argv[4] not in (
        "standard", "brynknot"
    )):
        raise SystemExit(
            "usage: verify_movement_fault_injection.py CLIENT FROZEN_MANIFEST "
            "MOVEMENT_MANIFEST [standard|brynknot]"
        )
    client = Path(sys.argv[1])
    verify_frozen(client, Path(sys.argv[2]))
    movement_manifest = Path(sys.argv[3])
    viewport = sys.argv[4] if len(sys.argv) == 5 else "standard"
    verify_movement(client, movement_manifest, viewport)
    verify_movement_fault(
        client,
        movement_manifest,
        CLOCK_FAULT,
        CLOCK_FAULT_DIAGNOSTIC,
        viewport,
    )
    verify_cursor(client, movement_manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
