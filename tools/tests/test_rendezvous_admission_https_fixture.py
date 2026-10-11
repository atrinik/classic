# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "admission_https_fixture", ROOT / "libatrinik/tests/rendezvous_admission_https.py")
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)


class AdmissionHTTPSFixtureTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("openssl"), "OpenSSL is required")
    def test_certificate_chain_obeys_strict_server_verification(self):
        with tempfile.TemporaryDirectory(prefix="admission-certificate-") as directory:
            ca, other_ca, certificate, _ = FIXTURE.certificates(Path(directory))
            for authority, hostname, expected in (
                    (ca, "localhost", 0), (other_ca, "localhost", 2),
                    (ca, "wrong.invalid", 2)):
                with self.subTest(authority=authority.name, hostname=hostname):
                    result = subprocess.run(
                        [shutil.which("openssl"), "verify", "-x509_strict",
                         "-purpose", "sslserver", "-verify_hostname", hostname,
                         "-CAfile", str(authority), str(certificate)],
                        capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    def test_native_failure_retains_only_bounded_numeric_diagnostics(self):
        result = subprocess.CompletedProcess(
            [], 1, stdout=b"synthetic attempt secret",
            stderr=(b"arbitrary URL https://private.invalid/secret\n"
                    b"HTTPS admission outcome result=1 solver_calls=0 elapsed_ms=52\n"
                    b"HTTPS admission assertion at line 149\n"
                    b"HTTPS admission assertion at line 150 secret\n"))
        message = FIXTURE.native_failure(result, "valid", 0)
        self.assertIn("exit=1; requests=0", message)
        self.assertIn("result=1 solver_calls=0 elapsed_ms=52", message)
        self.assertIn("assertion at line 149", message)
        self.assertNotIn("private.invalid", message)
        self.assertNotIn("secret", message)
        self.assertNotIn("line 150", message)

    def test_native_failure_handles_missing_and_excessive_diagnostics(self):
        result = subprocess.CompletedProcess([], -6, stdout=b"", stderr=b"\xffarbitrary\n")
        self.assertIn("exit=-6; requests=1; no numeric native diagnostics",
                      FIXTURE.native_failure(result, "valid", 1))
        result.stderr = b"HTTPS admission assertion at line 149\n" * 200
        self.assertEqual(FIXTURE.native_failure(result, "valid", 0).count("line 149"), 3)
        result.stderr = b"x" * 4096 + b"\nHTTPS admission assertion at line 149\n"
        self.assertNotIn("line 149", FIXTURE.native_failure(result, "valid", 0))


if __name__ == "__main__":
    unittest.main()
