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


def require_staging_error(
    executable: Path, assetspath: Path, surface: str
) -> None:
    result = run_server(executable, assetspath)
    output = result.stdout + result.stderr
    if (
        result.returncode == 0
        or "Could not inspect or create asset staging directory" not in output
    ):
        raise RuntimeError(f"uninspectable {surface} staging was not rejected: {output}")


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
        result = run_server(executable, assets)
        if result.returncode != 0:
            raise RuntimeError(
                f"existing asset staging failed: {result.stdout}{result.stderr}"
            )

        invalid_file = root / "asset-file"
        invalid_file.write_text("invalid\n", encoding="utf-8")
        require_staging_rejection(executable, invalid_file, "file")
        require_staging_error(executable, invalid_file / "child", "file child")

        target = root / "asset-target"
        target.mkdir()
        sentinel = target / "sentinel"
        sentinel.write_text("unchanged\n", encoding="utf-8")
        invalid_link = root / "asset-link"
        invalid_link.symlink_to(target, target_is_directory=True)
        require_staging_rejection(executable, invalid_link, "symlink")
        if sentinel.read_text(encoding="utf-8") != "unchanged\n":
            raise RuntimeError("symlink rejection modified its target")

        invalid_data_file = root / "asset-data-file"
        invalid_data_file.mkdir()
        (invalid_data_file / "data").write_text("invalid\n", encoding="utf-8")
        require_staging_rejection(
            executable, invalid_data_file, "nested data file"
        )

        invalid_data_link = root / "asset-data-link"
        invalid_data_link.mkdir()
        (invalid_data_link / "data").symlink_to(target, target_is_directory=True)
        require_staging_rejection(
            executable, invalid_data_link, "nested data symlink"
        )
        if sentinel.read_text(encoding="utf-8") != "unchanged\n":
            raise RuntimeError("nested data symlink rejection modified its target")

        invalid_maps_link = root / "asset-maps-link"
        invalid_maps_link.mkdir()
        (invalid_maps_link / "data").mkdir()
        (invalid_maps_link / "client-maps").symlink_to(
            target, target_is_directory=True
        )
        require_staging_rejection(
            executable, invalid_maps_link, "nested client-maps symlink"
        )
        if sentinel.read_text(encoding="utf-8") != "unchanged\n":
            raise RuntimeError("client-maps symlink rejection modified its target")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
