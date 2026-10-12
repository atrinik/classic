#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the measured native Mac curl features with a qualified c-ares resolver."""

import argparse
import contextlib
import http.server
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import ssl
import subprocess
import tarfile
import threading

from collect_macos_curl import (SYSTEM_CURL, canonical_image, clean_environment, digest,
                               platform_dependency_chain, platform_image, qualification_identity,
                               validate_headers, validate_inventory, validate_provider,
                               validate_symbol_providers)

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path(__file__).with_name("macos-curl-toolchain.json")


def native_environment(environment):
    blocked = {
        "CONFIG_SITE", "CONFIG_SHELL", "BASH_ENV", "ENV", "MAKEFLAGS", "MFLAGS",
        "GNUMAKEFLAGS", "MAKELEVEL", "MAKEOVERRIDES", "LIBS", "CPP", "AR", "ARFLAGS",
        "RANLIB", "RANLIBFLAGS", "NM", "LD", "AS", "OBJCOPY", "OBJDUMP", "STRIP",
        "PKG_CONFIG", "PKGCONFIG", "KRB5CONFIG", "PKG_CONFIG_SYSTEM_INCLUDE_PATH", "PKG_CONFIG_SYSTEM_LIBRARY_PATH",
        "CCC_OVERRIDE_OPTIONS", "CCC_ADD_ARGS",
    }
    result = {key: value for key, value in clean_environment(environment).items()
              if key not in blocked and "_cv_" not in key
              and not key.startswith(("CMAKE_", "CCACHE_", "DISTCC_"))}
    # Disable both environment-selected and automatically discovered site scripts.
    result["CONFIG_SITE"] = "/dev/null"
    result["CONFIG_SHELL"] = "/bin/sh"
    return result


def load_manifest():
    data = json.loads(MANIFEST.read_text())
    if data["schema_version"] != 1 or data["llvm_version"] != "23.1.3":
        raise ValueError("unsupported native curl toolchain manifest")
    for source in data["sources"].values():
        if not re.fullmatch(r"[0-9a-f]{64}", source["sha256"]):
            raise ValueError("source archive digest is not SHA256")
    return data


def validate_baseline(data, manifest):
    runtime = data["runtime"]
    validate_inventory(runtime)
    expected = manifest["baseline"]
    for key in ("features", "protocols"):
        if set(runtime[key]) != set(expected[key]) or len(runtime[key]) != len(set(runtime[key])):
            raise ValueError(f"native baseline {key} changed; policy review required")
    for key, version in expected["versions"].items():
        if runtime[key] != version:
            raise ValueError(f"native baseline {key} changed; policy review required")
    if runtime["ares"] is not None:
        raise ValueError("unexpected baseline resolver provider")
    for key in ("public_tls_default", "public_tls_bundle"):
        proof = runtime[key]
        if (proof["code"] != 0 or proof["status"] != 200 or proof["ssl_verify_result"] != 0
                or proof["default_ca_bundle"] is not None or proof["default_ca_path"] is not None):
            raise ValueError("native baseline trust policy changed")
    if data["ca_bundle_sha256"] != manifest["ca_bundle_sha256"]:
        raise ValueError("baseline CA bundle differs from the locked Classic input")


def extract_source(archive, destination, source):
    if digest(archive) != source["sha256"]:
        raise ValueError("source archive checksum mismatch")
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():
            parts = Path(member.name).parts
            if not parts or parts[0] != source["directory"] or ".." in parts:
                raise ValueError("source archive contains an unexpected or escaping path")
        bundle.extractall(destination, filter="data")
    return Path(destination) / source["directory"]


def validate_config(text, manifest):
    macros = dict(re.findall(r"^#define\s+(\w+)\s+([^\n]+)$", text, re.M))
    for name in manifest["required_config"]:
        if macros.get(name) != "1":
            raise ValueError(f"required native curl configuration absent: {name}")
    for name in manifest["forbidden_config"]:
        if name in macros:
            raise ValueError(f"forbidden native curl configuration: {name}")


