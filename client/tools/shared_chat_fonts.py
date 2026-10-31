#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate an explicitly supplied font manifest and emit private build inputs."""
import hashlib
import json
import pathlib
import sys

manifest = pathlib.Path(sys.argv[1]).resolve(strict=True)
data = json.loads(manifest.read_text())
fonts = data["fonts"]
if data["schema"] != 1 or not 1 <= len(fonts) <= 32:
    raise ValueError("unsupported font manifest")
rows = []
total = 0
for font in fonts:
    family = font["family"]
    if not isinstance(family, str) or not 1 <= len(family.encode()) <= 128 or any(ord(c) < 32 for c in family):
        raise ValueError("invalid family")
    path = (manifest.parent / font["path"]).resolve(strict=True)
    path.relative_to(manifest.parent)
    size = path.stat().st_size
    total += size
    if not 0 < size <= 32 * 1024 * 1024 or total > 64 * 1024 * 1024:
        raise ValueError("font budget exceeded")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if size != font["bytes"] or digest != font["sha256"]:
        raise ValueError("font identity mismatch")
    rows.append("{%s, %s, %d, {%s}}" % (json.dumps(family), json.dumps(str(path)), size,
        ",".join("0x" + digest[i:i+2] for i in range(0, 64, 2))))
pathlib.Path(sys.argv[2]).write_text("/* Generated from an explicit verified manifest. */\n"
    "static const struct pilot_font_file { const char *family, *path; size_t size; uint8_t sha256[32]; } pilot_fonts[] = {\n"
    + ",\n".join(rows) + "\n};\n")
