#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Collect an existing native curl provider before choosing a source cohort."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
CA_SHA256 = "a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def clean_environment(environment):
    """Prevent loader, build flag, proxy, and TLS environment overrides."""
    blocked = {
        "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CC", "CXX", "SDKROOT",
        "CURL_CA_BUNDLE", "SSL_CERT_FILE", "SSL_CERT_DIR", "CMAKE_PREFIX_PATH",
        "CMAKE_TOOLCHAIN_FILE", "CMAKE_GENERATOR", "CMAKE_ARGS",
        "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "PKG_CONFIG_SYSROOT_DIR",
        "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH",
        "COMPILER_PATH", "GCC_EXEC_PREFIX", "MACOSX_DEPLOYMENT_TARGET",
        "CURL_SSL_BACKEND", "OPENSSL_CONF", "OPENSSL_CONF_INCLUDE", "OPENSSL_MODULES",
        "OPENSSL_ENGINES", "SSLKEYLOGFILE",
    }
    return {key: value for key, value in environment.items()
            if key not in blocked and not key.startswith(("DYLD_", "LD_"))
            and not key.lower().endswith("_proxy")}


def validate_provider(data, selected, prefix):
    """A captured provider must agree with the selected library and headers."""
    selected = Path(selected).resolve(strict=True)
    prefix = Path(prefix).resolve(strict=True)
    if not selected.is_relative_to(prefix):
        raise ValueError("CMake selected a curl library outside the requested prefix")
    if Path(data["loaded_library"]).resolve(strict=True) != selected:
        raise ValueError("runtime curl provider differs from the CMake selected library")
    if data["header_version_num"] != data["version_num"]:
        raise ValueError("curl headers and loaded library versions differ")
    for key in ("features", "protocols"):
        if not data[key] or len(set(data[key])) != len(data[key]):
            raise ValueError(f"invalid runtime curl {key}")
    for key in ("public_tls_default", "public_tls_bundle"):
        proof = data[key]
        if proof["code"] != 0 or not 200 <= proof["status"] < 400:
            raise ValueError(f"normal verified public TLS request failed: {key}")


def validate_headers(include_directories, prefix):
    expected = (Path(prefix) / "include").resolve(strict=True)
    actual = [Path(value).resolve(strict=True) for value in include_directories.split(";")]
    if actual != [expected]:
        raise ValueError("CMake curl header directories differ from the requested prefix")


def collect(prefix, llvm_prefix, output, ca_bundle):
    if platform.system() != "Darwin":
        raise ValueError("curl baseline collection requires a native macOS host")
    prefix = prefix.resolve(strict=True)
    llvm_prefix = llvm_prefix.resolve(strict=True)
    ca_bundle = ca_bundle.resolve(strict=True)
    if digest(ca_bundle) != CA_SHA256:
        raise ValueError("Classic public CA bundle digest differs from the locked input")
    for relative in (".brew/curl.rb", "INSTALL_RECEIPT.json"):
        if not (prefix / relative).is_file():
            raise ValueError(f"required native bottle provenance absent: {relative}")
    output = output.absolute()
    output.mkdir()  # Preserve an earlier run; the output must be absent.
    build = output / "probe-build"
    env = clean_environment(os.environ)

    def run(name, arguments, check=True):
        result = subprocess.run(arguments, env=env, text=True, capture_output=True)
        (output / f"{name}.stdout").write_text(result.stdout)
        (output / f"{name}.stderr").write_text(result.stderr)
        if check and result.returncode:
            raise ValueError(f"native curl collection command failed: {name} ({result.returncode})")
        return result

    library = prefix / "lib/libcurl.dylib"
    run("configure", [
        "cmake", "-S", str(ROOT / "tools/ci/macos_curl_probe"), "-B", str(build),
        "-G", "Ninja", f"-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/llvm-macos.cmake",
        f"-DCMAKE_PROGRAM_PATH={llvm_prefix}/bin", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DCURL_ROOT={prefix}", f"-DCURL_INCLUDE_DIR={prefix}/include",
        f"-DCURL_LIBRARY_RELEASE={library}", f"-DCURL_LIBRARY_DEBUG={library}",
    ])
    headers = (build / "selected-headers.txt").read_text().strip()
    validate_headers(headers, prefix)
    run("build", ["cmake", "--build", str(build), "--parallel", "2"])
    result = run("runtime", [str(build / "macos-curl-probe"), str(ca_bundle)], check=False)
    data = json.loads(result.stdout)
    selected = (build / "selected-library.txt").read_text().strip()
    record = {
        "schema_version": 1, "source_commit": run("source", ["git", "-C", str(ROOT), "rev-parse", "HEAD"]).stdout.strip(),
        "purpose": "baseline collection; not source-built curl acceptance",
        "prefix": str(prefix), "selected_library": selected,
        "selected_library_sha256": digest(selected),
        "header_sha256": digest(prefix / "include/curl/curlver.h"),
        "curl_api_header_sha256": digest(prefix / "include/curl/curl.h"),
        "selected_header_directories": headers.split(";"),
        "ca_bundle_sha256": digest(ca_bundle), "public_url": "https://curl.se/",
        "runtime": data,
    }
    # A bottle retains its exact formula and receipt, including native TLS policy.
    # Keep raw inputs; configure output is evidence and is never executed.
    for name in ("--version", "--features", "--protocols", "--configure", "--ca"):
        run("curl-config-" + name[2:], [str(prefix / "bin/curl-config"), name])
    run("curl-version", [str(prefix / "bin/curl"), "-q", "--version"])
    run("library-dependencies", ["xcrun", "otool", "-L", selected])
    run("library-architecture", ["xcrun", "lipo", "-info", selected])
    for relative in (".brew/curl.rb", "INSTALL_RECEIPT.json"):
        path = prefix / relative
        shutil.copyfile(path, output / path.name)
        record[relative + "_sha256"] = digest(path)
    (output / "baseline.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    validate_provider(data, selected, prefix)
    if result.returncode:
        raise ValueError(f"native curl runtime probe failed ({result.returncode})")
    print(json.dumps(record, indent=2, sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--llvm-prefix", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--ca-bundle", required=True, type=Path)
    args = parser.parse_args()
    try:
        collect(args.prefix, args.llvm_prefix, args.output, args.ca_bundle)
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        parser.exit(1, f"macOS curl collection failed: {error}\n")


if __name__ == "__main__":
    main()