def configure_arguments(prefix, sdk, dependencies):
    def dep(name):
        return str(dependencies[name])
    return [
        f"--prefix={prefix}", f"--libdir={prefix}/lib", "--disable-silent-rules",
        "--disable-debug", "--disable-dependency-tracking", "--enable-shared", "--disable-static",
        f"--enable-ares={prefix}", "--disable-threaded-resolver",
        f"--with-ssl={dep('openssl@3')}", "--with-default-ssl-backend=openssl",
        "--without-ca-bundle", "--without-ca-path", "--with-ca-fallback",
        "--with-apple-sectrust", "--with-gssapi", "--with-apple-idn", "--without-libidn2",
        f"--with-zlib={sdk}/usr", f"--with-brotli={dep('brotli')}",
        f"--with-zstd={dep('zstd')}", f"--with-libpsl={dep('libpsl')}",
        f"--with-libssh2={dep('libssh2')}", f"--with-nghttp2={dep('libnghttp2')}",
        f"--with-nghttp3={dep('libnghttp3')}", f"--with-ngtcp2={dep('libngtcp2')}",
        "--enable-ldap", "--enable-ldaps", "--enable-ipv6",
    ]


def validate_runtime(data, manifest):
    for key in ("features", "protocols"):
        missing = set(manifest["baseline"][key]) - set(data[key])
        if missing:
            raise ValueError(f"native curl lost {key}: {', '.join(sorted(missing))}")
        if len(set(data[key])) != len(data[key]):
            raise ValueError(f"duplicate native curl {key}")
    if "asyn-rr" in data["features"]:
        raise ValueError("hybrid threaded resolver is not qualified for cancellation")
    for key, value in manifest["baseline"]["versions"].items():
        if data[key] != value:
            raise ValueError(f"native curl dependency identity changed: {key}")
    if data["header_version"] != manifest["sources"]["curl"]["version"]:
        raise ValueError("native curl header version differs from its source")
    if data["header_version_num"] != data["version_num"]:
        raise ValueError("native curl header/runtime version mismatch")
    cares = manifest["sources"]["cares"]["version"]
    if data["ares"] != cares or data["cares_version"] != cares or data["cares_header_version"] != cares:
        raise ValueError("native curl did not load the qualified c-ares version")
    for key in ("public_tls_default", "public_tls_bundle", "private_tls_trusted"):
        proof = data[key]
        if proof["code"] != 0 or proof["status"] != 200 or proof["ssl_verify_result"] != 0:
            raise ValueError(f"verified native TLS request failed: {key}")
    for key in ("public_tls_unrelated", "private_tls_untrusted", "private_tls_wrong_hostname"):
        if data[key]["code"] != 60 or data[key]["status"] != 0:
            raise ValueError(f"native TLS rejection did not establish certificate verification: {key}")
    for key in ("public_tls_default", "public_tls_bundle"):
        if data[key]["default_ca_bundle"] is not None or data[key]["default_ca_path"] is not None:
            raise ValueError("native curl default trust paths changed")


def validate_loaded(data, prefix, dependency_files, probe, required_platform_images=(),
                    baseline=None, platform_roots=()):
    prefix = Path(prefix).resolve(strict=True)
    own = {name: (prefix / "lib" / f"lib{name}.dylib").resolve(strict=True)
           for name in ("curl", "cares")}
    for name, key in (("curl", "loaded_library"), ("cares", "loaded_cares_library")):
        if not own[name].is_relative_to(prefix) or Path(data[key]).resolve(strict=True) != own[name]:
            raise ValueError(f"loaded {name} provider is outside the owned cohort")
    validate_inventory(data)
    validate_symbol_providers(data, own["curl"], own["cares"])
    resolver_edges = data["dependency_install_names"][str(own["curl"])]
    if not any(canonical_image(name) and not platform_image(name)
               and Path(name).resolve(strict=True) == own["cares"] for name in resolver_edges):
        raise ValueError("owned curl lacks its owned c-ares dependency edge")
    images = data["loaded_images"]
    if not images or len(set(images)) != len(images):
        raise ValueError("invalid native loaded-image inventory")
    missing_platform = set(required_platform_images) - set(images)
    if missing_platform:
        raise ValueError("baseline SDK providers absent: " + ", ".join(sorted(missing_platform)))
    observed = set()
    allowed_system_curl = False
    if SYSTEM_CURL in images:
        if baseline is None:
            raise ValueError("system curl coexistence requires the same-run native baseline")
        chain = platform_dependency_chain(baseline, platform_roots)
        platform_dependency_chain(data, platform_roots)
        edges = data["dependency_install_names"]
        if (chain[0] not in edges[data["loaded_library"]]
                or any(parent not in edges or child not in edges[parent] for parent, child in zip(chain, chain[1:]))):
            raise ValueError("candidate did not preserve the baseline platform curl dependency chain")
        allowed_system_curl = True
    for name in images:
        path = Path(name)
        if not path.is_absolute() or ".." in path.parts:
            raise ValueError("invalid native loaded-image path")
        if path.name.startswith(("libcurl", "libcares")):
            if name == SYSTEM_CURL and allowed_system_curl:
                continue
            if (name.startswith(("/usr/lib/", "/System/Library/", "/Library/Apple/System/Library/"))
                    or path.resolve(strict=True) not in own.values()):
                raise ValueError("additional curl/c-ares provider loaded outside the owned cohort")
        # These are OS-owned install names, often resident only in dyld's cache.
        if name.startswith(("/usr/lib/", "/System/Library/", "/Library/Apple/System/Library/")):
            continue
        resolved = path.resolve(strict=True)
        observed.add(resolved)
        if resolved in {*own.values(), Path(probe).resolve(strict=True)}:
            continue
        if str(resolved) not in dependency_files or digest(resolved) != dependency_files[str(resolved)]:
            raise ValueError(f"unrecorded native dependency loaded: {name}")
    if not set(own.values()) <= observed:
        raise ValueError("owned curl/c-ares missing from loaded-image inventory")


