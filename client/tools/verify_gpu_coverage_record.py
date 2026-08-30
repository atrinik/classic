#!/usr/bin/env python3
"""Verify the hosted GPU coverage record belongs to the exact source revision."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


REVISION = re.compile(r"[0-9a-f]{40}\Z")
ROOT_GLYPHS = {
    "intro_server_browser": {"count": 383, "semantic_hash": "29c427a4eff9acbd"},
    "login_popup": {"count": 385, "semantic_hash": "4266544b0b8b6fbd"},
    "popup_character_selection": {"count": 385, "semantic_hash": "4266544b0b8b6fbd"},
}


def verify(record_path: Path, revision: str) -> None:
    if REVISION.fullmatch(revision) is None:
        raise ValueError("expected revision must be a full lowercase Git SHA")
    records = [json.loads(line) for line in record_path.read_text(encoding="utf-8").splitlines()
               if line.strip()]
    if len(records) != 1:
        raise ValueError("GPU coverage must emit exactly one UI closure record")
    record = records[0]
    if record.get("fixture") != "gpu-ui-closure":
        raise ValueError("GPU coverage emitted the wrong fixture")
    if record.get("revision") != revision or record.get("dirty") is not False:
        raise ValueError("GPU coverage record does not match the exact clean source revision")
    states = {state.get("name"): state for state in record.get("ui_closure", [])}
    for name, expected in ROOT_GLYPHS.items():
        if states.get(name, {}).get("root_glyphs") != expected:
            raise ValueError(f"{name} root glyph submission contract changed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("record", type=Path)
    parser.add_argument("--revision", required=True)
    arguments = parser.parse_args()
    try:
        verify(arguments.record, arguments.revision)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
