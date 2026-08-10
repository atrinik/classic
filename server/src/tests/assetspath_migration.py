#!/usr/bin/env python3
"""Verify removed asset-staging names fail with an actionable migration."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys


def require_rejection(result: subprocess.CompletedProcess[str], surface: str) -> None:
    output = result.stdout + result.stderr
    if result.returncode == 0 or "use assetspath" not in output:
        raise RuntimeError(
            f"removed {surface} was not rejected with an assetspath migration: {output}"
        )


def main() -> int:
    executable = Path(sys.argv[1])
    custom = Path("server-custom.cfg")
    if custom.exists() or custom.is_symlink():
        raise RuntimeError(f"refusing to replace test runtime configuration: {custom}")

    try:
        custom.write_text("httppath = ./removed\n", encoding="utf-8")
        require_rejection(
            subprocess.run(
                [executable, "--unit"],
                capture_output=True,
                text=True,
                check=False,
                timeout=15,
            ),
            "configuration key",
        )
    finally:
        custom.unlink(missing_ok=True)

    require_rejection(
        subprocess.run(
            [executable, "--unit", "--httppath=./removed"],
            capture_output=True,
            text=True,
            check=False,
            timeout=15,
        ),
        "command-line option",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