def bind_baseline(record, source_commit, environment):
    if (record["schema_version"] != 2 or record["source_commit"] != source_commit
            or record["qualification_identity"] != qualification_identity(environment)):
        raise ValueError("native baseline belongs to a different source or workflow execution")
    if digest(record["selected_library"]) != record["selected_library_sha256"]:
        raise ValueError("baseline selected curl bytes changed since collection")
    validate_provider(record["runtime"], record["selected_library"], record["prefix"])


def cmake_cache(path):
    values = {}
    for line in Path(path).read_text().splitlines():
        if line and not line.startswith(("#", "//")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def retain_diagnostics(prefix):
    """Copy only public build diagnostics, including unsuccessful configuration."""
    prefix = Path(prefix)
    for directory in ("llvm-probe", "cares-build", "curl-build", "curl-probe"):
        build = prefix / "stage" / directory
        for name in ("CMakeCache.txt", "compile_commands.json", "config.log", "config.status",
                     "lib/curl_config.h", "CMakeFiles/CMakeConfigureLog.yaml"):
            source = build / name
            if source.is_file():
                destination = prefix / "evidence/configuration" / directory / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)


class Commands:
    def __init__(self, evidence, environment):
        self.evidence = evidence
        self.environment = environment
        self.records = []

    def run(self, name, arguments, *, cwd=None, environment=None, check=True):
        result = subprocess.run([str(arg) for arg in arguments], cwd=cwd,
                                env=environment or self.environment, text=True, capture_output=True)
        (self.evidence / f"{name}.stdout").write_text(result.stdout)
        (self.evidence / f"{name}.stderr").write_text(result.stderr)
        self.records.append({"name": name, "arguments": [str(arg) for arg in arguments],
                             "cwd": str(cwd) if cwd else None, "returncode": result.returncode})
        (self.evidence / "commands.json").write_text(json.dumps(self.records, indent=2) + "\n")
        if check and result.returncode:
            raise ValueError(f"native curl command failed: {name} ({result.returncode})")
        return result


def capture_dependencies(commands, manifest):
    inventory = json.loads(commands.run("brew-inventory", ["brew", "info", "--json=v2", "--installed"]).stdout)
    formulae = {entry["name"]: entry for entry in inventory["formulae"]}
    pending = list(manifest["dependency_formulae"])
    dependencies, files = {}, {}
    while pending:
        name = pending.pop()
        if name in dependencies:
            continue
        entry = formulae[name]
        if len(entry["installed"]) != 1:
            raise ValueError(f"ambiguous installed native dependency: {name}")
        prefix = Path(commands.run("brew-prefix-" + name.replace("@", "-"),
                                  ["brew", "--prefix", name]).stdout.strip()).resolve(strict=True)
        dependencies[name] = prefix
        pending.extend(dep["full_name"].split("/")[-1]
                       for dep in entry["installed"][0].get("runtime_dependencies", []))
        for file in (prefix / "lib").rglob("*.dylib"):
            resolved = file.resolve(strict=True)
            if not resolved.is_relative_to(prefix):
                raise ValueError(f"native dependency escapes its recorded keg: {file}")
            files[str(resolved)] = digest(resolved)
    return dependencies, files, inventory


