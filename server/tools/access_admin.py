#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Root-local Classic access administration. Issuance codes never use stdout."""
import argparse
import fcntl
import json
import os
import re
import secrets
import socket
import stat
import struct
import sys
import time

SCHEMA = 'atrinik-access-admin-v1'
PREFIX = b'ATRINIK-ADMIN/1 ACCESS '
HEX = re.compile(r'[0-9a-f]{32}\Z')
DECIMAL = re.compile(r'(?:0|[1-9][0-9]*)\Z')
OUTCOMES = {'committed', 'pending', 'locally_revoked_route_pending', 'conflict',
            'denied', 'unavailable', 'save_failed', 'indeterminate',
            'already_committed_secret_unavailable', 'limit', 'invalid', 'not_found'}


class ProtocolError(Exception):
    pass


def decimal(value):
    if not isinstance(value, str) or not DECIMAL.fullmatch(value) or int(value) > 2**64 - 1:
        raise ProtocolError('invalid decimal')
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProtocolError('duplicate response key')
        result[key] = value
    return result


def keys(value, required, optional=()):
    if not isinstance(value, dict) or not set(required) <= value.keys() or value.keys() - set(required) - set(optional):
        raise ProtocolError('invalid response fields')


def token(value, history=False):
    required = {'tokenId', 'revision', 'label', 'createdAt', 'expiresAt', 'state', 'routePending', 'lastAdmittedAt'}
    if history:
        required.add('history')
    keys(value, required)
    if not isinstance(value['tokenId'], str) or not HEX.fullmatch(value['tokenId']):
        raise ProtocolError('invalid token identity')
    if not isinstance(value['label'], str) or len(value['label'].encode('utf-8')) > 128:
        raise ProtocolError('invalid label')
    if value['state'] not in {'pending', 'active', 'revoked', 'expired'} or type(value['routePending']) is not bool:
        raise ProtocolError('invalid token state')
    for field in ('revision', 'createdAt'):
        decimal(value[field])
    for field in ('expiresAt', 'lastAdmittedAt'):
        if value[field] is not None:
            decimal(value[field])
    if history:
        if not isinstance(value['history'], list) or len(value['history']) > 16:
            raise ProtocolError('invalid history')
        for timestamp in value['history']:
            decimal(timestamp)


def validate_response(value, request):
    keys(value, {'schema', 'operation', 'requestId', 'outcome', 'result', 'revision'})
    if any(value[field] != request[field] for field in ('schema', 'operation', 'requestId')):
        raise ProtocolError('response identity mismatch')
    if value['outcome'] not in OUTCOMES:
        raise ProtocolError('invalid outcome')
    if value['revision'] is not None:
        decimal(value['revision'])
    result = value['result']
    op = request['operation']
    if value['revision'] is None and not (value['outcome'] == 'unavailable' or (op == 'status' and isinstance(result, dict) and result.get('state') == 'absent_open')):
        raise ProtocolError('missing store revision')
    if value['outcome'] == 'unavailable' and value['revision'] is None:
        keys(result, set())
    elif op == 'status':
        if isinstance(result, dict) and result.get('state') == 'absent_open':
            keys(result, {'state', 'schemaVersion', 'serverIdentity', 'policy'})
            if value['revision'] is not None or (result['policy'] != 'open' and value['outcome'] == 'committed'):
                raise ProtocolError('invalid absent store')
        else:
            keys(result, {'state', 'schemaVersion', 'serverIdentity', 'policy', 'integrity', 'durability', 'revision', 'pendingRouteSync'})
            if result['state'] != 'initialized':
                raise ProtocolError('invalid store state')
            decimal(result['revision'])
            if result['revision'] != value.get('revision') or result['integrity'] not in {'ok', 'failed'} or result['durability'] not in {'ok', 'indeterminate'}:
                raise ProtocolError('invalid store status')
            if type(result['pendingRouteSync']) is not int or not 0 <= result['pendingRouteSync'] <= 1024:
                raise ProtocolError('invalid pending route count')
        if type(result['schemaVersion']) is not int or result['schemaVersion'] != 1 or result['policy'] not in {'open', 'protected'} or not isinstance(result['serverIdentity'], str) or not re.fullmatch('[0-9a-f]{64}', result['serverIdentity']):
            raise ProtocolError('invalid store identity')
    elif op == 'list':
        keys(result, {'tokens', 'cursor'})
        if not isinstance(result['tokens'], list) or len(result['tokens']) > 64:
            raise ProtocolError('invalid token page')
        for row in result['tokens']:
            token(row)
        if result['cursor'] is not None:
            if not isinstance(result['cursor'], str) or len(result['cursor']) > 128 or not result['cursor'].isascii():
                raise ProtocolError('invalid cursor')
    elif op == 'history':
        if value['outcome'] == 'committed':
            token(result, history=True)
        else:
            keys(result, set())
    elif not result and value['outcome'] == 'unavailable' and value['revision'] is None:
        keys(result, set())
    else:
        keys(result, {'tokenId', 'tokenRevision', 'routePending'}, {'code'} if op == 'issue' and value['outcome'] == 'committed' else set())
        if not isinstance(result['tokenId'], str) or (result['tokenId'] and not HEX.fullmatch(result['tokenId'])) or type(result['routePending']) is not bool:
            raise ProtocolError('invalid mutation result')
        decimal(result['tokenRevision'])
        if op == 'issue' and value['outcome'] == 'committed' and 'code' not in result:
            raise ProtocolError('missing issuance code')
        if 'code' in result and (not isinstance(result['code'], str) or not re.fullmatch('[0123456789ABCDEFGHJKMNPQRSTVWXYZ]{16}', result['code'])):
            raise ProtocolError('invalid issuance result')
    return value


