#!/usr/bin/env python3
"""Exercise the standalone endpoint library in an isolated Linux worker.

Compile admin_shutdown.c as a shared library using the pinned build worker.
Arguments: LIBRARY PRIVATE_DIRECTORY. Run as root for scheduling/receipt cases,
and as the normal build UID for the unauthorized-peer case. No host privileges
or production server data are used. The directory must have trusted ancestry.
"""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations

import ctypes
import os
from pathlib import Path
import socket
import stat
import sys
import time

library = ctypes.CDLL(sys.argv[1])
root = Path(sys.argv[2])
assert root.is_dir() and stat.S_IMODE(root.stat().st_mode) == 0o700
class Request(ctypes.Structure):
    _fields_ = [("id", ctypes.c_char * 33), ("seconds", ctypes.c_uint),
                ("reason", ctypes.c_char * 193)]

library.admin_shutdown_parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                         ctypes.POINTER(Request)]
library.admin_shutdown_parse.restype = ctypes.c_bool

def parse(body):
    parsed = Request()
    return bool(library.admin_shutdown_parse(body, len(body), ctypes.byref(parsed)))

prefix = b"ATRINIK-ADMIN/1 SHUTDOWN " + b"0" * 32 + b" "
for seconds in (30, 60, 600):
    assert parse(prefix + str(seconds).encode() + " Maintenance bientôt\n".encode())
assert parse(prefix + b"60 " + b"a" * 192 + b"\n")
assert not parse(prefix + b"60 " + b"a" * 193 + b"\n")
for seconds in (b"29", b"601", b"060", b"+60", b"-60", b"999999999"):
    assert not parse(prefix + seconds + b" update\n")
for reason in (b"", b"\0", b"\x1b", b"\t", b"\n", b"\xc0\xaf",
               b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xc2\x85",
               b"\xe2\x80\xae", b"\xe2\x80", b"ok\nnext"):
    assert not parse(prefix + b"60 " + reason + b"\n")
valid = prefix + b"60 update\n"
for size in range(len(valid)):
    assert not parse(valid[:size])
assert not parse(valid.replace(b"ADMIN/1", b"ADMIN/2"))

callback_type = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_char_p)
calls = []

@callback_type
def schedule(seconds, reason):
    calls.append((seconds, reason))
    return True

library.admin_shutdown_init.argtypes = [ctypes.c_char_p, callback_type]
library.admin_shutdown_init.restype = ctypes.c_bool
library.admin_shutdown_cancel.restype = ctypes.c_bool
library.admin_shutdown_finish.argtypes = [ctypes.c_bool]
library.admin_shutdown_finish.restype = ctypes.c_bool
path = root / "server.sock"

def initialize():
    assert library.admin_shutdown_init(os.fsencode(path), schedule)
    assert stat.S_IMODE(path.stat().st_mode) == 0o600


def request(body):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
        peer.settimeout(1)
        peer.connect(str(path))
        peer.sendall(body)
        peer.shutdown(socket.SHUT_WR)
        for _ in range(3):
            library.admin_shutdown_poll()
        try:
            return peer.recv(1024)
        except ConnectionResetError:
            return b""


def command(identifier, seconds=60, reason=b"Restart soon"):
    return b"ATRINIK-ADMIN/1 SHUTDOWN " + identifier.encode() + b" " + str(seconds).encode() + b" " + reason + b"\n"


def receipt(identifier):
    target = Path(str(path) + "." + identifier + ".result")
    metadata = target.lstat()
    assert stat.S_ISREG(metadata.st_mode) and stat.S_IMODE(metadata.st_mode) == 0o600
    assert metadata.st_uid == os.geteuid()
    return target.read_bytes()

initialize()
if os.geteuid() != 0:
    assert request(b"ATRINIK-ADMIN/1 CAPABILITIES\n") == b""
    assert not calls
    library.admin_shutdown_deinit()
    print("PASS unauthorized non-root peer; root scheduling cases require the root fixture worker")
    sys.exit(0)

assert request(b"ATRINIK-ADMIN/1 CAPABILITIES\n") == b"ATRINIK-ADMIN/1 CAPABILITIES shutdown-v1 durable-result-v1\n"
# Fragmentation is accepted only after EOF; partial frames never schedule.
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as fragmented:
    fragmented.settimeout(0.03)
    fragmented.connect(str(path))
    for fragment in (b"ATRINIK-", b"ADMIN/1 ", b"CAPABILITIES\n"):
        fragmented.sendall(fragment)
        library.admin_shutdown_poll()
        assert not calls
    try:
        fragmented.recv(1024)
        raise AssertionError("endpoint replied before EOF")
    except TimeoutError:
        pass
    fragmented.shutdown(socket.SHUT_WR)
    library.admin_shutdown_poll()
    assert b"CAPABILITIES" in fragmented.recv(1024)