def private_tls_context(cert, key):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(cert, key)
    return context


@contextlib.contextmanager
def private_tls(commands, directory, openssl):
    """Own loopback fixture; private keys never enter the public evidence directory."""
    directory.mkdir(mode=0o700)
    config = directory / "openssl.cnf"
    config.write_text("[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n"
                      "[dn]\nCN=Classic native curl fixture CA\n[ext]\n"
                      "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n")
    cert, key = directory / "trusted.pem", directory / "private.key"
    commands.run("fixture-trusted-ca", [openssl, "req", "-x509", "-newkey", "rsa:2048", "-sha256",
                 "-nodes", "-days", "1", "-config", config, "-keyout", key, "-out", cert])
    key.chmod(0o600)
    unrelated = directory / "unrelated.pem"
    unrelated_key = directory / "unrelated.key"
    commands.run("fixture-unrelated-ca", [openssl, "req", "-x509", "-newkey", "rsa:2048", "-sha256",
                 "-nodes", "-days", "1", "-config", config, "-subj", "/CN=Unrelated native curl fixture CA",
                 "-keyout", unrelated_key, "-out", unrelated])
    unrelated_key.chmod(0o600)
    server_key = directory / "server.key"
    server_request = directory / "server.csr"
    server_cert = directory / "server.pem"
    server_extensions = directory / "server.cnf"
    server_extensions.write_text("[server]\nbasicConstraints=critical,CA:FALSE\n"
                                 "keyUsage=critical,digitalSignature,keyEncipherment\n"
                                 "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost\n")
    commands.run("fixture-server-request", [openssl, "req", "-new", "-newkey", "rsa:2048", "-sha256",
                 "-nodes", "-subj", "/CN=localhost", "-keyout", server_key, "-out", server_request])
    server_key.chmod(0o600)
    commands.run("fixture-server-sign", [openssl, "x509", "-req", "-in", server_request,
                 "-CA", cert, "-CAkey", key, "-CAcreateserial", "-days", "1", "-sha256",
                 "-extfile", server_extensions, "-extensions", "server", "-out", server_cert])
    # Store public certificates only; commands record paths, never key bytes.
    shutil.copyfile(cert, commands.evidence / "private-ca-public.pem")
    shutil.copyfile(unrelated, commands.evidence / "unrelated-ca-public.pem")
    shutil.copyfile(server_cert, commands.evidence / "server-public.pem")

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = b"native TLS fixture\n"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    context = private_tls_context(server_cert, server_key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield cert, unrelated, server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def prepare(prefix, llvm_prefix, baseline_path, ca_bundle, jobs=2):
    manifest = load_manifest()
    if platform.system() != "Darwin" or platform.machine() != manifest["architecture"]:
        raise ValueError("native curl cohort requires the qualified ARM64 macOS host")
    if jobs not in (1, 2):
        raise ValueError("native dependency jobs must be one or two")
    if not prefix.is_absolute() or prefix.exists() or prefix.is_symlink():
        raise ValueError("native curl install prefix must be absolute and absent")
    llvm_prefix = llvm_prefix.resolve(strict=True)
    ca_bundle = ca_bundle.resolve(strict=True)
    if digest(ca_bundle) != manifest["ca_bundle_sha256"]:
        raise ValueError("Classic CA bundle does not match its locked digest")
    baseline = json.loads(baseline_path.read_text())
    validate_baseline(baseline, manifest)
    prefix.mkdir(mode=0o755)
    try:
        _prepare_owned(prefix, llvm_prefix, baseline_path, ca_bundle, jobs, manifest)
    finally:
        retain_diagnostics(prefix)


def _prepare_owned(prefix, llvm_prefix, baseline_path, ca_bundle, jobs, manifest):
    stage, evidence = prefix / "stage", prefix / "evidence"
    stage.mkdir()
    evidence.mkdir()
    environment = native_environment(os.environ)
    commands = Commands(evidence, environment)
    source_commit = commands.run("source", ["git", "-C", ROOT, "rev-parse", "HEAD"]).stdout.strip()
    baseline = json.loads(baseline_path.read_text())
    bind_baseline(baseline, source_commit, environment)
    sdk = Path(commands.run("sdk-path", ["xcrun", "--sdk", "macosx", "--show-sdk-path"]).stdout.strip()).resolve(strict=True)
    commands.run("sdk-version", ["xcrun", "--sdk", "macosx", "--show-sdk-version"])
    commands.run("host-version", ["sw_vers"])
    tools = {}
    for name in ("clang", "clang++", "llvm-ar", "llvm-ranlib", "llvm-nm", "llvm-strip", "ld64.lld"):
        path = llvm_prefix / "bin" / name
        if not path.resolve(strict=True).is_relative_to(llvm_prefix):
            raise ValueError("LLVM tool escapes the selected cohort")
        result = commands.run("tool-" + name.replace("+", "x"), [path, "--version"])
        if "Apple clang" in result.stdout or not re.search(r"(?:clang|LLVM|LLD) version 23\.1\.3\b|LLD 23\.1\.3\b", result.stdout):
            raise ValueError(f"native dependency tool is not LLVM 23.1.3: {name}")
        tools[name] = {"path": str(path), "sha256": digest(path), "version": result.stdout.strip()}
    dependencies, dependency_files, inventory = capture_dependencies(commands, manifest)
    sources = {}
    retained_sources = prefix / "share/atrinik/macos-curl/sources"
    retained_sources.mkdir(parents=True)
    for name, source in manifest["sources"].items():
        archive = retained_sources / source["archive"]
        commands.run("download-" + name, ["/usr/bin/curl", "-q", "--fail", "--location", "--retry", "3",
                     "--connect-timeout", "15", "--max-time", "120", "--proxy", "", source["url"], "--output", archive])
        sources[name] = extract_source(archive, stage, source)
        notices = prefix / "share/licenses" / name
        notices.mkdir(parents=True)
        shutil.copyfile(sources[name] / source["notice"], notices / source["notice"])
    shutil.copyfile(MANIFEST, prefix / "share/atrinik/macos-curl/toolchain.json")
    shutil.copyfile(baseline_path, evidence / "baseline.json")
    # Native SDK/runtime/tool validation and optimization come from the same
    # CMake toolchain as Classic. No application target is built by this helper.
    cmake_arguments = [f"-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/llvm-macos.cmake",
                       f"-DCMAKE_PROGRAM_PATH={llvm_prefix}/bin", f"-DCMAKE_OSX_SYSROOT={sdk}",
                       "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_BUILD_TYPE=Release",
                       "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    native_probe = stage / "llvm-probe"
    commands.run("llvm-configure", ["cmake", "-S", ROOT / "tools/ci/macos_llvm_probe", "-B", native_probe,
                                   "-G", "Ninja", *cmake_arguments])
    commands.run("llvm-build", ["cmake", "--build", native_probe, "--parallel", str(jobs)])
    commands.run("llvm-c-run", [native_probe / "llvm-c-probe"])
    commands.run("llvm-cxx-run", [native_probe / "llvm-cxx-probe"])
    cache = cmake_cache(native_probe / "CMakeCache.txt")
    release_flags = shlex.split(cache["CMAKE_C_FLAGS_RELEASE"])
    if release_flags != ["-O3", "-DNDEBUG"]:
        raise ValueError("native Release optimization changed; policy review required")
    deployment = cache.get("CMAKE_OSX_DEPLOYMENT_TARGET", "")
    if deployment:
        cmake_arguments.append(f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment}")
    cares_build = stage / "cares-build"
    commands.run("cares-configure", ["cmake", "-S", sources["cares"], "-B", cares_build, "-G", "Ninja",
                 *cmake_arguments, f"-DCMAKE_INSTALL_PREFIX={prefix}", f"-DCMAKE_INSTALL_NAME_DIR={prefix}/lib",
                 "-DCARES_SHARED=ON", "-DCARES_STATIC=OFF", "-DCARES_BUILD_TESTS=OFF"])
    commands.run("cares-build", ["cmake", "--build", cares_build, "--parallel", str(jobs)])
    commands.run("cares-install", ["cmake", "--install", cares_build])
    build_environment = dict(environment)
    sdk_flags = ["-isysroot", str(sdk), "-arch", "arm64"]
    if deployment:
        sdk_flags.append(f"-mmacosx-version-min={deployment}")
    for key, name in {"CC": "clang", "CXX": "clang++", "AR": "llvm-ar", "RANLIB": "llvm-ranlib",
                      "NM": "llvm-nm", "STRIP": "llvm-strip", "LD": "ld64.lld"}.items():
        build_environment[key] = shlex.quote(tools[name]["path"])
    build_environment["CFLAGS"] = shlex.join([*release_flags, *sdk_flags])
    build_environment["CXXFLAGS"] = shlex.join([*release_flags, *sdk_flags])
    build_environment["LDFLAGS"] = shlex.join([
        *sdk_flags, "--rtlib=compiler-rt", f"-fuse-ld={tools['ld64.lld']['path']}",
        f"-L{prefix}/lib", f"-Wl,-rpath,{prefix}/lib",
    ])
    # Exclude installed Brew curl, c-ares, zlib and libidn2 from discovery.
    pc_dirs = [prefix / "lib/pkgconfig", *[dependencies[name] / "lib/pkgconfig"
               for name in manifest["dependency_formulae"]]]
    build_environment["PKG_CONFIG_LIBDIR"] = os.pathsep.join(str(path) for path in pc_dirs)
    build_environment["PKG_CONFIG_PATH"] = build_environment["PKG_CONFIG_LIBDIR"]
    (evidence / "build-environment.json").write_text(json.dumps({key: build_environment[key]
        for key in ("CC", "CXX", "AR", "RANLIB", "NM", "STRIP", "LD", "CFLAGS", "CXXFLAGS", "LDFLAGS",
                    "PKG_CONFIG_LIBDIR", "PKG_CONFIG_PATH")}, indent=2) + "\n")
    curl_build = stage / "curl-build"
    curl_build.mkdir()
    commands.run("curl-configure", [sources["curl"] / "configure",
                 *configure_arguments(prefix, sdk, dependencies)], cwd=curl_build, environment=build_environment)
    validate_config((curl_build / "lib/curl_config.h").read_text(), manifest)
    commands.run("curl-build", ["make", f"-j{jobs}", "V=1"], cwd=curl_build, environment=build_environment)
    commands.run("curl-install", ["make", "install"], cwd=curl_build, environment=build_environment)
    shutil.copyfile(curl_build / "lib/curl_config.h", evidence / "curl_config.h")
    probe_build = stage / "curl-probe"
    commands.run("probe-configure", ["cmake", "-S", ROOT / "tools/ci/macos_curl_probe", "-B", probe_build,
                 "-G", "Ninja", *cmake_arguments, "-DATRINIK_CURL_COHORT=ON", f"-DCURL_ROOT={prefix}",
                 f"-DCURL_INCLUDE_DIR={prefix}/include", f"-DCURL_LIBRARY_RELEASE={prefix}/lib/libcurl.dylib",
                 f"-DCURL_LIBRARY_DEBUG={prefix}/lib/libcurl.dylib", f"-Dc-ares_DIR={prefix}/lib/cmake/c-ares"])
    validate_headers((probe_build / "selected-headers.txt").read_text().strip(), prefix)
    validate_headers((probe_build / "selected-cares-headers.txt").read_text().strip(), prefix)
    for name, file in (("curl", "selected-library.txt"), ("cares", "selected-cares-library.txt")):
        selected = Path((probe_build / file).read_text().strip()).resolve(strict=True)
        if selected != (prefix / f"lib/lib{name}.dylib").resolve(strict=True):
            raise ValueError(f"probe selected a foreign {name} provider")
    commands.run("probe-build", ["cmake", "--build", probe_build, "--parallel", str(jobs)])
    probe = probe_build / "macos-curl-probe"
    with private_tls(commands, stage / "private-tls", dependencies["openssl@3"] / "bin/openssl") as (trusted, unrelated, port):
        result = commands.run("runtime", [probe, ca_bundle, unrelated, trusted, str(port)], check=False)
    data = json.loads(result.stdout)
    (evidence / "runtime.json").write_text(json.dumps(data, indent=2) + "\n")
    validate_runtime(data, manifest)
    validate_loaded(data, prefix, dependency_files, probe, manifest["required_platform_images"],
                    baseline["runtime"], manifest["baseline_platform_roots"])
    if result.returncode:
        raise ValueError(f"native curl runtime probe failed ({result.returncode})")
    for name in ("curl", "cares"):
        library = (prefix / f"lib/lib{name}.dylib").resolve(strict=True)
        identity = commands.run(name + "-install-name", ["xcrun", "otool", "-D", library]).stdout.splitlines()
        if len(identity) != 2 or Path(identity[1]).resolve(strict=True) != library:
            raise ValueError(f"{name} dylib install name is not the owned absolute provider")
        commands.run(name + "-load-commands", ["xcrun", "otool", "-l", library])
        commands.run(name + "-dependencies", ["xcrun", "otool", "-L", library])
        architecture = commands.run(name + "-architecture", ["xcrun", "lipo", "-archs", library]).stdout.strip()
        if architecture != "arm64":
            raise ValueError(f"{name} dylib architecture differs from the native cohort")
    receipt = {"schema_version": 1, "scope": manifest["scope"], "prefix": str(prefix),
               "sources": manifest["sources"], "baseline_provenance": manifest["baseline"],
               "source_commit": source_commit, "qualification_identity": qualification_identity(environment),
               "baseline_sha256": digest(baseline_path),
               "platform_curl_chain": (platform_dependency_chain(baseline["runtime"], manifest["baseline_platform_roots"])
                                       if SYSTEM_CURL in data["loaded_images"] else None),
               "tools": tools, "sdk": str(sdk), "architecture": "arm64", "deployment_target": deployment,
               "release_flags": release_flags, "dependency_prefixes": {key: str(value) for key, value in dependencies.items()},
               "dependency_files": dependency_files, "runtime": data,
               "curl_sha256": digest(prefix / "lib/libcurl.dylib"), "cares_sha256": digest(prefix / "lib/libcares.dylib"),
               "header_hashes": {name: digest(prefix / "include" / name)
                   for name in ("curl/curl.h", "curl/curlver.h", "ares.h", "ares_version.h")},
               "manifest_sha256": digest(MANIFEST),
               "consumer_cancellation_verified": False, "consumer_cpu_acceptance_verified": False,
               "sectrust_execution_verified": False}
    (evidence / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


def verify_consumer_cache(prefix, cache_path):
    """Prove consumer selection; unchanged CTest remains the cancellation gate."""
    if platform.system() != "Darwin":
        raise ValueError("consumer selection requires the actual native Mac filesystem")
    prefix = prefix.resolve(strict=True)
    receipt = json.loads((prefix / "evidence/receipt.json").read_text())
    cache = cmake_cache(cache_path)
    if Path(receipt["prefix"]).resolve(strict=True) != prefix:
        raise ValueError("cohort receipt belongs to another install prefix")
    for key, expected in (("CURL_INCLUDE_DIR", prefix / "include"),
                          ("CURL_LIBRARY_RELEASE", prefix / "lib/libcurl.dylib")):
        if not cache.get(key) or Path(cache[key]).resolve(strict=True) != expected.resolve(strict=True):
            raise ValueError(f"consumer selected a foreign curl provider: {key}")
    for name in ("curl", "cares"):
        if digest(prefix / f"lib/lib{name}.dylib") != receipt[name + "_sha256"]:
            raise ValueError(f"installed {name} bytes changed since native verification")
    for name, expected in receipt["header_hashes"].items():
        if digest(prefix / "include" / name) != expected:
            raise ValueError("installed curl/c-ares headers changed since verification")
    tool = receipt["tools"]["clang"]
    if (Path(cache["CMAKE_C_COMPILER"]).resolve(strict=True) != Path(tool["path"]).resolve(strict=True)
            or digest(tool["path"]) != tool["sha256"]):
        raise ValueError("consumer compiler differs from the verified LLVM cohort")
    if cache["CMAKE_BUILD_TYPE"] != "Release" or shlex.split(cache["CMAKE_C_FLAGS_RELEASE"]) != receipt["release_flags"]:
        raise ValueError("consumer Release optimization differs from the qualified cohort")
    print("consumer curl headers/library/compiler match the verified native cohort; CTest still required")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--llvm-prefix", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--ca-bundle", type=Path)
    parser.add_argument("--verify-consumer-cache", type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    try:
        if args.verify_consumer_cache:
            verify_consumer_cache(args.prefix, args.verify_consumer_cache)
        else:
            if any(value is None for value in (args.llvm_prefix, args.baseline, args.ca_bundle)):
                parser.error("preparation requires --llvm-prefix, --baseline, and --ca-bundle")
            prepare(args.prefix, args.llvm_prefix, args.baseline, args.ca_bundle, args.jobs)
    except (ValueError, OSError, KeyError, json.JSONDecodeError, tarfile.TarError) as error:
        parser.exit(1, f"native Mac curl preparation failed: {error}\n")


if __name__ == "__main__":
    main()
