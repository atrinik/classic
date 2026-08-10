#!/usr/bin/env python3
"""Verify removed asset-staging names fail with an actionable migration."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile


def require_rejection(result: subprocess.CompletedProcess[str], surface: str) -> None:
    output = result.stdout + result.stderr
    if result.returncode == 0 or "use assetspath" not in output:
        raise RuntimeError(
            f"removed {surface} was not rejected with an assetspath migration: {output}"
        )


def run_server(executable: Path, assetspath: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [executable, "--worldmaker", f"--assetspath={assetspath}"],
        capture_output=True,
        text=True,
        check=False,
        timeout=15,
    )


def require_staging_rejection(
    executable: Path, assetspath: Path, surface: str
) -> None:
    result = run_server(executable, assetspath)
    output = result.stdout + result.stderr
    if result.returncode == 0 or "not a real directory" not in output:
        raise RuntimeError(f"invalid {surface} staging was not rejected: {output}")


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

    with tempfile.TemporaryDirectory(dir=".") as temporary:
        root = Path(temporary)
        assets = root / "assets"
        result = run_server(executable, assets)
        if result.returncode != 0:
            raise RuntimeError(f"fresh asset staging failed: {result.stdout}{result.stderr}")
        data = assets / "data"
        if not (data / "listing.txt").is_file() or not any(data.glob("*.zz")):
            raise RuntimeError("fresh asset staging lacks generated core data")
        if not (assets / "client-maps").is_dir():
            raise RuntimeError("fresh asset staging lacks client-maps directory")

        invalid_file = root / "asset-file"
        invalid_file.write_text("invalid\n", encoding="utf-8")
        require_staging_rejection(executable, invalid_file, "file")

        target = root / "asset-target"
        target.mkdir()
        invalid_link = root / "asset-link"
        invalid_link.symlink_to(target, target_is_directory=True)
        require_staging_rejection(executable, invalid_link, "symlink")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