def verified_connection(path, uid, pid):
    """Pin no-follow path descriptors, then authenticate the actual kernel peer."""
    parts = path.split('/')
    if not path.startswith('/') or any(part in {'', '.', '..'} for part in parts[1:]):
        raise ProtocolError('socket path must be canonical absolute')
    directory = os.open('/', os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    client = None
    try:
        for part in parts[1:-1]:
            next_dir = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=directory)
            os.close(directory)
            directory = next_dir
            info = os.fstat(directory)
            if info.st_uid not in {0, uid} or info.st_mode & 0o022:
                raise ProtocolError('unsafe socket ancestor')
        parent = os.fstat(directory)
        if parent.st_uid != uid or stat.S_IMODE(parent.st_mode) != 0o700:
            raise ProtocolError('unsafe socket directory')
        endpoint = os.stat(parts[-1], dir_fd=directory, follow_symlinks=False)
        if not stat.S_ISSOCK(endpoint.st_mode) or endpoint.st_uid != uid or stat.S_IMODE(endpoint.st_mode) != 0o600:
            raise ProtocolError('unsafe socket endpoint')
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(2)
        client.connect(f'/proc/self/fd/{directory}/{parts[-1]}')
        actual_pid, actual_uid, _ = struct.unpack('3i', client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i')))
        fresh = os.stat(parts[-1], dir_fd=directory, follow_symlinks=False)
        if (actual_uid, actual_pid) != (uid, pid) or (fresh.st_dev, fresh.st_ino) != (endpoint.st_dev, endpoint.st_ino):
            raise ProtocolError('server peer identity mismatch')
        return client
    except BaseException:
        if client is not None:
            client.close()
        raise
    finally:
        os.close(directory)


def exchange(client, request):
    wire = PREFIX + json.dumps(request, ensure_ascii=False, separators=(',', ':')).encode('utf-8') + b'\n'
    if len(wire) > 1024:
        raise ProtocolError('request exceeds limit')
    deadline = time.monotonic() + 35

    def recv(count):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ProtocolError('response deadline')
        client.settimeout(remaining)
        return client.recv(count)

    client.sendall(wire)
    client.shutdown(socket.SHUT_WR)
    header = bytearray()
    while not header.endswith(b'\n'):
        chunk = recv(1)
        if not chunk or len(header) >= 32:
            raise ProtocolError('invalid response frame')
        header.extend(chunk)
    match = re.fullmatch(rb'ATRINIK-ADMIN/1 ACCESS ([1-9][0-9]{0,4})\n', header)
    if not match or int(match[1]) > 32768:
        raise ProtocolError('invalid response length')
    size = int(match[1])
    body = bytearray()
    while len(body) < size:
        chunk = recv(size - len(body))
        if not chunk:
            raise ProtocolError('short response')
        body.extend(chunk)
    if recv(1):
        raise ProtocolError('trailing response')
    try:
        value = json.loads(body.decode('utf-8'), object_pairs_hook=unique_object,
                           parse_constant=lambda _: (_ for _ in ()).throw(ProtocolError('invalid JSON number')))
        return validate_response(value, request)
    except (ValueError, UnicodeError, TypeError, RecursionError) as exc:
        raise ProtocolError('invalid response JSON') from exc
    finally:
        body[:] = b'\0' * len(body)


def output_descriptor(number):
    if number is None:
        if not os.isatty(0):
            raise ProtocolError('issuance requires interactive terminal input')
        fd = os.open('/dev/tty', os.O_WRONLY | os.O_NOCTTY | os.O_CLOEXEC)
        if not os.isatty(fd):
            os.close(fd)
            raise ProtocolError('issuance requires a controlling terminal')
        return fd
    if number <= 2:
        raise ProtocolError('code FD must be separately preopened')
    fd = os.dup(number)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDONLY:
        os.close(fd)
        raise ProtocolError('code FD must be a preopened owner-only writable regular file')
    return fd


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', required=True)
    parser.add_argument('--server-uid', type=int, required=True)
    parser.add_argument('--server-pid', type=int, required=True)
    parser.add_argument('--request-id')
    sub = parser.add_subparsers(dest='operation', required=True)
    issue = sub.add_parser('issue')
    issue.add_argument('--label', required=True)
    issue.add_argument('--expected-revision', required=True)
    issue.add_argument('--expires-at')
    issue.add_argument('--code-fd', type=int)
    listing = sub.add_parser('list')
    listing.add_argument('--cursor')
    listing.add_argument('--revision')
    listing.add_argument('--limit', type=int, default=64)
    history = sub.add_parser('history')
    history.add_argument('--token-id', required=True)
    history.add_argument('--revision')
    for op in ('revoke', 'remove'):
        mutate = sub.add_parser(op)
        mutate.add_argument('--token-id', required=True)
        mutate.add_argument('--expected-revision', required=True)
    sub.add_parser('status')
    result = sub.add_parser('result')
    result.add_argument('--target-request-id', required=True)
    args = parser.parse_args(argv)
    request = {'schema': SCHEMA, 'operation': args.operation, 'requestId': args.request_id or secrets.token_hex(16)}
    fd = None
    try:
        if os.geteuid() != 0 or args.server_uid < 0 or args.server_pid <= 0:
            raise ProtocolError('root and explicit positive server identity required')
        if not HEX.fullmatch(request['requestId']):
            raise ProtocolError('invalid request ID')
        for field in ('expected_revision', 'label', 'expires_at', 'token_id', 'cursor', 'revision', 'limit', 'target_request_id'):
            value = getattr(args, field, None)
            if value is not None:
                name = field.split('_')[0] + ''.join(piece.title() for piece in field.split('_')[1:])
                request[name] = value
        for field in ('expectedRevision', 'revision', 'expiresAt'):
            if field in request:
                decimal(request[field])
        for field in ('tokenId', 'targetRequestId'):
            if field in request and not HEX.fullmatch(request[field]):
                raise ProtocolError('invalid token/request identity')
        if args.operation == 'issue':
            fd = output_descriptor(args.code_fd)
        print(f"requestId={request['requestId']}", file=sys.stderr, flush=True)
        with verified_connection(args.socket, args.server_uid, args.server_pid) as client:
            response = exchange(client, request)
        code = response['result'].pop('code', None)
        if code is not None:
            data = bytearray((code + '\n').encode('ascii'))
            try:
                while data:
                    written = os.write(fd, data)
                    if written <= 0:
                        raise ProtocolError('code output failed')
                    del data[:written]
                if not os.isatty(fd):
                    os.fsync(fd)
            finally:
                data[:] = b'\0' * len(data)
                code = None
        print(json.dumps(response, ensure_ascii=True, separators=(',', ':')))
        return 0 if response['outcome'] == 'committed' else 1
    except (OSError, ProtocolError) as exc:
        # Never interpolate server bytes or secret-bearing Python exceptions.
        print('Access request failed; inspect the retained result using targetRequestId=' + request['requestId'] + '. Never repeat issuance under a new ID to recover a lost response.', file=sys.stderr)
        return 2
    finally:
        if fd is not None:
            os.close(fd)


if __name__ == '__main__':
    raise SystemExit(main())
