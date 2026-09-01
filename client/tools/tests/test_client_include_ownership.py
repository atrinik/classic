#!/usr/bin/env python3
"""Ensure the Classic client does not regain its catch-all header dependency."""

from __future__ import annotations

import re
import sys
from pathlib import Path


INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]global\.h[>\"]")
SOURCE_SUFFIXES = {".c", ".h"}


def find_violations(client_root: Path) -> list[str]:
    violations: list[str] = []
    source_root = client_root / "src"

    for path in sorted(source_root.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(client_root)
        if path.name == "global.h":
            violations.append(f"obsolete client header exists: {relative}")
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            if INCLUDE_RE.match(line):
                violations.append(
                    f"catch-all header included at {relative}:{line_number}"
                )

    cmake = (client_root / "CMakeLists.txt").read_text(encoding="utf-8")
    if "PRECOMPILED_HEADERS" in cmake or "target_precompile_headers" in cmake:
        violations.append("client CMakeLists.txt still wires a precompiled header")

    integrated_cmake = client_root.parent / "CMakeLists.txt"
    if integrated_cmake.is_file() and "ATRINIK_CLIENT_ENABLE_PRECOMPILED_HEADERS" in integrated_cmake.read_text(
        encoding="utf-8"
    ):
        violations.append(
            "integrated CMakeLists.txt still exposes the client PCH option"
        )

    return violations


def main() -> int:
    client_root = (
        Path(sys.argv[1]).resolve()
        if len(sys.argv) == 2
        else Path(__file__).resolve().parents[2]
    )
    if len(sys.argv) > 2:
        print(f"usage: {Path(sys.argv[0]).name} [CLIENT_ROOT]", file=sys.stderr)
        return 2

    violations = find_violations(client_root)
    if violations:
        print("client include ownership violations:", file=sys.stderr)
        for violation in violations:
            print(f"- {violation}", file=sys.stderr)
        return 1

    print("client include ownership: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
