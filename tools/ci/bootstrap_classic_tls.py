#!/usr/bin/env python3
"""Install the immutable devcontainer TLS recipe without changing system TLS."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import urllib.request

LOCK = Path(__file__).with_name("classic_tls.lock.json")
RECIPE_PATHS = {
    "linux/tls/build.py", "linux/tls/manifest.json",
    "tools/install_classic_shader_toolchain.py",
}
REQUIRED_FILES = (
    "include/openssl/ssl.h", "include/curl/curl.h",
    "lib/libssl.so.4", "lib/libcrypto.so.4", "lib/libcurl.so.4",
    "lib/ossl-modules/legacy.so", "ssl/openssl.cnf",
    "share/licenses/openssl/LICENSE.txt", "share/licenses/curl/COPYING",
)


class BootstrapError(RuntimeError):
    pass


def load_lock(path: Path = LOCK) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if (set(value) != {"schema_version", "repository", "revision", "prefix", "files"}
            or value["schema_version"] != 1
            or value["repository"] != "atrinik/devcontainer"
            or not re.fullmatch(r"[0-9a-f]{40}", value["revision"])
            or value["prefix"] != "/opt/atrinik/tls"
            or set(value["files"]) != RECIPE_PATHS
            or any(not re.fullmatch(r"[0-9a-f]{64}", digest)
                   for digest in value["files"].values())):
        raise BootstrapError("invalid Classic TLS producer lock")
    return value


def reject_symlink_parents(path: Path) -> None:
    for parent in (path, *path.parents):
        if parent.is_symlink():
            raise BootstrapError("TLS prefix must not traverse a symlink")


def require_empty_prefix(prefix: Path) -> None:
    reject_symlink_parents(prefix)
    if prefix.exists() and (not prefix.is_dir() or any(prefix.iterdir())):
        raise BootstrapError("refusing to overwrite or adopt a populated TLS prefix")


def verify_bytes(data: bytes, expected: str) -> None:
    if hashlib.sha256(data).hexdigest() != expected:
        raise BootstrapError("Classic TLS producer checksum mismatch")


def retrieve(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=30) as response:
        if not response.geturl().startswith("https://raw.githubusercontent.com/"):
            raise BootstrapError("unexpected TLS producer redirect")
        data = response.read(4 * 1024 * 1024 + 1)
    if len(data) > 4 * 1024 * 1024:
        raise BootstrapError("TLS producer file exceeds size bound")
    return data


def validate_prefix(prefix: Path, lock: dict) -> None:
    reject_symlink_parents(prefix)
    manifest = prefix / "share/atrinik/classic-tls.json"
    if not manifest.is_file() or manifest.is_symlink():
        raise BootstrapError("verified Classic TLS installation manifest is missing")
    verify_bytes(manifest.read_bytes(), lock["files"]["linux/tls/manifest.json"])
    for name in REQUIRED_FILES:
        target = prefix / name
        if not target.is_file() or not target.resolve().is_relative_to(prefix.resolve()):
            raise BootstrapError(f"Classic TLS installation is incomplete: {name}")


def install(lock: dict, jobs: int = 2) -> None:
    prefix = Path(lock["prefix"])
    require_empty_prefix(prefix)
    with tempfile.TemporaryDirectory(prefix="classic-tls-recipe-") as directory:
        root = Path(directory)
        for name, digest in lock["files"].items():
            url = (f"https://raw.githubusercontent.com/{lock['repository']}/"
                   f"{lock['revision']}/{name}")
            data = retrieve(url)
            verify_bytes(data, digest)
            (root / Path(name).name).write_bytes(data)
        # Verify every input before executing any downloaded producer code.
        # Only the explicitly selected Classic CMake consumers use the prefix.
        environment = dict(os.environ)
        for name in ("LD_LIBRARY_PATH", "OPENSSL_CONF", "OPENSSL_MODULES",
                     "CMAKE_PREFIX_PATH", "PKG_CONFIG_PATH", "PYTHONPATH"):
            environment.pop(name, None)
        subprocess.run([
            sys.executable, str(root / "build.py"),
            "--manifest", str(root / "manifest.json"),
            "--cache", str(root / "cache"), "--build-root", str(root / "build"),
            "--jobs", str(jobs),
        ], check=True, env=environment)
    validate_prefix(prefix, lock)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=2)
    args = parser.parse_args()
    lock = load_lock()
    if args.verify_only:
        validate_prefix(Path(lock["prefix"]), lock)
    else:
        install(lock, args.jobs)


if __name__ == "__main__":
    try:
        main()
    except (BootstrapError, OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Classic TLS bootstrap failed: {error}") from error
