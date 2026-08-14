#!/usr/bin/env python3
"""Verify removed asset-staging names fail with an actionable migration."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


SERVER_TIMEOUT_SECONDS = 60


def require_rejection(result: subprocess.CompletedProcess[str], surface: str) -> None:
    output = result.stdout + result.stderr
    if result.returncode == 0 or "use assetspath" not in output:
        raise RuntimeError(
            f"removed {surface} was not rejected with an assetspath migration: {output}"
        )


def run_server(
    executable: Path, assetspath: Path, pass_fds: tuple[int, ...] = ()
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [executable, "--worldmaker", f"--assetspath={assetspath}"],
        capture_output=True,
        text=True,
        check=False,
        pass_fds=pass_fds,
        timeout=SERVER_TIMEOUT_SECONDS,
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


def require_descriptor_rejection(
    executable: Path, assetspath: Path, pass_fds: tuple[int, ...], surface: str
) -> None:
    result = run_server(executable, assetspath, pass_fds)
    output = result.stdout + result.stderr
    if (
        result.returncode == 0
        or "Asset staging descriptor is invalid or not a directory" not in output
    ):
        raise RuntimeError(f"invalid {surface} descriptor was not rejected: {output}")


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
                timeout=SERVER_TIMEOUT_SECONDS,
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
            timeout=SERVER_TIMEOUT_SECONDS,
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
        if (invalid_data_file / "data").read_text(encoding="utf-8") != "invalid\n":
            raise RuntimeError("nested data file rejection modified the file")

        invalid_data_link = root / "asset-data-link"
        invalid_data_link.mkdir()
        (invalid_data_link / "data").symlink_to(target, target_is_directory=True)
        require_staging_rejection(
            executable, invalid_data_link, "nested data symlink"
        )
        if sentinel.read_text(encoding="utf-8") != "unchanged\n":
            raise RuntimeError("nested data symlink rejection modified its target")

        invalid_maps_file = root / "asset-maps-file"
        invalid_maps_file.mkdir()
        (invalid_maps_file / "data").mkdir()
        invalid_maps_component = invalid_maps_file / "client-maps"
        invalid_maps_component.write_text("invalid\n", encoding="utf-8")
        require_staging_rejection(
            executable, invalid_maps_file, "nested client-maps file"
        )
        if invalid_maps_component.read_text(encoding="utf-8") != "invalid\n":
            raise RuntimeError("client-maps file rejection modified the file")

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

        if sys.platform == "linux":
            descriptor_root = root / "descriptor-root"
            descriptor_root.mkdir()
            descriptor = os.open(
                descriptor_root,
                os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
            )
            descriptor_path = Path(f"/proc/self/fd/{descriptor}")
            try:
                result = run_server(executable, descriptor_path, (descriptor,))
                if result.returncode != 0:
                    raise RuntimeError(
                        "inherited directory descriptor staging failed: "
                        f"{result.stdout}{result.stderr}"
                    )
                if not (descriptor_root / "data" / "listing.txt").is_file():
                    raise RuntimeError(
                        "inherited directory descriptor lacks generated core data"
                    )
                require_descriptor_rejection(
                    executable,
                    Path(f"/proc/self/fd/0{descriptor}"),
                    (descriptor,),
                    "noncanonical",
                )
            finally:
                os.close(descriptor)

            invalid_descriptor = os.open(
                descriptor_root,
                os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
            )
            os.close(invalid_descriptor)
            require_descriptor_rejection(
                executable,
                Path(f"/proc/self/fd/{invalid_descriptor}"),
                (),
                "closed",
            )
            descriptor_file = root / "descriptor-file"
            descriptor_file.write_text("invalid\n", encoding="utf-8")
            file_descriptor = os.open(descriptor_file, os.O_RDONLY | os.O_CLOEXEC)
            try:
                require_descriptor_rejection(
                    executable,
                    Path(f"/proc/self/fd/{file_descriptor}"),
                    (file_descriptor,),
                    "non-directory",
                )
            finally:
                os.close(file_descriptor)

            replaceable = root / "descriptor-replaced"
            replaceable.mkdir()
            replaced_descriptor = os.open(
                replaceable,
                os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
            )
            displaced = root / "descriptor-displaced"
            replaceable.rename(displaced)
            replaceable.mkdir()
            replacement_sentinel = replaceable / "sentinel"
            replacement_sentinel.write_text("unchanged\n", encoding="utf-8")
            try:
                result = run_server(
                    executable,
                    Path(f"/proc/self/fd/{replaced_descriptor}"),
                    (replaced_descriptor,),
                )
                if result.returncode != 0:
                    raise RuntimeError(
                        "replaced-path directory descriptor staging failed: "
                        f"{result.stdout}{result.stderr}"
                    )
            finally:
                os.close(replaced_descriptor)
            if not (displaced / "data" / "listing.txt").is_file():
                raise RuntimeError("descriptor staging followed the replaced path")
            if replacement_sentinel.read_text(encoding="utf-8") != "unchanged\n":
                raise RuntimeError("descriptor staging modified the replacement path")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
