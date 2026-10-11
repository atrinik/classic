#!/usr/bin/env python3
"""Compare complete, bounded headless font evidence without tolerances.

Copyright 2026 The Atrinik Project
SPDX-License-Identifier: GPL-2.0-or-later
"""
from __future__ import annotations

import argparse
import itertools
import json
import os
from pathlib import Path

FONTS = ("arial.ttf", "courier.otf", "fanwood.otf", "logisoso.ttf",
         "mono.ttf", "pecita.otf", "sans.ttf", "serif.ttf")
POINTS = (11, 16, 24)
STYLES = ("normal", "bold", "italic", "bold-italic", "underline")
MODES = ("solid", "blended")
GLYPHS = (77, 103, 233, 65, 86, 102, 105, 101, 769)
CASES = tuple(itertools.product(FONTS, POINTS, STYLES, MODES, range(7)))
MAX_MANIFEST = 2 * 1024 * 1024
MAX_PIXELS = 128 * 1024 * 1024
FIELDS = {"id", "font", "point", "style", "mode", "sample", "text_size",
          "surface_size", "font_metrics", "glyphs"}


class EvidenceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON field: {key}")
        result[key] = value
    return result


def integers(value: object, count: int, name: str) -> list[int]:
    require(isinstance(value, list) and len(value) == count, f"invalid {name}")
    require(all(type(item) is int and -65536 <= item <= 65536 for item in value),
            f"invalid {name} integers")
    return value


def read_file(path: Path, maximum: int) -> bytes:
    require(not path.is_symlink() and path.is_file(), f"not a regular evidence file: {path}")
    require(path.stat().st_size <= maximum, f"evidence file too large: {path}")
    with path.open("rb") as stream:
        data = stream.read(maximum + 1)
    require(len(data) <= maximum, f"evidence file grew beyond bound: {path}")
    return data


def load_manifest(directory: Path) -> dict:
    require(not directory.is_symlink() and directory.is_dir(), f"invalid evidence directory: {directory}")
    manifest = json.loads(read_file(directory / "manifest.json", MAX_MANIFEST),
                          object_pairs_hook=unique_object)
    require(isinstance(manifest, dict) and set(manifest) == {"schema", "cases"},
            "invalid manifest fields")
    require(type(manifest["schema"]) is int and manifest["schema"] == 1, "invalid schema")
    cases = manifest["cases"]
    require(isinstance(cases, list) and len(cases) == len(CASES), "incomplete case matrix")
    total = 0
    for index, (case, identity) in enumerate(zip(cases, CASES)):
        require(isinstance(case, dict) and set(case) == FIELDS, f"invalid case fields: {index}")
        require(type(case["id"]) is int and case["id"] == index, f"invalid case ID: {index}")
        require(type(case["point"]) is int and type(case["sample"]) is int,
                f"invalid case identity types: {index}")
        require(tuple(case[key] for key in ("font", "point", "style", "mode", "sample")) == identity,
                f"invalid case identity: {index}")
        integers(case["text_size"], 2, "text size")
        width, height = integers(case["surface_size"], 2, "surface size")
        require(0 < width <= 1024 and 0 < height <= 1024, f"surface bound: {index}")
        total += width * height * 4
        require(total <= MAX_PIXELS, "total pixel bound")
        integers(case["font_metrics"], 4, "font metrics")
        require(isinstance(case["glyphs"], list) and len(case["glyphs"]) == len(GLYPHS),
                f"invalid glyph matrix: {index}")
        for glyph, codepoint in zip(case["glyphs"], GLYPHS):
            integers(glyph, 7, "glyph metrics")
            require(glyph[0] == codepoint and glyph[1] in (0, 1), f"invalid glyph identity: {index}")
            require(glyph[1] or glyph[2:] == [0] * 5, f"missing glyph has metrics: {index}")
    expected = {"manifest.json"} | {f"case{index:04d}.rgba" for index in range(len(CASES))}
    actual = set()
    with os.scandir(directory) as entries:
        for entry in entries:
            require(entry.name in expected, "missing or unexpected evidence files")
            actual.add(entry.name)
    require(actual == expected, "missing or unexpected evidence files")
    return manifest


def compare(baseline: Path, candidate: Path) -> int:
    before, after = load_manifest(baseline), load_manifest(candidate)
    require(before == after, "font metadata differs")
    for case in before["cases"]:
        index = case["id"]
        width, height = case["surface_size"]
        size = width * height * 4
        name = f"case{index:04d}.rgba"
        left, right = read_file(baseline / name, size), read_file(candidate / name, size)
        require(len(left) == size and len(right) == size, f"pixel byte count differs: {index}")
        for pixels in (left, right):
            require(all(pixels[offset + 3] or pixels[offset:offset + 3] == b"\0\0\0"
                        for offset in range(0, size, 4)), f"noncanonical transparent pixel: {index}")
        require(left == right, f"font pixels differ: {index}")
    return len(CASES)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    try:
        count = compare(args.baseline, args.candidate)
    except (OSError, ValueError, RecursionError) as error:
        parser.exit(1, f"font equivalence failed: {error}\n")
    print(f"font equivalence: {count} cases, exact metadata and RGBA pixels")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
