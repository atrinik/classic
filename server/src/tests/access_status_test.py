#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline status executable fixture: BINARY STORE_LIBRARY PRIVATE_EMPTY_DIRECTORY."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

binary, library_path, directory = sys.argv[1:]
root = Path(directory)
data = root / 'data'
data.mkdir(mode=0o700)
store_dir = data / 'access-tokens'
certificate = data / 'quic-identity.pem'
key, cert = root / 'key.pem', root / 'cert.pem'
subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key),
                '-out', str(cert), '-days', '1', '-subj', '/CN=isolated-access-status-fixture'],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
certificate.write_bytes(key.read_bytes() + cert.read_bytes())
certificate.chmod(0o600)
der = subprocess.run(['openssl', 'x509', '-in', str(cert), '-outform', 'DER'], check=True, capture_output=True).stdout
identity_bytes = hashlib.sha256(der).digest()
identity = (ctypes.c_ubyte * 32).from_buffer_copy(identity_bytes)
lib = ctypes.CDLL(library_path)
lib.access_store_open.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_void_p, ctypes.c_bool, ctypes.c_bool]
lib.access_store_open.restype = ctypes.c_int
lib.access_store_close.argtypes = [ctypes.c_void_p]
lib.access_state_lock.argtypes = [ctypes.c_char_p]
lib.access_state_lock.restype = ctypes.c_int
lib.access_state_unlock.argtypes = [ctypes.c_int]


def run(policy='protected', cert_path=certificate):
    return subprocess.run([binary, '--data-dir', str(data), '--store-dir', str(store_dir),
        '--certificate', str(cert_path), '--policy', policy], capture_output=True, check=False)


absent = run('open')
assert absent.returncode == 0
assert json.loads(absent.stdout) == {'state': 'absent_open', 'schemaVersion': 1,
    'serverIdentity': identity_bytes.hex(), 'policy': 'open'}
assert run().returncode != 0
store_dir.symlink_to(data / 'missing', target_is_directory=True)
assert run('open').returncode != 0
store_dir.unlink()
store_dir.mkdir(mode=0o700)
assert run('open').returncode != 0  # Existing uninitialized state never becomes absent.
store = ctypes.c_void_p()
assert lib.access_store_open(ctypes.byref(store), os.fsencode(store_dir), identity, True, True) == 0
try:
    assert run().returncode != 0  # Active store ownership cannot be bypassed.
finally:
    lib.access_store_close(store)
before = {p.name: hashlib.sha256(p.read_bytes()).digest() for p in store_dir.iterdir() if p.is_file()}
healthy = run()
assert healthy.returncode == 0
status = json.loads(healthy.stdout)
assert status['state'] == 'initialized' and status['serverIdentity'] == identity_bytes.hex()
assert status['policy'] == 'protected' and status['integrity'] == 'ok' and status['pendingRouteSync'] == 0
assert {p.name: hashlib.sha256(p.read_bytes()).digest() for p in store_dir.iterdir() if p.is_file()} == before
fd = lib.access_state_lock(os.fsencode(data))
assert fd >= 0
try:
    assert run().returncode != 0  # Simulated running-server ownership.
finally:
    lib.access_state_unlock(fd)
link = data / 'link.pem'
link.symlink_to(certificate)
assert run(cert_path=link).returncode != 0
certificate.chmod(0o640)
assert run().returncode != 0
certificate.chmod(0o600)
print('PASS offline exact status, absent-open distinction, store/data locking, read-only behavior and certificate path protections')
