#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Isolated strict protocol and secret-output tests; no service state."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('access_admin_cli', Path(__file__).parents[2] / 'tools' / 'access_admin.py')
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)


class AdminClientTest(unittest.TestCase):
    def setUp(self):
        self.request = {'schema': cli.SCHEMA, 'operation': 'status', 'requestId': 'a' * 32}
        self.response = dict(self.request, outcome='committed', revision=None, result={
            'state': 'absent_open', 'schemaVersion': 1, 'serverIdentity': 'b' * 64, 'policy': 'open'})

    def exchange(self, wire):
        client, peer = socket.socketpair()
        def server():
            with peer:
                while peer.recv(4096):
                    pass
                try:
                    peer.sendall(wire)
                except BrokenPipeError:
                    pass
        worker = threading.Thread(target=server)
        worker.start()
        try:
            with client:
                return cli.exchange(client, self.request)
        finally:
            worker.join(2)
            self.assertFalse(worker.is_alive())

    def frame(self, body):
        if isinstance(body, dict):
            body = json.dumps(body, separators=(',', ':')).encode()
        return cli.PREFIX + str(len(body)).encode() + b'\n' + body

    def test_status_tagged_union(self):
        self.assertEqual(self.exchange(self.frame(self.response)), self.response)
        self.response['revision'] = '0'
        self.response['result'] = {'state': 'initialized', 'schemaVersion': 1,
            'serverIdentity': 'b' * 64, 'policy': 'protected', 'integrity': 'ok',
            'durability': 'ok', 'revision': '0', 'pendingRouteSync': 0}
        self.assertEqual(self.exchange(self.frame(self.response)), self.response)
        self.response['result']['pendingRouteSync'] = 33
        with self.assertRaises(cli.ProtocolError):
            self.exchange(self.frame(self.response))

    def test_framing_rejects(self):
        good = self.frame(self.response)
        for bad in (good + b'x', good[:-1], good.replace(b'ACCESS ', b'ACCESS 0', 1),
                    cli.PREFIX + b'32769\n', cli.PREFIX + b'0\n',
                    b'ATRINIK-ADMIN/1 ERROR malformed\n', self.frame(b'\xff')):
            with self.subTest(wire=bad[:35]), self.assertRaises(cli.ProtocolError):
                self.exchange(bad)

    def test_strict_json_identity(self):
        raw = json.dumps(self.response, separators=(',', ':')).encode()
        for body in (raw[:-1] + b',"requestId":"' + b'a' * 32 + b'"}',
                     raw.replace(b'"state":', b'"extra":0,"state":'),
                     raw.replace(b'a' * 32, b'c' * 32), raw + b'{}',
                     raw.replace(b'"schemaVersion":1', b'"schemaVersion":true')):
            with self.subTest(body=body[:30]), self.assertRaises(cli.ProtocolError):
                self.exchange(self.frame(body))

    def test_output_descriptor(self):
        with tempfile.NamedTemporaryFile() as file:
            os.fchmod(file.fileno(), 0o600)
            fd = cli.output_descriptor(file.fileno())
            os.close(fd)
            os.fchmod(file.fileno(), 0o640)
            with self.assertRaises(cli.ProtocolError):
                cli.output_descriptor(file.fileno())
        with self.assertRaises(cli.ProtocolError):
            cli.output_descriptor(1)
        with mock.patch.object(cli.os, 'isatty', return_value=False), self.assertRaises(cli.ProtocolError):
            cli.output_descriptor(None)

    def test_issue_code_only_on_explicit_fd(self):
        code = '0123456789ABCDEF'  # Independent, nonfunctional fixture value.
        request_id = 'a' * 32
        response = {'schema': cli.SCHEMA, 'operation': 'issue', 'requestId': request_id,
                    'outcome': 'committed', 'revision': '2', 'result': {'tokenId': 'b' * 32,
                    'tokenRevision': '1', 'routePending': False, 'code': code}}
        with tempfile.NamedTemporaryFile() as file:
            os.fchmod(file.fileno(), 0o600)
            with mock.patch.object(cli.os, 'geteuid', return_value=0), \
                 mock.patch.object(cli, 'output_descriptor', return_value=os.dup(file.fileno())), \
                 mock.patch.object(cli, 'verified_connection', return_value=contextlib.nullcontext(object())), \
                 mock.patch.object(cli, 'exchange', return_value=response), \
                 contextlib.redirect_stdout(io.StringIO()) as stdout, \
                 contextlib.redirect_stderr(io.StringIO()) as stderr:
                result = cli.main(['--socket', '/private/socket', '--server-uid', '10001',
                    '--server-pid', '42', '--request-id', request_id, 'issue', '--label',
                    'Fixture', '--expected-revision', '1', '--code-fd', str(file.fileno())])
            self.assertEqual(result, 0)
            self.assertNotIn(code, stdout.getvalue() + stderr.getvalue())
            file.seek(0)
            self.assertEqual(file.read(), (code + '\n').encode())

    def test_committed_issue_requires_code_and_revision(self):
        request = dict(self.request, operation='issue')
        response = dict(request, outcome='committed', revision='1', result={
            'tokenId': 'b' * 32, 'tokenRevision': '1', 'routePending': False})
        with self.assertRaises(cli.ProtocolError):
            cli.validate_response(response, request)
        response['result']['code'] = '0123456789ABCDEF'
        self.assertEqual(cli.validate_response(response, request), response)
        response['revision'] = None
        with self.assertRaises(cli.ProtocolError):
            cli.validate_response(response, request)

    def test_code_forbidden_outside_initial_issue(self):
        for op in ('result', 'revoke', 'remove'):
            request = dict(self.request, operation=op)
            response = dict(request, outcome='committed', revision='1', result={
                'tokenId': 'b' * 32, 'tokenRevision': '1', 'routePending': False,
                'code': '0123456789ABCDEF'})
            with self.assertRaises(cli.ProtocolError):
                cli.validate_response(response, request)


if __name__ == '__main__':
    unittest.main()
