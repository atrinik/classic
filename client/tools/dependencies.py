#!/usr/bin/env python3
"""Run Classic's authoritative immutable dependency fetcher for the client."""

from pathlib import Path
import runpy
import sys


if not any(argument == "--root" or argument.startswith("--root=") for argument in sys.argv[1:]):
    sys.argv.extend(("--root", str(Path(__file__).resolve().parents[1])))

runpy.run_path(
    str(Path(__file__).resolve().parents[2] / "server" / "tools" / "dependencies.py"),
    run_name="__main__",
)
