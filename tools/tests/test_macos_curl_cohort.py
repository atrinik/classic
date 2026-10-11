# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
CI = ROOT / "tools/ci"
SPEC = importlib.util.spec_from_file_location("prepare_macos_curl", CI / "prepare_macos_curl.py")
COHORT = importlib.util.module_from_spec(SPEC)
sys.path.insert(0, str(CI))
try:
    SPEC.loader.exec_module(COHORT)
finally:
    sys.path.pop(0)


class MacOSCurlCohortTests(unittest.TestCase):
    def setUp(self):
        self.manifest = COHORT.load_manifest()

    def runtime(self):
        baseline = self.manifest["baseline"]
        result = dict(baseline["versions"], features=list(baseline["features"]),
                      protocols=list(baseline["protocols"]), header_version="8.22.0",
                      header_version_num=0x081600, version_num=0x081600,
                      ares="1.34.6", cares_version="1.34.6", cares_header_version="1.34.6")
        for name in ("public_tls_default", "public_tls_bundle", "private_tls_trusted"):
            result[name] = {"code": 0, "status": 200, "ssl_verify_result": 0,
                            "default_ca_bundle": None, "default_ca_path": None}
        for name in ("public_tls_unrelated", "private_tls_untrusted", "private_tls_wrong_hostname"):
            result[name] = {"code": 60, "status": 0, "ssl_verify_result": 20}
        return result

    def baseline(self):
        runtime = self.runtime()
        runtime["ares"] = None
        return {"runtime": runtime, "ca_bundle_sha256": self.manifest["ca_bundle_sha256"]}

    def test_manifest_rejects_unsupported_schema_toolchain_and_malformed_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            for key, value in (("schema_version", 2), ("llvm_version", "23.1.2"),
                               ("sha256", "a" * 63), ("sha256", "A" * 64)):
                data = copy.deepcopy(self.manifest)
                if key == "sha256":
                    data["sources"]["curl"][key] = value
                else:
                    data[key] = value
                path.write_text(json.dumps(data))
                with self.subTest(key=key, value=value), patch.object(COHORT, "MANIFEST", path), \
                        self.assertRaises(ValueError):
                    COHORT.load_manifest()

    def test_baseline_requires_exact_measured_capabilities_and_versions(self):
        original = self.baseline()
        COHORT.validate_baseline(original, self.manifest)
        self.assertEqual(len(original["runtime"]["features"]), 20)
        self.assertEqual(len(original["runtime"]["protocols"]), 25)
        for key in ("features", "protocols"):
            for lost in original["runtime"][key]:
                data = copy.deepcopy(original)
                data["runtime"][key].remove(lost)
                with self.subTest(key=key, lost=lost), self.assertRaises(ValueError):
                    COHORT.validate_baseline(data, self.manifest)
            for added in ("unmeasured", original["runtime"][key][0]):
                data = copy.deepcopy(original)
                data["runtime"][key].append(added)
                with self.subTest(key=key, added=added), self.assertRaises(ValueError):
                    COHORT.validate_baseline(data, self.manifest)
        for key in self.manifest["baseline"]["versions"]:
            data = copy.deepcopy(original)
            data["runtime"][key] = "changed version"
            with self.subTest(version=key), self.assertRaises(ValueError):
                COHORT.validate_baseline(data, self.manifest)
        for key, value in (("ares", "1.34.6"),):
            data = copy.deepcopy(original)
            data["runtime"][key] = value
            with self.assertRaises(ValueError):
                COHORT.validate_baseline(data, self.manifest)
        data = copy.deepcopy(original)
        data["ca_bundle_sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            COHORT.validate_baseline(data, self.manifest)

    def test_baseline_trust_policy_and_positive_proofs_are_fixed(self):
        for proof in ("public_tls_default", "public_tls_bundle"):
            for key, value in (("code", 60), ("status", 301), ("ssl_verify_result", 20),
                               ("default_ca_bundle", "/foreign.pem"), ("default_ca_path", "/foreign")):
                data = self.baseline()
                data["runtime"][proof][key] = value
                with self.subTest(proof=proof, key=key), self.assertRaises(ValueError):
                    COHORT.validate_baseline(data, self.manifest)

    def test_runtime_preserves_every_capability_and_provider_identity(self):
        original = self.runtime()
        COHORT.validate_runtime(original, self.manifest)
        extra = copy.deepcopy(original)
        extra["features"].append("additional-supported-feature")
        extra["protocols"].append("additional-supported-protocol")
        COHORT.validate_runtime(extra, self.manifest)
        for key in ("features", "protocols"):
            for lost in original[key]:
                data = copy.deepcopy(original)
                data[key].remove(lost)
                with self.subTest(key=key, lost=lost), self.assertRaises(ValueError):
                    COHORT.validate_runtime(data, self.manifest)
            data = copy.deepcopy(original)
            data[key].append(data[key][0])
            with self.subTest(duplicate=key), self.assertRaises(ValueError):
                COHORT.validate_runtime(data, self.manifest)
        changes = [(key, "changed version") for key in self.manifest["baseline"]["versions"]]
        changes += [("header_version", "8.21.0"), ("header_version_num", 0x081500),
                    ("ares", None), ("ares", "1.34.5"), ("cares_version", "1.34.5"), ("cares_header_version", "1.34.5"),
                    ("features", original["features"] + ["asyn-rr"])]
        for key, value in changes:
            data = copy.deepcopy(original)
            data[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                COHORT.validate_runtime(data, self.manifest)

    def test_runtime_requires_verified_positive_and_specific_negative_tls_results(self):
        for proof in ("public_tls_default", "public_tls_bundle", "private_tls_trusted"):
            for key, value in (("code", 60), ("status", 0), ("status", 302), ("ssl_verify_result", 1)):
                data = self.runtime()
                data[proof][key] = value
                with self.subTest(proof=proof, key=key, value=value), self.assertRaises(ValueError):
                    COHORT.validate_runtime(data, self.manifest)
        for proof in ("public_tls_unrelated", "private_tls_untrusted", "private_tls_wrong_hostname"):
            for key, value in (("code", 0), ("code", 7), ("code", 35), ("status", 200)):
                data = self.runtime()
                data[proof][key] = value
                with self.subTest(proof=proof, key=key, value=value), self.assertRaises(ValueError):
                    COHORT.validate_runtime(data, self.manifest)
        for proof in ("public_tls_default", "public_tls_bundle"):
            for key in ("default_ca_bundle", "default_ca_path"):
                data = self.runtime()
                data[proof][key] = "/unexpected-default"
                with self.subTest(proof=proof, key=key), self.assertRaises(ValueError):
                    COHORT.validate_runtime(data, self.manifest)

    def test_config_requires_cares_and_preserves_ldap_native_idn_and_trust_policy(self):
        original = "\n".join(f"#define {name} 1" for name in self.manifest["required_config"])
        COHORT.validate_config(original, self.manifest)
        for name in self.manifest["required_config"]:
            for replacement in ("", f"#define {name} 0", f"/* #undef {name} */"):
                changed = original.replace(f"#define {name} 1", replacement)
                with self.subTest(required=name, replacement=replacement), self.assertRaises(ValueError):
                    COHORT.validate_config(changed, self.manifest)
        for name in self.manifest["forbidden_config"]:
            with self.subTest(forbidden=name), self.assertRaises(ValueError):
                COHORT.validate_config(original + f'\n#define {name} "unexpected"', self.manifest)

    def test_configure_policy_uses_sdk_zlib_and_owned_resolver(self):
        prefix, sdk = Path("/owned curl"), Path("/native SDK")
        deps = {name: Path("/kegs") / name for name in self.manifest["dependency_formulae"]}
        arguments = COHORT.configure_arguments(prefix, sdk, deps)
        options = dict(argument.split("=", 1) if "=" in argument else (argument, None)
                       for argument in arguments)
        self.assertEqual(options["--enable-ares"], str(prefix))
        self.assertEqual(options["--with-zlib"], str(sdk / "usr"))
        self.assertEqual(options["--with-default-ssl-backend"], "openssl")
        for option, dep in (("ssl", "openssl@3"), ("brotli", "brotli"), ("zstd", "zstd"),
                            ("libpsl", "libpsl"), ("libssh2", "libssh2"),
                            ("nghttp2", "libnghttp2"), ("nghttp3", "libnghttp3"), ("ngtcp2", "libngtcp2")):
            self.assertEqual(options["--with-" + option], str(deps[dep]))
        for option in ("--disable-threaded-resolver", "--without-ca-bundle", "--without-ca-path",
                       "--with-ca-fallback", "--with-apple-sectrust", "--with-gssapi",
                       "--with-apple-idn", "--without-libidn2", "--enable-ldap", "--enable-ldaps"):
            self.assertIn(option, options)
        self.assertFalse(any("unwind" in value or "libstdc++" in value for value in arguments))

    def test_source_archive_digest_root_and_link_containment(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, (name, target) in enumerate((("curl-fixture/COPYING", None),
                                                   ("other/COPYING", None),
                                                   ("curl-fixture/../../escaped", None),
                                                   ("/curl-fixture/absolute", None),
                                                   ("curl-fixture/link", "../../escaped"))):
                archive = root / f"source-{index}.tar"
                with tarfile.open(archive, "w") as output:
                    member = tarfile.TarInfo(name)
                    if target:
                        member.type = tarfile.SYMTYPE
                        member.linkname = target
                        output.addfile(member)
                    else:
                        content = b"source fixture"
                        member.size = len(content)
                        output.addfile(member, io.BytesIO(content))
                source = {"sha256": COHORT.digest(archive), "directory": "curl-fixture"}
                destination = root / f"destination-{index}"
                with self.subTest(name=name, target=target):
                    if index == 0:
                        extracted = COHORT.extract_source(archive, destination, source)
                        self.assertEqual((extracted / "COPYING").read_bytes(), b"source fixture")
                        source["sha256"] = "0" * 64
                        with self.assertRaises(ValueError):
                            COHORT.extract_source(archive, root / "bad-digest", source)
                        self.assertFalse((root / "bad-digest").exists())
                    else:
                        with self.assertRaises((ValueError, tarfile.TarError)):
                            COHORT.extract_source(archive, destination, source)
                self.assertFalse((root / "escaped").exists())

    def test_loaded_inventory_rejects_foreign_providers_missing_images_and_dependency_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "cohort"
            (prefix / "lib").mkdir(parents=True)
            curl, cares = prefix / "lib/libcurl.dylib", prefix / "lib/libcares.dylib"
            probe, dep, foreign = root / "probe", root / "keg/ssl.dylib", root / "foreign/libcurl.dylib"
            for path in (curl, cares, probe, dep, foreign):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"same-version fixture binary")
            files = {str(dep): COHORT.digest(dep)}
            data = {"loaded_library": str(curl), "loaded_cares_library": str(cares),
                    "loaded_images": [str(curl), str(cares), str(probe), str(dep),
                                      "/usr/lib/libSystem.B.dylib", "/System/Library/Frameworks/Security.framework/Security",
                                      "/Library/Apple/System/Library/fixture.dylib"]}
            platform_images = ["/usr/lib/libkrb5.dylib", "/usr/lib/libldap.dylib",
                               "/usr/lib/libz.1.dylib", "/usr/lib/libicucore.A.dylib"]
            data["loaded_images"].extend(platform_images)
            # macOS shared-cache install names need no ordinary filesystem file.
            COHORT.validate_loaded(data, prefix, files, probe, platform_images)
            for missing in platform_images:
                changed = copy.deepcopy(data)
                changed["loaded_images"].remove(missing)
                with self.subTest(missing_platform=missing), self.assertRaisesRegex(ValueError, "baseline SDK"):
                    COHORT.validate_loaded(changed, prefix, files, probe, platform_images)
            mutations = [("loaded_library", str(foreign)), ("loaded_cares_library", str(foreign)),
                         ("loaded_images", []), ("loaded_images", data["loaded_images"] + [str(curl)]),
                         ("loaded_images", data["loaded_images"] + [str(foreign)]),
                         ("loaded_images", data["loaded_images"] + ["relative.dylib"]),
                         ("loaded_images", data["loaded_images"] + ["/usr/lib/../foreign.dylib"])]
            mutations += [("loaded_images", [image for image in data["loaded_images"] if image != str(own)])
                          for own in (curl, cares)]
            for key, value in mutations:
                changed = copy.deepcopy(data)
                changed[key] = value
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    COHORT.validate_loaded(changed, prefix, files, probe)
            with self.assertRaises(ValueError):
                COHORT.validate_loaded(data, prefix, {}, probe)
            dep.write_bytes(b"changed dependency bytes")
            with self.assertRaises(ValueError):
                COHORT.validate_loaded(data, prefix, files, probe)
            curl.unlink()
            curl.symlink_to(foreign)
            with self.assertRaises(ValueError):
                COHORT.validate_loaded(data, prefix, files, probe)

    def test_header_directories_are_owned_even_for_same_version_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, foreign = root / "cohort", root / "foreign"
            for include in (prefix / "include", foreign / "include"):
                include.mkdir(parents=True)
                (include / "curl").mkdir()
                (include / "curl/curl.h").write_text("same version API")
            COHORT.validate_headers(str(prefix / "include"), prefix)
            for selected in (str(foreign / "include"), f"{prefix}/include;{foreign}/include"):
                with self.subTest(selected=selected), self.assertRaises(ValueError):
                    COHORT.validate_headers(selected, prefix)

    def test_consumer_cache_binds_prefix_providers_headers_compiler_and_release_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, foreign = root / "cohort", root / "same-version-foreign"
            headers = ("curl/curl.h", "curl/curlver.h", "ares.h", "ares_version.h")
            owned_files = [prefix / "lib/libcurl.dylib", prefix / "lib/libcares.dylib",
                           *[prefix / "include" / name for name in headers]]
            compiler = root / "llvm/bin/clang"
            for path in [*owned_files, compiler]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"verified same-version fixture")
            for path in (foreign / "lib/libcurl.dylib", foreign / "include/curl/curl.h", foreign / "bin/clang"):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"verified same-version fixture")
            receipt_path = prefix / "evidence/receipt.json"
            receipt_path.parent.mkdir()
            receipt = {"prefix": str(prefix), "curl_sha256": COHORT.digest(owned_files[0]),
                       "cares_sha256": COHORT.digest(owned_files[1]),
                       "header_hashes": {name: COHORT.digest(prefix / "include" / name) for name in headers},
                       "tools": {"clang": {"path": str(compiler), "sha256": COHORT.digest(compiler)}},
                       "release_flags": ["-O3", "-DNDEBUG"]}
            receipt_path.write_text(json.dumps(receipt))
            cache_path = root / "CMakeCache.txt"
            cache = {"CURL_INCLUDE_DIR": str(prefix / "include"),
                     "CURL_LIBRARY_RELEASE": str(prefix / "lib/libcurl.dylib"),
                     "CMAKE_C_COMPILER": str(compiler), "CMAKE_BUILD_TYPE": "Release",
                     "CMAKE_C_FLAGS_RELEASE": "-O3 -DNDEBUG"}

            def write_cache(values):
                cache_path.write_text("# fixture CMake cache\n" + "\n".join(
                    f"{key}:STRING={value}" for key, value in values.items()) + "\n")

            write_cache(cache)
            with patch.object(COHORT.platform, "system", return_value="Linux"), \
                    self.assertRaisesRegex(ValueError, "actual native Mac"):
                COHORT.verify_consumer_cache(prefix, cache_path)
            with patch.object(COHORT.platform, "system", return_value="Darwin"), \
                    contextlib.redirect_stdout(io.StringIO()):
                COHORT.verify_consumer_cache(prefix, cache_path)
                mutations = [("CURL_INCLUDE_DIR", str(foreign / "include")),
                             ("CURL_LIBRARY_RELEASE", str(foreign / "lib/libcurl.dylib")),
                             ("CMAKE_C_COMPILER", str(foreign / "bin/clang")),
                             ("CURL_INCLUDE_DIR", ""), ("CURL_LIBRARY_RELEASE", ""),
                             ("CMAKE_BUILD_TYPE", "Debug"), ("CMAKE_C_FLAGS_RELEASE", "-O0 -DNDEBUG"),
                             ("CMAKE_C_FLAGS_RELEASE", "-O3"), ("CMAKE_C_FLAGS_RELEASE", "-O3 -DNDEBUG -g")]
                for key, value in mutations:
                    changed = dict(cache, **{key: value})
                    write_cache(changed)
                    with self.subTest(cache_key=key, value=value), self.assertRaises(ValueError):
                        COHORT.verify_consumer_cache(prefix, cache_path)
                write_cache(cache)
                changed = dict(receipt, prefix=str(foreign))
                receipt_path.write_text(json.dumps(changed))
                with self.assertRaisesRegex(ValueError, "another install prefix"):
                    COHORT.verify_consumer_cache(prefix, cache_path)
                receipt_path.write_text(json.dumps(receipt))
                for path in [*owned_files, compiler]:
                    original = path.read_bytes()
                    path.write_bytes(b"changed same-version fixture")
                    with self.subTest(changed_bytes=path), self.assertRaises(ValueError):
                        COHORT.verify_consumer_cache(prefix, cache_path)
                    path.write_bytes(original)
                COHORT.verify_consumer_cache(prefix, cache_path)

    def test_failed_command_retains_output_status_and_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory)
            commands = COHORT.Commands(evidence, {"PATH": "/fixture"})
            result = subprocess.CompletedProcess(["fixture", "literal $(not executed)"], 17, "partial output", "failure detail")
            with patch.object(COHORT.subprocess, "run", return_value=result) as run, \
                    self.assertRaisesRegex(ValueError, "17"):
                commands.run("failure", result.args)
            self.assertEqual((evidence / "failure.stdout").read_text(), result.stdout)
            self.assertEqual((evidence / "failure.stderr").read_text(), result.stderr)
            record = json.loads((evidence / "commands.json").read_text())[0]
            self.assertEqual(record["returncode"], 17)
            self.assertEqual(record["arguments"], result.args)
            self.assertEqual(run.call_args.args[0], result.args)
            self.assertNotIn("shell", run.call_args.kwargs)


if __name__ == "__main__":
    unittest.main()
