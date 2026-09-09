#!/usr/bin/env python3
"""Verify removed asset-staging names fail with an actionable migration."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


SERVER_TIMEOUT_SECONDS = 60
MAX_ASSETSPATH_BYTES = 255
ASSETSPATH_LENGTH_DIAGNOSTIC = "--assetspath must be at most 255 bytes"


def require_rejection(result: subprocess.CompletedProcess[str], surface: str) -> None:
    output = result.stdout + result.stderr
    if result.returncode == 0 or "use assetspath" not in output:
        raise RuntimeError(
            f"removed {surface} was not rejected with an assetspath migration: {output}"
        )


def run_server(
    executable: Path,
    assetspath: str | Path,
    pass_fds: tuple[int, ...] = (),
    previous_assetspath: str | Path | None = None,
    following_assetspath: str | Path | None = None,
) -> subprocess.CompletedProcess[str]:
    arguments = [executable, "--worldmaker"]
    if previous_assetspath is not None:
        arguments.append(f"--assetspath={previous_assetspath}")
    arguments.append(f"--assetspath={assetspath}")
    if following_assetspath is not None:
        arguments.append(f"--assetspath={following_assetspath}")
    return subprocess.run(
        arguments,
        capture_output=True,
        text=True,
        check=False,
        pass_fds=pass_fds,
        timeout=SERVER_TIMEOUT_SECONDS,
    )


def path_with_encoded_length(root: Path, length: int, fill: str) -> str:
    prefix = os.fsencode(f"{root}/")
    encoded_fill = os.fsencode(fill)
    remaining = length - len(prefix)
    if remaining <= 0 or remaining % len(encoded_fill) != 0:
        raise RuntimeError(f"cannot construct {length}-byte path using {fill!r}")
    return os.fsdecode(prefix + encoded_fill * (remaining // len(encoded_fill)))


def multibyte_path_with_encoded_length(root: Path, length: int) -> str:
    prefix = os.fsencode(f"{root}/")
    remaining = length - len(prefix)
    encoded_fill = "é".encode("utf-8")
    suffix = b"m" * (remaining % len(encoded_fill))
    suffix += encoded_fill * ((remaining - len(suffix)) // len(encoded_fill))
    if not suffix.endswith(encoded_fill):
        raise RuntimeError("multibyte boundary path does not end with a full character")
    return os.fsdecode(prefix + suffix)


def require_length_rejection(
    executable: Path,
    assetspath: str,
    surface: str,
    previous_assetspath: str | Path | None = None,
    following_assetspath: str | Path | None = None,
) -> None:
    result = run_server(
        executable,
        assetspath,
        previous_assetspath=previous_assetspath,
        following_assetspath=following_assetspath,
    )
    output = result.stdout + result.stderr
    if result.returncode == 0 or ASSETSPATH_LENGTH_DIAGNOSTIC not in output:
        raise RuntimeError(f"oversized {surface} was not rejected: {output}")


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
        # Keep CLI paths short even when tempfile returns an absolute path.
        root = Path(os.path.relpath(temporary))
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

        boundary_assets = path_with_encoded_length(
            root, MAX_ASSETSPATH_BYTES, "v"
        )
        if len(os.fsencode(boundary_assets)) != MAX_ASSETSPATH_BYTES:
            raise RuntimeError("valid boundary asset path has the wrong byte length")
        result = run_server(executable, boundary_assets)
        if result.returncode != 0:
            raise RuntimeError(
                f"255-byte asset staging failed: {result.stdout}{result.stderr}"
            )
        if not (Path(boundary_assets) / "data" / "listing.txt").is_file():
            raise RuntimeError("255-byte asset staging lacks generated core data")

        oversized_assets = path_with_encoded_length(
            root, MAX_ASSETSPATH_BYTES + 1, "o"
        )
        truncated_prefix = Path(
            os.fsdecode(os.fsencode(oversized_assets)[:MAX_ASSETSPATH_BYTES])
        )
        truncated_prefix.mkdir()
        truncated_sentinel = truncated_prefix / "sentinel"
        truncated_sentinel.write_text("unchanged\n", encoding="utf-8")
        previous_assets = root / "previous-assets"
        previous_assets.mkdir()
        previous_sentinel = previous_assets / "sentinel"
        previous_sentinel.write_text("unchanged\n", encoding="utf-8")
        require_length_rejection(
            executable,
            oversized_assets,
            "256-byte path",
            previous_assetspath=previous_assets,
        )
        following_assets = root / "following-assets"
        following_assets.mkdir()
        following_sentinel = following_assets / "sentinel"
        following_sentinel.write_text("unchanged\n", encoding="utf-8")
        require_length_rejection(
            executable,
            oversized_assets,
            "256-byte path followed by a valid path",
            following_assetspath=following_assets,
        )
        if following_sentinel.read_text(encoding="utf-8") != "unchanged\n":
            raise RuntimeError("rejected asset path modified the following setting")
        for untouched in (truncated_prefix, previous_assets, following_assets):
            if set(untouched.iterdir()) != {untouched / "sentinel"}:
                raise RuntimeError(f"rejected asset path staged into {untouched}")
        if truncated_sentinel.read_text(encoding="utf-8") != "unchanged\n":
            raise RuntimeError("oversized asset path modified its truncated prefix")
        if previous_sentinel.read_text(encoding="utf-8") != "unchanged\n":
            raise RuntimeError("rejected asset path modified the previous setting")

        multibyte_assets = multibyte_path_with_encoded_length(
            root, MAX_ASSETSPATH_BYTES + 1
        )
        multibyte_prefix = os.fsencode(multibyte_assets)[:MAX_ASSETSPATH_BYTES]
        require_length_rejection(executable, multibyte_assets, "multibyte path")
        if os.path.lexists(multibyte_prefix):
            raise RuntimeError("multibyte asset path staged through a partial character")

        invalid_file = root / "asset-file"
        invalid_file.write_text("invalid\n", encoding="utf-8")
        require_staging_rejection(executable, invalid_file, "file")
        require_staging_error(executable, invalid_file / "child", "file child")

        target = root / "asset-target"
        target.mkdir()
        sentinel = target / "sentinel"
        sentinel.write_text("unchanged\n", encoding="utf-8")
        invalid_link = root / "asset-link"
        invalid_link.symlink_to(target.resolve(), target_is_directory=True)
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
        (invalid_data_link / "data").symlink_to(target.resolve(), target_is_directory=True)
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
            target.resolve(), target_is_directory=True
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
