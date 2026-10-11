# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import copy
import contextlib
import importlib.util
import json
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("macos_curl", ROOT / "tools/ci/collect_macos_curl.py")
COLLECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COLLECTOR)


class MacOSCurlCollectionTests(unittest.TestCase):
    def fixture(self, root):
        prefix = root / "curl"
        (prefix / "lib").mkdir(parents=True)
        library = prefix / "lib/libcurl.dylib"
        library.write_bytes(b"fixture curl library, not a native binary")
        (prefix / "include/curl").mkdir(parents=True)
        (prefix / "include/curl/curlver.h").write_text("fixture header")
        (prefix / "include/curl/curl.h").write_text("fixture API header")
        (prefix / ".brew").mkdir()
        (prefix / ".brew/curl.rb").write_text("fixture bottle formula")
        (prefix / "INSTALL_RECEIPT.json").write_text("{}")
        llvm = root / "llvm"
        llvm.mkdir()
        bundle = root / "ca.pem"
        bundle.write_text("fixture CA, never used for a network request")
        data = {
            "header_version_num": 0x081600, "version_num": 0x081600,
            "loaded_library": str(library), "features": ["SSL", "AsynchDNS", "HTTP3"],
            "protocols": ["http", "https", "ldap"],
            "public_tls_default": {"code": 0, "status": 200},
            "public_tls_bundle": {"code": 0, "status": 200},
        }
        return prefix, library, llvm, bundle, data

    def test_environment_removes_tls_proxy_loader_and_compile_overrides(self):
        environment = {
            "PATH": "/native/tools", "HOME": "/runner/home", "NORMAL": "retained",
            "HTTPS_PROXY": "https://proxy.invalid", "all_proxy": "socks://proxy.invalid",
            "DYLD_INSERT_LIBRARIES": "/foreign.dylib", "LD_PRELOAD": "/foreign.so",
            "SSL_CERT_FILE": "/foreign-ca", "CURL_CA_BUNDLE": "/foreign-ca",
            "CFLAGS": "-O0", "LDFLAGS": "-L/foreign", "CPATH": "/foreign/include",
            "PKG_CONFIG_PATH": "/foreign/pkgconfig", "CMAKE_TOOLCHAIN_FILE": "/foreign/toolchain",
            "CURL_SSL_BACKEND": "other", "OPENSSL_CONF": "/foreign.cnf",
            "OPENSSL_CONF_INCLUDE": "/foreign", "OPENSSL_MODULES": "/foreign",
            "OPENSSL_ENGINES": "/foreign", "SSLKEYLOGFILE": "/secret-output",
        }
        self.assertEqual(COLLECTOR.clean_environment(environment), {
            "PATH": "/native/tools", "HOME": "/runner/home", "NORMAL": "retained",
        })

    def test_binding_and_verified_tls_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, library, _, _, original = self.fixture(root)
            foreign = root / "foreign.dylib"
            foreign.write_bytes(b"foreign fixture")
            COLLECTOR.validate_provider(original, library, prefix)
            mutations = [
                ("loaded_library", str(foreign)), ("header_version_num", 0x080700),
                ("features", []), ("protocols", ["http", "http"]),
                ("public_tls_default", {"code": 60, "status": 0}),
                ("public_tls_bundle", {"code": 0, "status": 401}),
            ]
            for key, value in mutations:
                data = copy.deepcopy(original)
                data[key] = value
                with self.subTest(mutation=key), self.assertRaises(ValueError):
                    COLLECTOR.validate_provider(data, library, prefix)
            with self.assertRaisesRegex(ValueError, "outside"):
                COLLECTOR.validate_provider(original, foreign, prefix)
            COLLECTOR.validate_headers(str(prefix / "include"), prefix)
            other = root / "foreign-include"
            other.mkdir()
            for header_dirs in (str(other), f"{prefix}/include;{other}"):
                with self.assertRaisesRegex(ValueError, "header directories"):
                    COLLECTOR.validate_headers(header_dirs, prefix)

    def test_native_orchestration_records_failure_and_never_executes_configure_text(self):
        for runtime_status in (0, 1):
            with self.subTest(runtime_status=runtime_status), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                prefix, library, llvm, bundle, data = self.fixture(root)
                output = root / "absent-output"
                calls = []
                if runtime_status:
                    data["public_tls_default"] = {"code": 60, "status": 0}

                def run(arguments, **kwargs):
                    calls.append(arguments)
                    self.assertNotIn("HTTPS_PROXY", kwargs["env"])
                    stdout = "captured text; never execute $(touch injected)\n"
                    code = 0
                    if arguments[0] == "cmake" and "-S" in arguments:
                        build = Path(arguments[arguments.index("-B") + 1])
                        build.mkdir()
                        (build / "selected-library.txt").write_text(str(library) + "\n")
                        (build / "selected-headers.txt").write_text(str(prefix / "include") + "\n")
                    elif arguments[0].endswith("macos-curl-probe"):
                        stdout = json.dumps(data)
                        code = runtime_status
                    elif arguments[0] == "git":
                        stdout = "a" * 40 + "\n"
                    return subprocess.CompletedProcess(arguments, code, stdout, "")

                with patch.object(COLLECTOR.platform, "system", return_value="Darwin"), \
                        patch.object(COLLECTOR, "CA_SHA256", COLLECTOR.digest(bundle)), \
                        patch.object(COLLECTOR.subprocess, "run", side_effect=run), \
                        patch.dict(COLLECTOR.os.environ, {"HTTPS_PROXY": "https://foreign.invalid"}), \
                        contextlib.redirect_stdout(io.StringIO()):
                    if runtime_status:
                        with self.assertRaisesRegex(ValueError, "verified public TLS"):
                            COLLECTOR.collect(prefix, llvm, output, bundle)
                    else:
                        COLLECTOR.collect(prefix, llvm, output, bundle)
                record = json.loads((output / "baseline.json").read_text())
                self.assertEqual(record["runtime"], data)
                self.assertIn("not source-built", record["purpose"])
                self.assertIn("-DCMAKE_BUILD_TYPE=Release", calls[0])
                self.assertIn(f"-DCURL_LIBRARY_RELEASE={library}", calls[0])
                self.assertTrue(any(call[-1] == "--configure" for call in calls))
                self.assertFalse(any(call[0] in ("bash", "sh") for call in calls))
                self.assertTrue((output / "runtime.stdout").is_file())
                with self.assertRaises(FileExistsError), \
                        patch.object(COLLECTOR.platform, "system", return_value="Darwin"), \
                        patch.object(COLLECTOR, "CA_SHA256", COLLECTOR.digest(bundle)):
                    COLLECTOR.collect(prefix, llvm, output, bundle)

    def test_rejects_foreign_host_and_changed_locked_ca_before_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, llvm, bundle, _ = self.fixture(root)
            output = root / "not-created"
            with patch.object(COLLECTOR.platform, "system", return_value="Linux"), \
                    self.assertRaisesRegex(ValueError, "native macOS"):
                COLLECTOR.collect(prefix, llvm, output, bundle)
            with patch.object(COLLECTOR.platform, "system", return_value="Darwin"), \
                    self.assertRaisesRegex(ValueError, "CA bundle digest"):
                COLLECTOR.collect(prefix, llvm, output, bundle)
            self.assertFalse(output.exists())
            (prefix / ".brew/curl.rb").unlink()
            with patch.object(COLLECTOR.platform, "system", return_value="Darwin"), \
                    patch.object(COLLECTOR, "CA_SHA256", COLLECTOR.digest(bundle)), \
                    self.assertRaisesRegex(ValueError, "bottle provenance absent"):
                COLLECTOR.collect(prefix, llvm, output, bundle)
            self.assertFalse(output.exists())

    def test_workflow_collects_before_application_selection_and_retains_evidence(self):
        text = (ROOT / ".github/workflows/llvm-macos-qualification.yml").read_text()
        self.assertLess(text.index("collect_macos_curl.py"), text.index("Configure the supported macOS"))
        self.assertIn('--prefix "$(brew --prefix curl)"', text)
        self.assertIn('--ca-bundle "${GITHUB_WORKSPACE}/client/ca-bundle.crt"', text)
        self.assertIn("curl-baseline.log", text)
        self.assertIn("            build/macos-qualification-evidence\n", text)


if __name__ == "__main__":
    unittest.main()
