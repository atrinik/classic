#!/usr/bin/env python3
"""Acquire and build the pinned host tools used to compile GPU shaders."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import stat
import subprocess
import tarfile
import tempfile
from typing import BinaryIO
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOCK = ROOT / "shaders" / "toolchain.lock.json"
MAX_ARCHIVE_BYTES = 1024 * 1024 * 1024
MAX_EXPANDED_BYTES = 2 * 1024 * 1024 * 1024
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_MEMBERS = 100_000


class ToolchainError(RuntimeError):
    """The shader toolchain lock, archive, or build failed validation."""


def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ToolchainError(f"duplicate shader toolchain lock key: {key}")
        value[key] = item
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_keys(value: dict[str, object], expected: set[str], context: str) -> None:
    if set(value) != expected:
        raise ToolchainError(
            f"{context} fields must be exactly {sorted(expected)}, got {sorted(value)}"
        )


def locked_text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value or value != value.strip():
        raise ToolchainError(f"{field} must be a non-empty trimmed string")
    return value


def locked_digest(value: object, field: str) -> str:
    text = locked_text(value, field)
    if len(text) != 64 or any(character not in "0123456789abcdef" for character in text):
        raise ToolchainError(f"{field} must be a lowercase SHA-256")
    return text


def load_lock(path: Path) -> dict[str, object]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=reject_duplicate_keys
        )
    except (OSError, json.JSONDecodeError) as error:
        raise ToolchainError(f"cannot read shader toolchain lock {path}: {error}") from error
    if not isinstance(value, dict):
        raise ToolchainError("shader toolchain lock root must be an object")
    require_keys(value, {"schema_version", "dxc", "spirv_cross"}, "lock root")
    if value["schema_version"] != 1:
        raise ToolchainError("unsupported shader toolchain lock schema")
    dxc = value["dxc"]
    spirv_cross = value["spirv_cross"]
    if not isinstance(dxc, dict) or not isinstance(spirv_cross, dict):
        raise ToolchainError("shader toolchain entries must be objects")
    require_keys(
        dxc,
        {"repository", "tag", "commit", "url", "sha256", "archive_root", "files"},
        "dxc",
    )
    require_keys(
        spirv_cross,
        {"repository", "commit", "url", "sha256"},
        "spirv_cross",
    )
    if locked_text(dxc["repository"], "dxc.repository") != "microsoft/DirectXShaderCompiler":
        raise ToolchainError("dxc.repository is not the governed upstream")
    if locked_text(spirv_cross["repository"], "spirv_cross.repository") != "KhronosGroup/SPIRV-Cross":
        raise ToolchainError("spirv_cross.repository is not the governed upstream")
    for context, entry in (("dxc", dxc), ("spirv_cross", spirv_cross)):
        commit = locked_text(entry["commit"], f"{context}.commit")
        if len(commit) != 40 or any(character not in "0123456789abcdef" for character in commit):
            raise ToolchainError(f"{context}.commit must be a full lowercase Git SHA")
        locked_digest(entry["sha256"], f"{context}.sha256")
        parsed = urllib.parse.urlsplit(locked_text(entry["url"], f"{context}.url"))
        if parsed.scheme != "https" or parsed.query or parsed.fragment:
            raise ToolchainError(f"{context}.url must be a canonical HTTPS URL")
    if locked_text(dxc["tag"], "dxc.tag") != "v1.9.2607":
        raise ToolchainError("dxc.tag is not the qualified release")
    if dxc["commit"] != "0d3ee6b551b8fa768fbf825300ebab81047ef6a8":
        raise ToolchainError("dxc.commit is not the qualified release commit")
    archive_root = locked_text(dxc["archive_root"], "dxc.archive_root")
    if "/" in archive_root or archive_root in {".", ".."}:
        raise ToolchainError("dxc.archive_root must be one safe path component")
    expected_dxc_url = (
        "https://github.com/microsoft/DirectXShaderCompiler/releases/download/"
        f"{dxc['tag']}/{archive_root}.tar.gz"
    )
    if dxc["url"] != expected_dxc_url:
        raise ToolchainError("dxc.url does not match its governed release coordinate")
    expected_spirv_cross_url = (
        "https://codeload.github.com/KhronosGroup/SPIRV-Cross/tar.gz/"
        f"{spirv_cross['commit']}"
    )
    if spirv_cross["url"] != expected_spirv_cross_url:
        raise ToolchainError(
            "spirv_cross.url does not match its governed commit coordinate"
        )
    files = dxc["files"]
    if not isinstance(files, dict) or not files:
        raise ToolchainError("dxc.files must be a non-empty object")
    for name, digest in files.items():
        if not isinstance(name, str):
            raise ToolchainError("dxc.files keys must be paths")
        relative = PurePosixPath(name)
        if relative.is_absolute() or any(part in {"", ".", ".."} for part in relative.parts):
            raise ToolchainError(f"unsafe dxc.files path: {name}")
        locked_digest(digest, f"dxc.files.{name}")
    return value


def download(url: str, expected: str, cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    name = f"{expected}.tar.gz"
    destination = cache / name
    if destination.is_symlink():
        raise ToolchainError(f"shader toolchain cache entry is a symlink: {destination}")
    if destination.is_file() and destination.stat().st_size <= MAX_ARCHIVE_BYTES:
        if sha256(destination) == expected:
            print(f"shader toolchain cache hit: {name}")
            return destination
    elif destination.exists():
        raise ToolchainError(f"invalid shader toolchain cache entry: {destination}")
    request = urllib.request.Request(url, headers={"User-Agent": "Atrinik shader toolchain/1"})
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{name}.", dir=cache)
    temporary = Path(temporary_name)
    try:
        total = 0
        with os.fdopen(descriptor, "wb") as output, urllib.request.urlopen(
            request, timeout=120
        ) as response:
            final = urllib.parse.urlsplit(response.geturl())
            if final.scheme != "https":
                raise ToolchainError("shader toolchain download left HTTPS")
            while block := response.read(1024 * 1024):
                total += len(block)
                if total > MAX_ARCHIVE_BYTES:
                    raise ToolchainError("shader toolchain archive exceeds size limit")
                output.write(block)
        if sha256(temporary) != expected:
            raise ToolchainError("shader toolchain archive digest mismatch")
        temporary.replace(destination)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    print(f"shader toolchain cache miss: {name}; downloaded and verified")
    return destination


def copy_member(source: BinaryIO, destination: Path, expected_size: int) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("xb") as output:
        remaining = expected_size
        while remaining:
            block = source.read(min(1024 * 1024, remaining))
            if not block:
                raise ToolchainError(f"truncated archive member: {destination.name}")
            output.write(block)
            remaining -= len(block)


def extract_dxc(archive_path: Path, destination: Path, entry: dict[str, object]) -> None:
    root = str(entry["archive_root"])
    files = entry["files"]
    assert isinstance(files, dict)
    expected = {f"{root}/{name}": name for name in files}
    seen: set[str] = set()
    with tarfile.open(archive_path, mode="r:gz") as archive:
        for member in archive:
            name = expected.get(member.name)
            if name is None:
                continue
            if name in seen or not member.isfile() or member.size > MAX_FILE_BYTES:
                raise ToolchainError(f"invalid DXC archive member: {member.name}")
            source = archive.extractfile(member)
            if source is None:
                raise ToolchainError(f"cannot read DXC archive member: {member.name}")
            with source:
                copy_member(source, destination / name, member.size)
            output = destination / name
            output.chmod(0o755 if name == "bin/dxc" else 0o644)
            if sha256(output) != files[name]:
                raise ToolchainError(f"DXC member digest mismatch: {name}")
            seen.add(name)
    if seen != set(files):
        raise ToolchainError(f"DXC archive is missing locked members: {sorted(set(files) - seen)}")


def safe_spirv_path(name: str) -> PurePosixPath | None:
    if not name or "\0" in name or "\\" in name or ":" in name:
        raise ToolchainError(f"unsafe SPIRV-Cross archive member: {name}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise ToolchainError(f"unsafe SPIRV-Cross archive member: {name}")
    if len(path.parts) <= 1:
        return None
    return PurePosixPath(*path.parts[1:])


def extract_spirv_cross(archive_path: Path, destination: Path) -> None:
    seen: set[str] = set()
    expanded = 0
    count = 0
    with tarfile.open(archive_path, mode="r:gz") as archive:
        for member in archive:
            count += 1
            if count > MAX_MEMBERS:
                raise ToolchainError("SPIRV-Cross archive has too many members")
            relative = safe_spirv_path(member.name)
            if relative is None:
                continue
            key = relative.as_posix().casefold()
            if key in seen:
                raise ToolchainError(f"duplicate SPIRV-Cross archive path: {relative}")
            seen.add(key)
            output = destination.joinpath(*relative.parts)
            if member.isdir():
                output.mkdir(parents=True, exist_ok=True)
                continue
            if not member.isfile() or member.size > MAX_FILE_BYTES:
                raise ToolchainError(f"unsupported SPIRV-Cross archive member: {member.name}")
            expanded += member.size
            if expanded > MAX_EXPANDED_BYTES:
                raise ToolchainError("SPIRV-Cross archive exceeds expanded size limit")
            source = archive.extractfile(member)
            if source is None:
                raise ToolchainError(f"cannot read SPIRV-Cross archive member: {member.name}")
            with source:
                copy_member(source, output, member.size)
            output.chmod(stat.S_IMODE(member.mode) & 0o755 or 0o644)
    if not (destination / "CMakeLists.txt").is_file():
        raise ToolchainError("SPIRV-Cross archive has no source root")


def valid_install(output: Path, lock_digest: str, dxc: dict[str, object]) -> bool:
    marker = output / "toolchain.json"
    try:
        state = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    expected_state_keys = {
        "schema_version",
        "lock_sha256",
        "spirv_cross_sha256",
    }
    if (
        not isinstance(state, dict)
        or set(state) != expected_state_keys
        or state["schema_version"] != 1
        or state["lock_sha256"] != lock_digest
    ):
        return False
    files = dxc["files"]
    assert isinstance(files, dict)
    for name, expected in files.items():
        path = output / "dxc" / name
        if (
            not path.is_file()
            or path.is_symlink()
            or sha256(path) != expected
            or (name == "bin/dxc" and not os.access(path, os.X_OK))
        ):
            return False
    spirv_cross = output / "spirv-cross" / "bin" / "spirv-cross"
    license_path = output / "spirv-cross" / "LICENSE"
    return (
        spirv_cross.is_file()
        and not spirv_cross.is_symlink()
        and os.access(spirv_cross, os.X_OK)
        and sha256(spirv_cross) == state["spirv_cross_sha256"]
        and license_path.is_file()
        and not license_path.is_symlink()
    )


def prepare(lock_path: Path, cache: Path, output: Path) -> None:
    lock_path = lock_path.resolve(strict=True)
    lock = load_lock(lock_path)
    lock_digest = sha256(lock_path)
    dxc = lock["dxc"]
    spirv_cross = lock["spirv_cross"]
    assert isinstance(dxc, dict) and isinstance(spirv_cross, dict)
    if output.exists():
        if (
            output.is_dir()
            and not output.is_symlink()
            and valid_install(output, lock_digest, dxc)
        ):
            print(f"shader toolchain current: {output}")
            return
        raise ToolchainError(f"refusing to replace invalid shader toolchain output: {output}")
    dxc_archive = download(str(dxc["url"]), str(dxc["sha256"]), cache)
    spirv_archive = download(
        str(spirv_cross["url"]), str(spirv_cross["sha256"]), cache
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent))
    try:
        extract_dxc(dxc_archive, staging / "dxc", dxc)
        source = staging / "spirv-cross-source"
        build = staging / "spirv-cross-build"
        extract_spirv_cross(spirv_archive, source)
        subprocess.run(
            [
                "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", "-DSPIRV_CROSS_CLI=ON",
                "-DSPIRV_CROSS_ENABLE_TESTS=OFF",
                "-DGIT_EXECUTABLE=GIT_EXECUTABLE-NOTFOUND",
            ],
            check=True,
        )
        subprocess.run(
            ["cmake", "--build", str(build), "--target", "spirv-cross", "--parallel"],
            check=True,
        )
        destination = staging / "spirv-cross" / "bin" / "spirv-cross"
        destination.parent.mkdir(parents=True)
        shutil.copyfile(build / "spirv-cross", destination)
        destination.chmod(0o755)
        shutil.copyfile(source / "LICENSE", staging / "spirv-cross" / "LICENSE")
        spirv_cross_digest = sha256(destination)
        shutil.rmtree(source)
        shutil.rmtree(build)
        (staging / "toolchain.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "lock_sha256": lock_digest,
                    "spirv_cross_sha256": spirv_cross_digest,
                },
                sort_keys=True,
                separators=(",", ":"),
            ) + "\n",
            encoding="utf-8",
        )
        staging.replace(output)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    print(f"shader toolchain prepared: {output}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() not in {
        "x86_64",
        "AMD64",
    }:
        parser.error(
            "the pinned downloadable shader toolchain targets x86-64 Linux; "
            "use system dxc and spirv-cross or a validated external cohort"
        )
    try:
        prepare(arguments.lock, arguments.cache.resolve(), arguments.output.resolve())
    except (OSError, subprocess.CalledProcessError, ToolchainError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
