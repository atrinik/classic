#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Standalone Linux endpoint fixture: LIBRARY PRIVATE_DIRECTORY, root in pinned worker."""
import ctypes
import json
import os
from pathlib import Path
import socket
import sys

lib = ctypes.CDLL(sys.argv[1])
root = Path(sys.argv[2])
assert os.geteuid() == 0
schedule_type = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_char_p)
start_type = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t)
poll_type = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
cancel_type = ctypes.CFUNCTYPE(None, ctypes.c_uint64)
ready = False
requests = []
cancelled = []
# Exact maximum frame exercises offset/header memmoves and EOF after full output.
body = b'{"padding":"' + b'x' * (32768 - len(b'{"padding":""}')) + b'"}'

@schedule_type
def schedule(seconds, reason):
    return True

@start_type
def start(data, size):
    requests.append(ctypes.string_at(data, size))
    return 123

@poll_type
def poll(job, output, capacity, size):
    assert job == 123 and capacity >= len(body) + 1
    if not ready:
        return False
    ctypes.memmove(output, body, len(body))
    size[0] = len(body)
    return True

@cancel_type
def cancel(job):
    cancelled.append(job)

lib.admin_shutdown_init.argtypes = [ctypes.c_char_p, schedule_type]
lib.admin_shutdown_init.restype = ctypes.c_bool
lib.admin_shutdown_set_access.argtypes = [start_type, poll_type, cancel_type]
path = root / 'access.sock'
assert lib.admin_shutdown_init(os.fsencode(path), schedule)
lib.admin_shutdown_set_access(start, poll, cancel)


def connect():
    peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    peer.settimeout(0.02)
    peer.connect(str(path))
    return peer


try:
    with connect() as peer:
        peer.sendall(b'ATRINIK-ADMIN/1 CAPABILITIES\n')
        peer.shutdown(socket.SHUT_WR)
        for _ in range(3):
            lib.admin_shutdown_poll()
        assert peer.recv(1024) == b'ATRINIK-ADMIN/1 CAPABILITIES shutdown-v1 durable-result-v1 access-tokens-v1\n'
    request = b'{"schema":"atrinik-access-admin-v1","operation":"status","requestId":"' + b'a' * 32 + b'"}'
    with connect() as peer:
        peer.sendall(b'ATRINIK-ADMIN/1 ACCESS ' + request + b'\n')
        lib.admin_shutdown_poll()
        assert not requests
        peer.shutdown(socket.SHUT_WR)
        lib.admin_shutdown_poll()
        assert requests == [request]
        for _ in range(10):
            lib.admin_shutdown_poll()
        try:
            peer.recv(1)
            raise AssertionError('response before worker completion')
        except TimeoutError:
            pass
        ready = True
        data = bytearray()
        while True:
            lib.admin_shutdown_poll()
            try:
                fragment = peer.recv(257)
            except TimeoutError:
                continue
            if not fragment:
                break
            data.extend(fragment)
        assert data == b'ATRINIK-ADMIN/1 ACCESS 32768\n' + body
        assert not cancelled
    # Incomplete/trailing line bodies never reach the worker.
    for suffix in (b'\nextra\n', b'\n\0', b'\r\n'):
        with connect() as peer:
            peer.sendall(b'ATRINIK-ADMIN/1 ACCESS ' + request + suffix)
            peer.shutdown(socket.SHUT_WR)
            for _ in range(3):
                lib.admin_shutdown_poll()
            assert peer.recv(1024).startswith(b'ATRINIK-ADMIN/1 ERROR malformed')
    assert len(requests) == 1
    ready = False
    with connect() as peer:
        peer.sendall(b'ATRINIK-ADMIN/1 ACCESS ' + request + b'\n')
        peer.shutdown(socket.SHUT_WR)
        for _ in range(3):
            lib.admin_shutdown_poll()
        lib.admin_shutdown_deinit()
        assert cancelled == [123]
finally:
    lib.admin_shutdown_deinit()
print('PASS async root access socket, maximum frame, fragmented reads, strict line bounds and cancellation cleanup')
