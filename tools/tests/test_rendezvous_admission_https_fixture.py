# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

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
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                FIXTURE.verify_certificates(ca, other_ca, certificate)
            self.assertEqual(output.getvalue(),
                             "HTTPS fixture certificate preflight trusted=0 untrusted=2 wrong_host=2\n")
            with self.assertRaisesRegex(RuntimeError, "preflight case=1 exit=2 expected=0"):
                FIXTURE.verify_certificates(other_ca, ca, certificate)

    @unittest.skipUnless(shutil.which("openssl"), "OpenSSL is required")
    def test_certificate_profiles_are_independent_of_host_request_extensions(self):
        with tempfile.TemporaryDirectory(prefix="admission-host-config-") as directory:
            root = Path(directory)
            host_config = root / "host-openssl.cnf"
            host_config.write_text(
                "[req]\ndistinguished_name=dn\nx509_extensions=host_ca\n"
                "[dn]\n[host_ca]\nbasicConstraints=critical,CA:TRUE\n"
                "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n"
                "nameConstraints=critical,excluded;DNS:localhost\n", encoding="ascii")
            with mock.patch.dict(os.environ, {"OPENSSL_CONF": str(host_config)}):
                ca, other_ca, certificate, _ = FIXTURE.certificates(root)
                with contextlib.redirect_stdout(io.StringIO()):
                    FIXTURE.verify_certificates(ca, other_ca, certificate)

    def test_certificate_failure_retains_only_bounded_numeric_errors(self):
        stderr = (b"private certificate subject and file path\n"
                  b"error 20 at 0 depth lookup: private details\n"
                  b"error 86 at 1 depth lookup: more private details\n"
                  b"error 1234 at 0 depth lookup: invalid error width\n")
        self.assertEqual(FIXTURE.verification_errors(stderr),
                         "verify_error=20 depth=0; verify_error=86 depth=1")
        self.assertEqual(FIXTURE.verification_errors(b"\xffprivate"),
                         "no numeric verification errors")
        self.assertEqual(FIXTURE.verification_errors(b"x" * 4096 + stderr),
                         "no numeric verification errors")
        repeated = b"error 20 at 0 depth lookup: private\n" * 100
        self.assertEqual(FIXTURE.verification_errors(repeated).count("verify_error=20"), 3)

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

    def test_native_failure_retains_transport_evidence_without_text_fields(self):
        transport = (b"HTTPS admission transport init_calls=1 ca_calls=1 ca_result=0 "
                     b"curl_code=60 verify=20 verify_info=0 status=0 status_info=0 "
                     b"os_errno=-1 os_info=0 version=530944 async_dns=1 ares=1 "
                     b"ca_seen=1 ca_matches=1 native_seen=0 debug_result=0 verbose_result=0 easy=0")
        result = subprocess.CompletedProcess(
            [], 1, stdout=b"private", stderr=transport + b"\n" + transport + b" secret\n")
        message = FIXTURE.native_failure(result, "valid", 0)
        self.assertIn(transport.decode("ascii"), message)
        self.assertEqual(message.count("HTTPS admission transport"), 1)
        self.assertNotIn("secret", message)
        self.assertNotIn("private", message)
        for invalid in (transport.replace(b"ca_calls=1", b"ca_calls=99999999999"),
                        transport.replace(b"ares=1", b"ares=2"),
                        transport.replace(b"verify=20", b"verify=private")):
            result.stderr = invalid
            self.assertIn("no numeric native diagnostics",
                          FIXTURE.native_failure(result, "valid", 0))

    def test_successful_diagnostic_preserves_original_failure_and_deadline(self):
        with mock.patch.object(FIXTURE, "certificates", return_value=(1, 2, 3, 4)), \
             mock.patch.object(FIXTURE, "verify_certificates"), \
             mock.patch.object(FIXTURE.sys, "argv", ["fixture", "binary", "openssl"]), \
             mock.patch.object(FIXTURE, "run_case", side_effect=[
                 RuntimeError("original exit=1; requests=0; curl_code=60"),
                 "diagnostic only: exit=0; requests=1; curl_code=0"]) as run:
            with self.assertRaisesRegex(RuntimeError, "original exit=1; requests=0; curl_code=60") as error:
                FIXTURE.main()
            self.assertIn("diagnostic only: exit=0; requests=1", str(error.exception))
            self.assertEqual(run.call_count, 2)
            self.assertEqual(run.call_args_list[0].args[1], "valid")
            self.assertEqual(run.call_args_list[1].args[1], "diagnostic-easy")
            self.assertTrue(run.call_args_list[1].kwargs["diagnostic"])
            self.assertEqual(run.call_args_list[0].kwargs["deadline"],
                             run.call_args_list[1].kwargs["deadline"])

    def test_exhausted_deadline_does_not_launch_a_diagnostic(self):
        with mock.patch.object(FIXTURE, "Fixture") as server, \
             mock.patch.object(FIXTURE.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "deadline exhausted"):
                FIXTURE.run_case("private binary", "diagnostic-easy", 1, 2, 3, 4,
                                 diagnostic=True, deadline=0)
            server.assert_not_called()
            run.assert_not_called()

    def test_certificate_preflight_timeout_is_sanitized(self):
        with mock.patch.object(FIXTURE.subprocess, "run", side_effect=
                               subprocess.TimeoutExpired("private certificate path", 15)):
            with self.assertRaises(RuntimeError) as error:
                FIXTURE.verify_certificates("private CA", "other CA", "private cert")
            self.assertEqual(str(error.exception),
                             "HTTPS fixture certificate preflight case=1 error=TimeoutExpired")

    def test_diagnostic_timeout_preserves_original_failure(self):
        with mock.patch.object(FIXTURE, "certificates", return_value=(1, 2, 3, 4)), \
             mock.patch.object(FIXTURE, "verify_certificates"), \
             mock.patch.object(FIXTURE.sys, "argv", ["fixture", "binary", "openssl"]), \
             mock.patch.object(FIXTURE, "run_case", side_effect=[
                 RuntimeError("original exit=1; requests=0"),
                 subprocess.TimeoutExpired("private command", 1)]):
            with self.assertRaises(RuntimeError) as error:
                FIXTURE.main()
            self.assertIn("original exit=1; requests=0", str(error.exception))
            self.assertIn("HTTPS diagnostic unavailable: TimeoutExpired", str(error.exception))
            self.assertNotIn("private command", str(error.exception))

    def test_negative_cases_never_run_diagnostic(self):
        with mock.patch.object(FIXTURE, "certificates", return_value=(1, 2, 3, 4)), \
             mock.patch.object(FIXTURE, "verify_certificates"), \
             mock.patch.object(FIXTURE.sys, "argv", ["fixture", "binary", "openssl"]), \
             mock.patch.object(FIXTURE, "run_case", side_effect=[None, RuntimeError("malformed")]) as run:
            with self.assertRaisesRegex(RuntimeError, "malformed"):
                FIXTURE.main()
            self.assertEqual(run.call_count, 2)
            self.assertEqual(run.call_args_list[1].args[1], "malformed")

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
