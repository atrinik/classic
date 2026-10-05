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


BOOK_EDITOR_UI_STATES = (
    "book_editor_fields", "book_editor_copy_confirmation", "book_editor_sign_confirmation",
    "book_reader_signed_footer", "book_reader_unsigned_footer_clear",
    "book_editor_insufficient_ink", "book_editor_reopened_draft",
    "book_editor_discard_confirmation", "book_editor_rebase_confirmation",
)
SHA256 = re.compile(r"[0-9a-f]{64}\Z")


def verify_book_editor_ui(record: dict) -> None:
    states = record.get("book_editor_ui")
    if (not isinstance(states, list) or any(not isinstance(state, dict) for state in states) or
            [state.get("name") for state in states] != list(BOOK_EDITOR_UI_STATES)):
        raise ValueError("production book popup UI sweep is incomplete")
    viewport = record.get("viewport")
    if (not isinstance(viewport, list) or len(viewport) != 2 or
            any(type(dimension) is not int or dimension <= 0 for dimension in viewport)):
        raise ValueError("book popup viewport is invalid")
    for state in states:
        name = state["name"]
        size = state.get("output_size")
        if (not SHA256.fullmatch(str(state.get("pixels_sha256", ""))) or
                not isinstance(size, list) or size != viewport or
                any(type(dimension) is not int for dimension in size)):
            raise ValueError(f"{name} book popup pixel checkpoint is invalid")
        if (not str(state.get("artifact", "")).startswith("review/") or
                not SHA256.fullmatch(str(state.get("artifact_sha256", "")))):
            raise ValueError(f"{name} book popup review artifact is absent")
        if state.get("command") != "" or state.get("asynchronous") is not False:
            raise ValueError(f"{name} book popup checkpoint has an unexpected command/readback")
        steady = state.get("steady_state", {})
        if (steady.get("uploads") != steady.get("slot_uniform_uploads") or
                steady.get("upload_bytes") != steady.get("slot_uniform_upload_bytes") or
                not isinstance(steady.get("slot_uniform_uploads"), int) or
                not isinstance(steady.get("slot_uniform_upload_bytes"), int) or
                not steady["slot_uniform_uploads"] * 16 <= steady["slot_uniform_upload_bytes"] <= steady["slot_uniform_uploads"] * 1024 or
                any(steady.get(field) != 0 for field in
                    ("resource_creations", "resource_destructions", "readbacks", "fallbacks"))):
            raise ValueError(f"{name} book popup steady frame is not retained")
    by_name = {state["name"]: state for state in states}
    for names in (("book_editor_fields", "book_editor_copy_confirmation", "book_editor_sign_confirmation"),
                  ("book_reader_signed_footer", "book_reader_unsigned_footer_clear"),
                  ("book_editor_insufficient_ink", "book_editor_reopened_draft")):
        if len({by_name[name]["pixels_sha256"] for name in names}) != len(names):
            raise ValueError("book popup visual transitions are indistinguishable")


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
    verify_book_editor_ui(record)


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