assert request(b"ATRINIK-ADMIN/2 CAPABILITIES\n") == b"ATRINIK-ADMIN/1 ERROR malformed\n"
assert request(b"x" * 1025) == b"ATRINIK-ADMIN/1 ERROR too-long\n"
first = "a" * 32
assert request(command(first)) == b"ATRINIK-ADMIN/1 SCHEDULED " + first.encode() + b"\n"
assert request(command(first)) == b"ATRINIK-ADMIN/1 SCHEDULED " + first.encode() + b"\n"
assert len(calls) == 1, "retry reset countdown"
assert request(command(first, 90)) == b"ATRINIK-ADMIN/1 ERROR busy\n"
assert request(command("b" * 32)) == b"ATRINIK-ADMIN/1 ERROR busy\n"
assert library.admin_shutdown_cancel()
assert receipt(first).endswith(b" cancelled\n")
assert request(command(first)) == b"ATRINIK-ADMIN/1 ERROR reused-id\n"

second = "b" * 32
assert b"SCHEDULED" in request(command(second))
library.admin_shutdown_expired()
assert library.admin_shutdown_finish(True)
assert receipt(second).endswith(b" saved\n")
third = "c" * 32
assert b"SCHEDULED" in request(command(third))
library.admin_shutdown_expired()
assert library.admin_shutdown_finish(False)
assert receipt(third).endswith(b" failed\n")
fourth = "d" * 32
assert b"SCHEDULED" in request(command(fourth))
assert library.admin_shutdown_finish(True)
assert receipt(fourth).endswith(b" cancelled\n"), "unrelated shutdown claimed success"

# A client that never terminates its request cannot retain the single slot.
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as slow:
    slow.connect(str(path))
    slow.sendall(b"ATRINIK")
    library.admin_shutdown_poll()
    time.sleep(2.1)
    library.admin_shutdown_poll()
assert b"CAPABILITIES" in request(b"ATRINIK-ADMIN/1 CAPABILITIES\n")

# A conflicting temp file cannot be replaced or interpreted as completion.
fifth = "e" * 32
assert b"SCHEDULED" in request(command(fifth))
conflict = Path(str(path) + "." + fifth + ".tmp")
conflict.write_text("preserve")
library.admin_shutdown_expired()
assert not library.admin_shutdown_finish(True)
assert conflict.read_text() == "preserve"
assert not Path(str(path) + "." + fifth + ".result").exists()
assert request(command("f" * 32)) == b"ATRINIK-ADMIN/1 ERROR persistence\n"
library.admin_shutdown_deinit()
assert not path.exists()

# Existing paths are never unlinked, and cleanup preserves replacements.
path.write_text("preserve")
assert not library.admin_shutdown_init(os.fsencode(path), schedule)
assert path.read_text() == "preserve"
path.unlink()
path.symlink_to(root / "elsewhere")
assert not library.admin_shutdown_init(os.fsencode(path), schedule)
assert path.is_symlink()
path.unlink()
initialize()
path.unlink()
path.write_text("replacement")
library.admin_shutdown_deinit()
assert path.read_text() == "replacement"
path.unlink()

unsafe = root / "unsafe"
unsafe.mkdir(mode=0o777)
unsafe.chmod(0o777)
assert not library.admin_shutdown_init(os.fsencode(unsafe / "server.sock"), schedule)
link = root / "link"
link.symlink_to(root, target_is_directory=True)
assert not library.admin_shutdown_init(os.fsencode(link / "server.sock"), schedule)
if hasattr(library, "admin_shutdown_fail_sync_for_test"):
    initialize()
    sixth = "f" * 32
    assert b"SCHEDULED" in request(command(sixth))
    library.admin_shutdown_expired()
    library.admin_shutdown_fail_sync_for_test(True)
    assert not library.admin_shutdown_finish(True)
    assert not Path(str(path) + "." + sixth + ".result").exists()
    library.admin_shutdown_fail_sync_for_test(False)
    library.admin_shutdown_deinit()
print("PASS root peer, exact protocol, bounds, retries, conflicts, timeout, cancellation, durable results, publication failures, and path fencing")
