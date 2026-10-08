#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 The Atrinik Project
"""Stage the qualified curl/c-ares payload and check destination providers.

These checks fence copied bytes and actual dynamic loader results. They do not
replace cancellation, TLS, QUIC or normal server behavioral qualification.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import ssl
import subprocess

PREFIX = PurePosixPath("/usr/local")
LOCK = Path(__file__).with_name("runtime-provider.lock.json")
PYTHON_DYNLOAD = Path("/usr/lib/python3.14/lib-dynload")
REQUIRED = {
    "lib/libcurl.so.4", "lib/libcurl.so.4.8.0",
    "lib/libcares.so.2", "lib/libcares.so.2.19.5",
    "share/licenses/curl/COPYING", "share/licenses/c-ares/LICENSE.md",
    "share/atrinik/classic-curl-toolchain.json",
    "share/atrinik/curl/curl_config.h", "share/atrinik/curl/config.log",
}


class ProviderError(ValueError):
    pass


def relative_path(path):
    absolute = PurePosixPath(path)
    if not absolute.is_absolute() or ".." in absolute.parts:
        raise ProviderError(f"Unsafe provider path: {path}")
    try:
        return absolute.relative_to(PREFIX)
    except ValueError as error:
        raise ProviderError(f"Provider path outside /usr/local: {path}") from error


def validate_lock(lock):
    if lock["schema_version"] != 1:
        raise ProviderError("Unsupported provider lock schema")
    names = [str(relative_path(row["path"])) for row in lock["files"]]
    if len(names) != len(set(names)) or not REQUIRED.issubset(names):
        raise ProviderError("Duplicate or missing required provider payload")
    allowed = REQUIRED | {"lib/libcurl.so", "lib/libcares.so"}
    if set(names) != allowed:
        raise ProviderError("Provider payload must contain only locked runtime families and evidence")
    for row in lock["files"]:
        name = relative_path(row["path"])
        if "symlink" in row:
            target = PurePosixPath(row["symlink"])
            if target.is_absolute() or len(target.parts) != 1 or target.name in (".", ".."):
                raise ProviderError(f"Non-internal provider symlink: {row['path']}")
            if str(name.parent / target) not in names:
                raise ProviderError(f"Incomplete provider symlink chain: {row['path']}")
    packages = {row["Package"] for row in lock["packages"]}
    if "libcurl4t64" in packages:
        raise ProviderError("Distro curl must not shadow the qualified provider")
    required_packages = {"libssl3t64", "openssl", "libc6", "libc-bin", "libnghttp2-14",
                         "libidn2-0", "libpsl5t64", "libzstd1", "libbrotli1", "libunistring5",
                         "zlib1g", "ca-certificates", "libgd3", "libminiupnpc21", "libcrypt1",
                         "libreadline8t64", "python3", "libpython3.14"}
    if not required_packages.issubset(packages):
        raise ProviderError("Missing direct or indirect runtime package")
    verify_package_closure(lock["package_roots"], lock["package_dependencies"], lock["packages"])
    selections = lock["package_dependency_selections"]
    if set(selections) != packages:
        raise ProviderError("Incomplete recorded dpkg dependency selections")
    for name, fields in selections.items():
        selected = {row["selected"] for field in ("Depends", "Pre-Depends") for row in fields[field]}
        if selected != set(lock["package_dependencies"][name]):
            raise ProviderError(f"Recorded dpkg dependency edge mismatch: {name}")


def verify_payload(lock, prefix):
    validate_lock(lock)
    prefix = Path(prefix)
    for row in lock["files"]:
        path = prefix / relative_path(row["path"])
        # A substituted parent must never redirect a verified/copy destination.
        parent = path.parent
        while parent != prefix:
            if parent.is_symlink():
                raise ProviderError(f"Symlinked provider ancestry: {path}")
            parent = parent.parent
        if "symlink" in row:
            if not path.is_symlink() or os.readlink(path) != row["symlink"]:
                raise ProviderError(f"Provider symlink mismatch: {path}")
            try:
                resolved = path.resolve(strict=True)
                resolved.relative_to(prefix.resolve())
            except (OSError, ValueError, RuntimeError) as error:
                raise ProviderError(f"Broken/escaping provider symlink: {path}") from error
        else:
            if path.is_symlink() or not path.is_file():
                raise ProviderError(f"Missing regular provider file: {path}")
            content = path.read_bytes()
            if hashlib.sha256(content).hexdigest() != row["sha256"] or len(content) != row["bytes"]:
                raise ProviderError(f"Provider content mismatch: {path}")
            if path.stat().st_mode & 0o777 != int(row["mode"], 8):
                raise ProviderError(f"Provider mode mismatch: {path}")
    verify_resolver(prefix)


def verify_resolver(prefix):
    contract = json.loads((prefix / "share/atrinik/classic-curl-toolchain.json").read_text())
    if (contract.get("resolver") != "c-ares" or contract.get("threaded_resolver") is not False
            or contract["cares"]["version"] != "1.34.6"
            or contract["linux"]["curl_version"] != "8.18.0"
            or contract["linux"]["openssl_version"] != "3.5.5"):
        raise ProviderError("Unqualified c-ares resolver contract")
    config = (prefix / "share/atrinik/curl/curl_config.h").read_text()
    if not re.search(r"^#define USE_ARES 1$", config, re.MULTILINE):
        raise ProviderError("curl lacks the c-ares resolver macro")
    if re.search(r"^#define USE_THREADS_(?:POSIX|WIN32) 1$", config, re.MULTILINE):
        raise ProviderError("curl enables a threaded resolver")


def stage(lock, source, output):
    verify_payload(lock, source)
    output = Path(output)
    if output.exists():
        raise ProviderError(f"Provider staging output already exists: {output}")
    output.mkdir(parents=True)
    for row in lock["files"]:
        relative = relative_path(row["path"])
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if "symlink" in row:
            destination.symlink_to(row["symlink"])
        else:
            shutil.copy2(Path(source) / relative, destination)
    verify_payload(lock, output)



def verify_package_closure(roots, dependencies, packages):
    """Require the complete recorded, installed dpkg dependency graph."""
    names = [row["Package"] for row in packages]
    if len(names) != len(set(names)):
        raise ProviderError("Duplicate runtime package")
    names = set(names)
    if not roots or not set(roots).issubset(names) or set(dependencies) != names:
        raise ProviderError("Incomplete runtime package dependency graph")
    pending = list(roots)
    reached = set()
    while pending:
        name = pending.pop()
        if name in reached:
            continue
        if name not in names:
            raise ProviderError(f"Missing indirect runtime package: {name}")
        reached.add(name)
        pending.extend(dependencies[name])
    if reached != names:
        raise ProviderError("Unrelated package outside the runtime dependency closure")


def verify_packages(lock, observed):
    validate_lock(lock)
    for row in lock["packages"]:
        if observed.get(row["Package"]) != (row["Version"], row["Architecture"]):
            raise ProviderError(f"Runtime package mismatch: {row['Package']}")
    if "libcurl4t64" in observed:
        raise ProviderError("Unexpected distro libcurl4t64 installed")


def parse_ldd(output, *, allow_dependency_free=False):
    # Pinned distro Python modules may resolve their symbols from the hosting
    # interpreter and have no DT_NEEDED entries. glibc ldd reports this exact
    # successful result, despite these files being dynamic shared objects.
    if allow_dependency_free and output.strip() == "statically linked":
        return {}
    if "not found" in output:
        raise ProviderError("Unresolved ELF dependency: " + output.strip())
    resolved = dict(re.findall(r"^\s*(\S+) => (/\S+) \(", output, re.MULTILINE))
    loader = re.search(r"^\s*(/\S*ld-linux\S*) \(", output, re.MULTILINE)
    if loader:
        resolved["loader"] = loader.group(1)
    if not resolved:
        raise ProviderError("No dynamic ELF dependency evidence")
    return resolved


def verify_elf(lock, targets):
    expected = dict(lock["curl_distro_sonames"], **{
        "libcurl.so.4": "/usr/local/lib/libcurl.so.4",
        "libcares.so.2": "/usr/local/lib/libcares.so.2",
    })
    observed_all = {}
    for target in targets:
        if not target.is_file():
            raise ProviderError(f"Missing runtime ELF target: {target}")
        try:
            output = subprocess.check_output(["ldd", str(target)], text=True)
            resolved = parse_ldd(output, allow_dependency_free=(
                target.parent == PYTHON_DYNLOAD and target.suffix == ".so"))
        except subprocess.CalledProcessError as error:
            raise ProviderError(f"Runtime ELF target {target}: ldd failed with exit {error.returncode}") from error
        except ProviderError as error:
            raise ProviderError(f"Runtime ELF target {target}: {error}") from error
        for soname, path in resolved.items():
            if soname in expected and Path(path).resolve(strict=True) != Path(expected[soname]).resolve(strict=True):
                raise ProviderError(f"Unexpected ELF provider for {soname}: {path}")
        observed_all.update(resolved)
    if not set(expected).issubset(observed_all):
        raise ProviderError("Incomplete curl/c-ares transitive ELF evidence")


def runtime_targets(server):
    server = Path(server)
    targets = [server / name for name in ("atrinik-server", "atrinik-access-status",
                                          "libplugin_arena.so", "libplugin_python.so")]
    targets += [Path("/usr/local/lib/libcurl.so.4"), Path("/usr/local/lib/libcares.so.2"),
                Path("/usr/lib/x86_64-linux-gnu/libpython3.14.so.1.0")]
    extensions = sorted(PYTHON_DYNLOAD.glob("*.so"))
    if not extensions:
        raise ProviderError("Missing Python runtime extension modules")
    return targets + extensions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("stage", "packages", "verify-packages", "verify-runtime"))
    parser.add_argument("--lock", type=Path, default=LOCK)
    parser.add_argument("--prefix", type=Path, default=Path("/usr/local"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--server", type=Path)
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text())
    validate_lock(lock)
    if args.command == "stage":
        if args.output is None:
            parser.error("stage requires --output")
        stage(lock, args.prefix, args.output)
    elif args.command == "packages":
        for row in lock["packages"]:
            print(f"{row['Package']}={row['Version']}")
    else:
        output = subprocess.check_output(["dpkg-query", "-W", "-f=${Package}\t${Version}\t${Architecture}\n"], text=True)
        observed = {name: (version, architecture) for name, version, architecture
                    in (line.split("\t") for line in output.splitlines())}
        verify_packages(lock, observed)
        if args.command == "verify-runtime":
            if args.server is None:
                parser.error("verify-runtime requires --server")
            verify_payload(lock, args.prefix)
            version = ssl.OPENSSL_VERSION.split()[1]
            if version != lock["openssl_version"]:
                raise ProviderError(f"Incorrect actual OpenSSL provider: {version}")
            verify_elf(lock, runtime_targets(args.server))


if __name__ == "__main__":
    try:
        main()
    except (ProviderError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Runtime provider validation failed: {error}") from error
