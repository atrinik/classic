from __future__ import annotations

import hashlib
from pathlib import Path
import ssl
import unittest


ROOT = Path(__file__).resolve().parents[2]
BUNDLE_SHA256 = "a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505"


class PublicCABundleTests(unittest.TestCase):
    def test_shipped_copies_match_dated_upstream_bytes(self) -> None:
        client = (ROOT / "client/ca-bundle.crt").read_bytes()
        server = (ROOT / "server/ca-bundle.crt").read_bytes()
        self.assertEqual(client, server)
        self.assertEqual(len(client), 188900)
        self.assertEqual(hashlib.sha256(client).hexdigest(), BUNDLE_SHA256)
        self.assertEqual(client.count(b"-----BEGIN CERTIFICATE-----"), 121)
        self.assertEqual(client.count(b"-----END CERTIFICATE-----"), 121)
        self.assertNotIn(b"PRIVATE KEY", client)

    def test_complete_bundle_loads_as_public_trust_roots(self) -> None:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.load_verify_locations(cafile=str(ROOT / "client/ca-bundle.crt"))
        self.assertEqual(context.cert_store_stats()["x509_ca"], 121)
        self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(context.check_hostname)

    def test_shipped_attribution_identifies_upstream_and_license(self) -> None:
        notice = (ROOT / "ATTRIBUTIONS.md").read_text(encoding="utf-8")
        self.assertIn("https://www.mozilla.org/MPL/2.0/", notice)
        self.assertIn("https://curl.se/ca/cacert-2026-09-25.pem", notice)
        self.assertIn("`ca-bundle.crt`", notice)


if __name__ == "__main__":
    unittest.main()
