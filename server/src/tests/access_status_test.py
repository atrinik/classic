#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline status executable fixture: BINARY STORE_LIBRARY PRIVATE_EMPTY_DIRECTORY."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary')
parser.add_argument('library')
parser.add_argument('directory')
parser.add_argument('--evidence-dir')
parser.add_argument('--socket-library')
parser.add_argument('--source-sha')
args = parser.parse_args()
binary, library_path, directory = args.binary, args.library, args.directory
if args.evidence_dir:
    if not args.socket_library or not args.source_sha or not re.fullmatch('[0-9a-f]{40}', args.source_sha):
        parser.error('evidence requires --socket-library and exact --source-sha')
    if os.geteuid() != 0:
        parser.error('root socket evidence requires the isolated root fixture worker')
status_evidence = {}
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
status_evidence['absent_open'] = absent.stdout
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
status_evidence['initialized_protected'] = healthy.stdout
opened = run('open')
assert opened.returncode == 0
status_evidence['initialized_open'] = opened.stdout
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


# Optional PUBLIC synthetic evidence contains only bounded status responses.
# No token is issued in this fixture; certificate/key/snapshot bytes stay private.
if args.evidence_dir:
    evidence = Path(args.evidence_dir)
    evidence.mkdir(mode=0o755, parents=True, exist_ok=False)
    endpoint = ctypes.CDLL(args.socket_library)
    schedule_type = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_char_p)
    start_type = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t)
    poll_type = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
    cancel_type = ctypes.CFUNCTYPE(None, ctypes.c_uint64)
    endpoint.admin_shutdown_init.argtypes = [ctypes.c_char_p, schedule_type]
    endpoint.admin_shutdown_init.restype = ctypes.c_bool
    endpoint.admin_shutdown_set_access.argtypes = [start_type, poll_type, cancel_type]
    lib.access_admin_execute.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    lib.access_admin_execute.restype = ctypes.c_bool

    class ConfiguredStatus(ctypes.Structure):
        _fields_ = [('schema_version', ctypes.c_uint), ('server_identity', ctypes.c_ubyte * 32),
                    ('protected_policy', ctypes.c_bool), ('integrity_ok', ctypes.c_bool),
                    ('durability_ok', ctypes.c_bool), ('fenced', ctypes.c_bool),
                    ('revision', ctypes.c_uint64), ('pending_route_sync', ctypes.c_size_t)]

    lib.access_admin_execute_absent.argtypes = [ctypes.POINTER(ConfiguredStatus), ctypes.c_char_p,
        ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    lib.access_admin_execute_absent.restype = ctypes.c_bool
    manifest = {'schema': 'atrinik-synthetic-status-evidence-v1', 'sourceSha': args.source_sha,
        'command': sys.argv, 'scope': 'Native inspector and real root socket framing with native status adapter callbacks; synthetic empty stores only.',
        'cases': []}

    for ordinal, (name, offline_bytes) in enumerate(status_evidence.items(), 1):
        expected = json.loads(offline_bytes)
        required = {'state', 'schemaVersion', 'serverIdentity', 'policy'}
        if name != 'absent_open':
            required |= {'integrity', 'durability', 'revision', 'pendingRouteSync'}
        assert set(expected) == required and len(offline_bytes) < 1024
        assert expected['schemaVersion'] == 1 and expected['serverIdentity'] == identity_bytes.hex()
        online_store = ctypes.c_void_p()
        if name != 'absent_open':
            assert lib.access_store_open(ctypes.byref(online_store), os.fsencode(store_dir), identity,
                name == 'initialized_protected', False) == 0
        configured = ConfiguredStatus(schema_version=1, server_identity=identity)
        request = {'schema': 'atrinik-access-admin-v1', 'operation': 'status', 'requestId': f'{ordinal:032x}'}
        request_bytes = json.dumps(request, separators=(',', ':')).encode()
        response = None
        callback_errors = []

        @schedule_type
        def schedule(seconds, reason):
            return False

        @start_type
        def start(raw, size):
            global response
            try:
                body = ctypes.string_at(raw, size)
                assert body == request_bytes
                output = ctypes.create_string_buffer(32769)
                length = ctypes.c_size_t()
                if online_store.value:
                    ok = lib.access_admin_execute(online_store, body, len(body), None, None,
                        output, len(output), ctypes.byref(length))
                else:
                    ok = lib.access_admin_execute_absent(ctypes.byref(configured), body, len(body),
                        output, len(output), ctypes.byref(length))
                assert ok and 0 < length.value < 1024
                response = output.raw[:length.value]
                return 1
            except BaseException as error:
                callback_errors.append(type(error).__name__)
                return 0

        @poll_type
        def poll(job, output, capacity, length):
            if job != 1 or response is None or len(response) >= capacity:
                return False
            ctypes.memmove(output, response, len(response))
            length[0] = len(response)
            return True

        @cancel_type
        def cancel(job):
            pass

        path = root / (name + '.sock')
        wire = bytearray()
        try:
            assert endpoint.admin_shutdown_init(os.fsencode(path), schedule)
            endpoint.admin_shutdown_set_access(start, poll, cancel)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
                peer.settimeout(0.05)
                peer.connect(str(path))
                peer.sendall(b'ATRINIK-ADMIN/1 ACCESS ' + request_bytes + b'\n')
                peer.shutdown(socket.SHUT_WR)
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    endpoint.admin_shutdown_poll()
                    try:
                        chunk = peer.recv(4096)
                    except TimeoutError:
                        continue
                    if not chunk:
                        break
                    wire.extend(chunk)
                else:
                    raise AssertionError('status capture deadline')
            assert not callback_errors
            header, online_bytes = bytes(wire).split(b'\n', 1)
            assert header == b'ATRINIK-ADMIN/1 ACCESS ' + str(len(online_bytes)).encode()
            online = json.loads(online_bytes)
            assert set(online) == {'schema', 'operation', 'requestId', 'outcome', 'revision', 'result'}
            assert all(online[key] == request[key] for key in request)
            assert online['outcome'] == 'committed' and online['result'] == expected
            assert online['revision'] == expected.get('revision')
            files = {}
            for suffix, content in (('offline.json', offline_bytes), ('online.frame', bytes(wire))):
                filename = name + '.' + suffix
                with (evidence / filename).open('xb') as output:
                    output.write(content)
                files[suffix] = {'file': filename, 'sha256': hashlib.sha256(content).hexdigest(), 'bytes': len(content)}
            manifest['cases'].append({'name': name, 'requestId': request['requestId'], 'files': files})
        finally:
            endpoint.admin_shutdown_deinit()
            if online_store.value:
                lib.access_store_close(online_store)
    with (evidence / 'manifest.json').open('x') as output:
        json.dump(manifest, output, indent=2)
        output.write('\n')
    print('PASS captured public synthetic initialized-protected/open and absent-open status bytes')
