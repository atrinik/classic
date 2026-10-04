#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run with a real adapter/store shared library and an owned empty private directory."""
import ctypes
import json
import os
from pathlib import Path
import sys

library = ctypes.CDLL(sys.argv[1])
root = Path(sys.argv[2])
library.access_admin_parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p]
library.access_admin_parse.restype = ctypes.c_bool


def request(op='status', **fields):
    return dict(schema='atrinik-access-admin-v1', operation=op, requestId='a' * 32, **fields)


def encoded(value):
    return json.dumps(value, separators=(',', ':'), ensure_ascii=False).encode()


def parse(value):
    raw = encoded(value) if isinstance(value, dict) else value
    result = ctypes.create_string_buffer(1024)
    return library.access_admin_parse(raw, len(raw), result)


for value in (request(), request('issue', expectedRevision='0', label='Équipe "A" \\'),
              request('issue', expectedRevision=str(2**64 - 1), label='x', expiresAt='253402300799'),
              request('list', revision='0', cursor='1', limit=64),
              request('history', tokenId='b' * 32), request('revoke', tokenId='b' * 32, expectedRevision='0'),
              request('remove', tokenId='b' * 32, expectedRevision='0'), request('result', targetRequestId='b' * 32)):
    assert parse(value)
for value in (request(extra='x'), request('unknown'), request('issue', expectedRevision='00', label='x'),
              request('issue', expectedRevision=str(2**64), label='x'), request('issue', expectedRevision='0', label=''),
              request('issue', expectedRevision='0', label='x' * 129),
              request('issue', expectedRevision='0', label='x\n'), request('issue', expectedRevision='0', label='x\u202e'),
              request('issue', expectedRevision='0', label='x', expiresAt='0'),
              request('issue', expectedRevision='0', label='x', expiresAt='253402300800'),
              request('list', limit=0), request('list', limit=65), request('list', limit='1'),
              request('list', cursor='1'), request('history', tokenId='B' * 32), request('status', label='x'),
              request('result', targetRequestId='b' * 31), encoded(request()) + b'{}', encoded(request()) + b' ', encoded(request()) + b'\n',
              encoded(request())[:-1] + b',"requestId":"' + b'a' * 32 + b'"}',
              encoded(request()).replace(b'"operation"', b'"op\\u0065ration"')[:-1] + b',"operation":"status"}'):
    assert not parse(value), repr(value)
raw = encoded(request('issue', expectedRevision='0', label='x'))
for escaped in (b'\\u0000', b'\\ud800', b'\\udc00', b'\xc0\xaf', b'\xf4\x90\x80\x80', b'\xed\xa0\x80'):
    assert not parse(raw.replace(b'"x"', b'"' + escaped + b'"'))
for end in range(len(raw)):
    assert not parse(raw[:end])
assert parse(raw.replace(b'"x"', b'"\\ud83d\\ude00"'))

store = ctypes.c_void_p()
identity = (ctypes.c_ubyte * 32)(*range(32))
library.access_store_open.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_void_p, ctypes.c_bool, ctypes.c_bool]
library.access_store_open.restype = ctypes.c_int
library.access_store_close.argtypes = [ctypes.c_void_p]
route_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
route_calls = []

@route_type
def route(context, registration):
    route_calls.append(True)
    return 0  # Test-owned authenticated route acknowledgement, not network authority.

library.access_admin_execute.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
    route_type, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
library.access_admin_execute.restype = ctypes.c_bool
assert library.access_store_open(ctypes.byref(store), os.fsencode(root), identity, True, True) == 0


def execute(value, capacity=32769):
    data = encoded(value)
    output = ctypes.create_string_buffer(capacity)
    length = ctypes.c_size_t()
    ok = library.access_admin_execute(store, data, len(data), route, None, output, capacity, ctypes.byref(length))
    if not ok:
        assert length.value == 0
        return None
    assert 0 < length.value <= 32768
    result = json.loads(output.raw[:length.value])
    ctypes.memset(output, 0, capacity)
    return result


try:
    status = execute(request())
    assert status['result']['state'] == 'initialized' and status['result']['policy'] == 'protected'
    revision = status['revision']
    issue = request('issue', expectedRevision=revision, label='Fixture "équipe"')
    issued = execute(issue)
    assert issued['outcome'] == 'committed' and len(issued['result']['code']) == 16 and route_calls
    token_id = issued['result']['tokenId']
    code = issued['result'].pop('code')
    issued['result'].clear()
    repeated = execute(issue)
    assert repeated['outcome'] == 'already_committed_secret_unavailable' and 'code' not in repeated['result']
    recovered = execute(request('result', targetRequestId='a' * 32))
    assert recovered['outcome'] == 'already_committed_secret_unavailable' and 'code' not in recovered['result']
    listed = execute(request('list'))
    assert len(listed['result']['tokens']) == 1
    assert listed['result']['tokens'][0]['label'] == 'Fixture "équipe"'
    assert listed['result']['tokens'][0]['lastAdmittedAt'] is None
    history = execute(request('history', tokenId=token_id))
    assert history['result']['history'] == [] and code not in json.dumps(history)
    assert execute(request('list'), capacity=16) is None
    assert execute(request('list', revision=revision))['outcome'] == 'conflict'
    revoke = request('revoke', expectedRevision=listed['revision'], tokenId=token_id)
    revoke['requestId'] = 'b' * 32
    revoked = execute(revoke)
    assert revoked['outcome'] in {'committed', 'locally_revoked_route_pending'}
    assert 'code' not in revoked['result']
finally:
    library.access_store_close(store)
print('PASS strict native parser, real-store issue/list/history/revoke, durable result recovery and response bounds')
