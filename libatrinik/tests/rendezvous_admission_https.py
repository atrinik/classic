#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise native admission through disposable, certificate-verified HTTPS."""

import hashlib
import http.server
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time

OPENSSL = "openssl"
SCHEMA = "atrinik-rendezvous-admission-v1"
SERVER_ID = "1" * 64
TICKET_HASH = hashlib.sha256(bytes.fromhex("2" * 64)).hexdigest()
EXPECTED_REQUEST = json.dumps(
    {"schema": SCHEMA, "ticketHash": TICKET_HASH}, separators=(",", ":")
).encode("ascii")


def openssl(*args):
    subprocess.run(
        [OPENSSL, *map(str, args)], check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15,
    )


def certificates(root):
    ca_key, ca_cert = root / "ca.key", root / "ca.pem"
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=Atrinik admission test CA", "-keyout", ca_key,
            "-out", ca_cert, "-addext", "basicConstraints=critical,CA:TRUE")
    key, csr, cert = root / "server.key", root / "server.csr", root / "server.pem"
    openssl("req", "-new", "-newkey", "rsa:2048", "-nodes",
            "-subj", "/CN=localhost", "-keyout", key, "-out", csr)
    extensions = root / "extensions.conf"
    extensions.write_text(
        "subjectAltName=DNS:localhost\n"
        "basicConstraints=critical,CA:FALSE\n"
        "extendedKeyUsage=serverAuth\n", encoding="ascii",
    )
    openssl("x509", "-req", "-in", csr, "-CA", ca_cert, "-CAkey", ca_key,
            "-CAcreateserial", "-days", "1", "-extfile", extensions, "-out", cert)
    other_ca = root / "other-ca.pem"
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=Unrelated admission test CA", "-keyout", root / "other.key",
            "-out", other_ca, "-addext", "basicConstraints=critical,CA:TRUE")
    return ca_cert, other_ca, cert, key


class Fixture(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def handle_error(self, request, client_address):
        # Do not print addresses or HTTP bodies, even on fixture failure.
        self.errors.append("fixture handler failed")


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):
        server = self.server
        server.requests += 1
        try:
            assert self.path == f"/v1/rendezvous/challenge/classic/{SERVER_ID}"
            length = int(self.headers.get("Content-Length", "0"))
            assert 0 < length <= 256
            assert self.rfile.read(length) == EXPECTED_REQUEST
            assert self.headers.get("Content-Type") == "application/json"
            assert self.headers.get("Cache-Control") == "no-store"
            for name in ("Authorization", "Cookie", "Accept-Encoding",
                         "X-Atrinik-Rendezvous-Ticket", "X-Atrinik-Access-Client-Nonce"):
                assert self.headers.get(name) is None
        except (AssertionError, ValueError):
            server.errors.append("invalid challenge request")
            self.send_error(400)
            return
        server.received.set()
        if server.mode in ("deadline", "cancel"):
            # Keep the handler blocked independently of the native callback.
            # The runner releases this event only after the native process exits.
            server.release.wait(6)
        issued = int(time.time())
        token = f"v1.synthetic.{issued}.{TICKET_HASH}.{'3' * 32}.{'4' * 64}"
        body = json.dumps(
            {"schema": SCHEMA, "challenge": token, "difficulty": 24,
             "expiresAt": str(issued + 60)}, separators=(",", ":"),
        ).encode("ascii")
        if server.mode == "malformed":
            body = body.replace(b'"difficulty":24', b'"difficulty":23')
        elif server.mode == "oversized":
            body = b"x" * 513
        try:
            self.send_response(302 if server.mode == "redirect" else 200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            if server.mode != "missing-no-store":
                self.send_header("Cache-Control", "no-store")
            if server.mode == "redirect":
                self.send_header("Location", self.path)
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass


def run_case(binary, mode, ca, other_ca, cert, key):
    with Fixture(("127.0.0.1", 0), Handler) as server:
        server.mode = mode
        server.errors = []
        server.requests = 0
        server.received = threading.Event()
        server.release = threading.Event()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        host = "127.0.0.1" if mode == "wrong-host" else "localhost"
        url = f"wss://{host}:{server.server_port}/servers/{SERVER_ID}?role=client"
        env = dict(os.environ)
        env["ATRINIK_TEST_ADMISSION_CA"] = str(other_ca if mode == "untrusted" else ca)
        # Keep localhost evidence independent of machine proxy configuration.
        env["NO_PROXY"] = env["no_proxy"] = "localhost,127.0.0.1"
        try:
            result = subprocess.run([binary, url, mode], env=env, capture_output=True,
                                    timeout=8, check=False)
            if result.returncode:
                raise RuntimeError(f"native HTTPS admission case failed: {mode}")
            if server.errors:
                raise RuntimeError(f"HTTPS fixture request check failed: {mode}")
            expected_requests = 0 if mode in ("wrong-host", "untrusted") else 1
            if server.requests != expected_requests:
                raise RuntimeError(f"HTTPS fixture request count failed: {mode}")
            if mode in ("deadline", "cancel") and not server.received.is_set():
                raise RuntimeError(f"stalled HTTP case never received request: {mode}")
        finally:
            server.release.set()
            server.shutdown()
            thread.join(timeout=2)
            if thread.is_alive():
                raise RuntimeError("TLS fixture did not stop")


def main():
    global OPENSSL
    if len(sys.argv) != 3:
        raise SystemExit("usage: rendezvous_admission_https.py NATIVE_TEST_BINARY OPENSSL")
    OPENSSL = sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="atrinik-admission-https-") as directory:
        ca, other_ca, cert, key = certificates(Path(directory))
        modes = ("valid", "malformed", "missing-no-store", "oversized", "redirect",
                 "deadline", "cancel", "wrong-host", "untrusted")
        for mode in modes:
            run_case(sys.argv[1], mode, ca, other_ca, cert, key)
    print("verified HTTPS admission: nine transport cases passed")


if __name__ == "__main__":
    main()
