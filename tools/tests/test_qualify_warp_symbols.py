# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure fixtures: no network, system DLLs, or application execution."""

import importlib.util
from email.message import Message
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import uuid
import urllib.response


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "warp_symbols", ROOT / "tools/ci/qualify_warp_symbols.py")
warp = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(warp)


def fixture(block=4096, guid=warp.GUID, age=warp.AGE, dbi_age=None):
    # Header, two FPMs, blockmap, directory, info and DBI streams.
    data = bytearray(block * 7)
    data[:32] = warp.MAGIC
    struct.pack_into("<6I", data, 32, block, 1, 7, 28, 0, 3)
    struct.pack_into("<I", data, block * 3, 4)
    struct.pack_into("<7I", data, block * 4, 4, 0xFFFFFFFF, 28, 0xFFFFFFFF, 64, 5, 6)
    struct.pack_into("<3I", data, block * 5, 20000404, 0, age)
    data[block * 5 + 12:block * 5 + 28] = uuid.UUID(hex=guid).bytes_le
    struct.pack_into("<iII", data, block * 6, -1, 19990903,
                     age if dbi_age is None else dbi_age)
    return bytes(data)


class Response(io.BytesIO):
    status = 200

    def __init__(self, data, headers=None, url=warp.URL):
        super().__init__(data)
        self.headers = {} if headers is None else headers
        self.url = url

    def geturl(self):
        return self.url


class PdbTests(unittest.TestCase):
    def test_supported_blocks_and_little_endian_identity(self):
        for block in (512, 1024, 2048, 4096):
            with self.subTest(block=block):
                self.assertEqual(warp.pdb_identity(fixture(block)),
                                 {"guid": warp.GUID, "age": 1, "info_age": 1})

    def test_public_pdb_info_age_can_advance_but_dbi_must_match(self):
        self.assertEqual(warp.pdb_identity(fixture(age=3, dbi_age=1)),
                         {"guid": warp.GUID, "age": 1, "info_age": 3})
        for info_age, dbi_age in ((0, 1), (3, 0), (3, 2), (1, 2)):
            with self.subTest(info_age=info_age, dbi_age=dbi_age):
                with self.assertRaisesRegex(warp.Inconclusive, "identity_mismatch"):
                    warp.pdb_identity(fixture(age=info_age, dbi_age=dbi_age))

    def test_missing_truncated_or_unsupported_dbi(self):
        for offset, value in ((4 * 4096, 3), (4 * 4096 + 16, 0xFFFFFFFF),
                              (4 * 4096 + 16, 63), (6 * 4096, 0),
                              (6 * 4096 + 4, 0), (4 * 4096 + 24, 5)):
            data = bytearray(fixture())
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset, value=value):
                with self.assertRaises(warp.Inconclusive):
                    warp.pdb_identity(bytes(data))

    def test_identity_mismatch(self):
        for data in (fixture(guid="0" * 32), fixture(age=2)):
            with self.assertRaisesRegex(warp.Inconclusive, "identity_mismatch"):
                warp.pdb_identity(data)

    def test_truncation_and_header(self):
        for data in (b"", b"x" * 56, fixture()[:-1], fixture() + b"x"):
            with self.assertRaises(warp.Inconclusive):
                warp.pdb_identity(data)

    def test_malformed_coordinates(self):
        cases = ((32, 0), (36, 3), (40, 0xFFFFFFFF), (44, 0xFFFFFFFF),
                 (48, 1), (52, 6), (52, 1), (3 * 4096, 3),
                 (3 * 4096, 6), (4 * 4096, 0xFFFFFFFF),
                 (4 * 4096 + 8, 0xFFFFFFFF), (4 * 4096 + 12, 4),
                 (4 * 4096 + 12, 6), (5 * 4096, 0))
        for offset, value in cases:
            with self.subTest(offset=offset, value=value):
                data = bytearray(fixture())
                struct.pack_into("<I", data, offset, value)
                with self.assertRaises(warp.Inconclusive):
                    warp.pdb_identity(bytes(data))

    def test_duplicate_stream_pages(self):
        data = bytearray(fixture())
        struct.pack_into("<I", data, 44, 32)
        struct.pack_into("<8I", data, 4 * 4096, 4, 1, 28, 0xFFFFFFFF, 64, 5, 5, 6)
        with self.assertRaisesRegex(warp.Inconclusive, "overlap"):
            warp.pdb_identity(bytes(data))

    def test_noncontiguous_directory_and_info(self):
        data = bytearray(fixture())
        # Stream 1 crosses pages; the second page precedes its first on disk.
        data += bytes(4096)
        struct.pack_into("<I", data, 40, 8)
        struct.pack_into("<I", data, 44, 32)
        struct.pack_into("<8I", data, 4 * 4096, 4, 0xFFFFFFFF, 4097, 0xFFFFFFFF, 64, 7, 5, 6)
        data[7 * 4096:7 * 4096 + 28] = data[5 * 4096:5 * 4096 + 28]
        self.assertEqual(warp.pdb_identity(bytes(data))["guid"], warp.GUID)


class DownloadTests(unittest.TestCase):
    def download(self, response, clock=lambda: 0):
        opener = mock.Mock()
        opener.open.return_value = response
        with tempfile.TemporaryDirectory() as directory:
            result = warp.download_pdb(Path(directory) / "fixture.pdb",
                                       opener=opener, clock=clock)
        request = opener.open.call_args.args[0]
        self.assertEqual(request.full_url, warp.URL)
        self.assertEqual(request.header_items(), [("Accept-encoding", "identity")])
        return result

    def test_bounded_download_with_and_without_length(self):
        for headers in ({}, {"Content-Length": "56"}):
            self.assertEqual(self.download(Response(b"x" * 56, headers))["bytes"], 56)

    def test_reject_status_url_encoding_and_length(self):
        cases = [Response(b"x" * 56, url="https://example.invalid/private"),
                 Response(b"x" * 56, {"Content-Encoding": "gzip"})]
        for length in ("-1", "abc", "55", str(warp.MAX_PDB + 1), "9999999999999", "57"):
            cases.append(Response(b"x" * 56, {"Content-Length": length}))
        bad_status = Response(b"x" * 56)
        bad_status.status = 404
        cases.append(bad_status)
        for response in cases:
            with self.assertRaises(warp.Inconclusive):
                self.download(response)

    def test_stream_hard_cap(self):
        with mock.patch.object(warp, "MAX_PDB", 64):
            with self.assertRaisesRegex(warp.Inconclusive, "download_size"):
                self.download(Response(b"x" * 65))

    def test_total_deadline_checked_after_read(self):
        clock = mock.Mock(side_effect=[0, 0, 91])
        with self.assertRaisesRegex(warp.Inconclusive, "deadline"):
            self.download(Response(b"x" * 56), clock)

    def test_redirect_fails_closed(self):
        with self.assertRaisesRegex(warp.Inconclusive, "redirect_rejected"):
            warp.SymbolRedirect().redirect_request(None, None, 302, "", {}, warp.URL)

    def test_redirect_evidence_is_hostname_only(self):
        handler = warp.SymbolRedirect()
        for url, expected in (("https://user:secret@symbols.example.invalid/private?token=secret#x",
                               "symbols.example.invalid"),
                              ("http://example.invalid/token", None),
                              ("https://bad%2fhost/private", None),
                              ("https://[invalid/private", None)):
            with self.assertRaises(warp.Inconclusive) as error:
                handler.redirect_request(None, None, 302, "", {}, url)
            self.assertEqual(error.exception.redirect_host, expected)
            self.assertEqual(str(error.exception), "download_redirect_rejected")

    def test_exact_storage_redirect_once_without_forwarded_credentials(self):
        handler = warp.SymbolRedirect()
        destination = "https://" + warp.SYMBOL_BLOB_HOST + "/opaque/pdb?sig=fixture"
        request = warp.urllib.request.Request(warp.URL, headers={"Authorization": "fixture",
                                                                 "Cookie": "fixture"})
        redirected = handler.redirect_request(request, None, 302, "", {}, destination)
        self.assertEqual(redirected.full_url, destination)
        self.assertEqual(redirected.header_items(), [("Accept-encoding", "identity")])
        with self.assertRaises(warp.Inconclusive):
            handler.redirect_request(redirected, None, 302, "", {}, destination)
        with self.assertRaises(warp.Inconclusive):
            handler.redirect_request(request, None, 302, "", {}, destination)

    def test_storage_redirect_negative_controls(self):
        host = warp.SYMBOL_BLOB_HOST
        allowed = "https://" + host + "/pdb?sig=fixture"
        destinations = ["http://" + host + "/pdb", "https://other.blob.core.windows.net/pdb",
                        "https://" + host + ".evil.invalid/pdb", "https://user:secret@" + host + "/pdb",
                        "https://" + host + ":443/pdb", "https://" + host + "/pdb#fragment",
                        "https://" + host + "/pdb#",
                        "https://" + host + "/pdb\n?sig=secret", "https://" + host + "/\\pdb",
                        "https://" + host + "/" + "x" * 4096, "https://[invalid/pdb", "/relative"]
        for destination in destinations:
            with self.subTest(destination=destination[:80]):
                with self.assertRaises(warp.Inconclusive):
                    warp.SymbolRedirect().redirect_request(
                        warp.urllib.request.Request(warp.URL), None, 302, "", {}, destination)
        for source, method, status in ((warp.URL + "?x=1", "GET", 302),
                                       (warp.URL, "POST", 302), (warp.URL, "GET", 300)):
            with self.assertRaises(warp.Inconclusive):
                warp.SymbolRedirect().redirect_request(
                    warp.urllib.request.Request(source, method=method), None, status, "", {}, allowed)

    def test_redirect_body_closed_without_read_and_timeout_preserved(self):
        handler = warp.SymbolRedirect()
        handler.parent = mock.Mock()
        request = warp.urllib.request.Request(warp.URL)
        request.timeout = 10
        headers = Message()
        headers["Location"] = "https://" + warp.SYMBOL_BLOB_HOST + "/pdb?sig=fixture"
        body = mock.Mock()
        handler.http_error_302(request, body, 302, "", headers)
        body.close.assert_called_once()
        body.read.assert_not_called()
        self.assertEqual(handler.parent.open.call_args.kwargs, {"timeout": 10})
        for duplicated in (False, True):
            headers = Message()
            if duplicated:
                headers["Location"] = warp.URL
                headers["Location"] = warp.URL
            body = mock.Mock()
            with self.assertRaises(warp.Inconclusive):
                handler.http_error_302(request, body, 302, "", headers)
            body.close.assert_called_once()

    def test_final_storage_response_must_match_admitted_hop(self):
        destination = "https://" + warp.SYMBOL_BLOB_HOST + "/pdb?sig=fixture"
        for admitted in (False, True):
            opener = mock.Mock()
            opener.symbol_redirect = warp.SymbolRedirect()
            if admitted:
                opener.symbol_redirect.redirect_request(
                    warp.urllib.request.Request(warp.URL), None, 302, "", {}, destination)
            opener.open.return_value = Response(b"x" * 56, url=destination)
            with tempfile.TemporaryDirectory() as temporary:
                path = Path(temporary) / "pdb"
                if admitted:
                    result = warp.download_pdb(path, opener=opener)
                    self.assertEqual(result["bytes"], 56)
                    self.assertNotIn("sig", json.dumps(result))
                else:
                    with self.assertRaises(warp.Inconclusive):
                        warp.download_pdb(path, opener=opener)

    def test_real_opener_redirect_dispatch_with_offline_transport(self):
        destination = "https://" + warp.SYMBOL_BLOB_HOST + "/fixture?sig=private-fixture"
        observed = []

        class OfflineHTTPS(warp.urllib.request.HTTPSHandler):
            def https_open(self, request):
                observed.append((request.full_url, request.header_items()))
                headers = Message()
                if len(observed) == 1:
                    headers["Location"] = destination
                    response = urllib.response.addinfourl(
                        io.BytesIO(b"unused redirect body"), headers, request.full_url, 302)
                else:
                    response = urllib.response.addinfourl(
                        io.BytesIO(b"x" * 56), headers, request.full_url, 200)
                response.msg = "fixture"
                return response

        redirect = warp.SymbolRedirect()
        opener = warp.urllib.request.build_opener(
            warp.urllib.request.ProxyHandler({}), redirect, OfflineHTTPS())
        opener.symbol_redirect = redirect
        with tempfile.TemporaryDirectory() as temporary:
            result = warp.download_pdb(Path(temporary) / "pdb", opener=opener)
        self.assertEqual(result["bytes"], 56)
        self.assertEqual([url for url, _ in observed], [warp.URL, destination])
        self.assertFalse(any(name.lower() in ("authorization", "cookie")
                             for name, _ in observed[1][1]))
        self.assertNotIn("private-fixture", json.dumps(result))

    def test_opener_has_no_proxy_or_auth_handlers(self):
        opener = warp.make_opener()
        self.assertFalse(any(isinstance(h, warp.urllib.request.ProxyHandler)
                             and h.proxies for h in opener.handlers))
        self.assertFalse(any("Auth" in type(h).__name__ for h in opener.handlers))
        self.assertTrue(any(isinstance(h, warp.SymbolRedirect) for h in opener.handlers))


class ResultTests(unittest.TestCase):
    def test_symbol_info_x64_layout(self):
        self.assertEqual(warp.ctypes.sizeof(warp.SymbolInfo), 88)
        self.assertEqual(warp.SymbolInfo.Name.offset, 84)
        self.assertEqual(warp.SymbolInfo.Address.offset, 56)

    def test_symbol_address_and_privacy(self):
        rva = warp.RVAS[0]
        base = 0x10000000
        self.assertEqual(warp.symbol_record(rva, b"WARP::Compile", base + rva - 4,
                                           base, 4)["displacement"], 4)
        for name in (b"", b"x" * 513, b"C:\\private\\file", b"/home/name", b"x\ny", b"\xff"):
            with self.assertRaises(warp.Inconclusive):
                warp.symbol_record(rva, name, base + rva, base, 0)
        for address, module, delta in ((base - 1, base, 0), (base + rva + 1, base, 0),
                                        (base, base + 1, rva), (base, base, 0)):
            with self.assertRaises(warp.Inconclusive):
                warp.symbol_record(rva, b"name", address, module, delta)

    def test_identity_failure_never_loads_native(self):
        def downloaded(path):
            path.write_bytes(fixture(age=2))
            return {"bytes": path.stat().st_size, "sha256": "fixture"}
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(warp, "download_pdb", side_effect=downloaded), \
                    mock.patch.object(warp, "resolve_symbols") as native:
                result = warp.qualify(Path(directory))
        self.assertEqual(result["status"], "inconclusive")
        self.assertEqual(result["reason"], "pdb_identity_mismatch")
        native.assert_not_called()

    def test_unknown_symbol_is_inconclusive(self):
        def downloaded(path):
            path.write_bytes(fixture())
            return {"bytes": path.stat().st_size, "sha256": "fixture"}
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(warp, "download_pdb", side_effect=downloaded), \
                    mock.patch.object(warp, "resolve_symbols", side_effect=warp.Inconclusive("symbol_unresolved")):
                result = warp.qualify(Path(directory))
        self.assertEqual(result["status"], "inconclusive")
        self.assertEqual(result["reason"], "symbol_unresolved")
        self.assertNotIn("symbols", result)

    def test_native_error_does_not_leak_exception(self):
        with mock.patch.object(warp, "download_pdb", side_effect=OSError("private/path/token")):
            result = warp.qualify(Path("unused"))
        self.assertNotIn("private", json.dumps(result))
        self.assertEqual(result["reason"], "acquisition_or_native_error")

    def test_report_cap(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(warp.Inconclusive, "report_size"):
                warp.write_report(Path(directory) / "report", {"x": "x" * warp.MAX_REPORT})

    def test_local_execution_is_rejected_without_network(self):
        with mock.patch.object(warp.sys, "platform", "linux"), \
                mock.patch.object(warp, "download_pdb") as download:
            with self.assertRaisesRegex(warp.Inconclusive, "requires_hosted_windows"):
                warp.main()
        download.assert_not_called()

    def test_worker_timeout_retains_report_and_removes_private_files(self):
        directories = []

        def timeout(command, **kwargs):
            directory = Path(kwargs["cwd"])
            directories.append(directory)
            (directory / "d3d10warp.pdb").write_bytes(b"private fixture")
            self.assertEqual(kwargs["timeout"], 120)
            self.assertEqual(command[1], "-I")
            self.assertNotIn("_NT_SYMBOL_PATH", kwargs["env"])
            self.assertNotIn("GITHUB_TOKEN", kwargs["env"])
            raise warp.subprocess.TimeoutExpired(command, 120)

        with tempfile.TemporaryDirectory() as output:
            target = Path(output) / "report.json"
            with mock.patch.object(warp.sys, "platform", "win32"), \
                    mock.patch.object(warp.sys, "argv", ["helper"]), \
                    mock.patch.dict(warp.os.environ, {"GITHUB_ACTIONS": "true", "RUNNER_OS": "Windows",
                                                      "_NT_SYMBOL_PATH": "private", "GITHUB_TOKEN": "fixture"}), \
                    mock.patch.object(warp.subprocess, "run", side_effect=timeout), \
                    mock.patch.object(warp, "REPORT", str(target)), \
                    mock.patch("builtins.print"):
                self.assertEqual(warp.main(), 1)
            self.assertEqual(json.loads(target.read_text())["reason"], "worker_deadline")
        self.assertEqual(len(directories), 1)
        self.assertFalse(directories[0].exists())

    def test_successful_worker_cleanup_and_report(self):
        directories = []
        report = {"status": "resolved", "trace": warp.TRACE, "symbols": []}

        def completed(command, **kwargs):
            directory = Path(kwargs["cwd"])
            directories.append(directory)
            (directory / "d3d10warp.pdb").write_bytes(b"private fixture")
            (directory / warp.REPORT).write_text(json.dumps(report))
            return warp.subprocess.CompletedProcess(command, 0)

        with mock.patch.object(warp.sys, "platform", "win32"), \
                mock.patch.object(warp.sys, "argv", ["helper"]), \
                mock.patch.dict(warp.os.environ, {"GITHUB_ACTIONS": "true", "RUNNER_OS": "Windows"}), \
                mock.patch.object(warp.subprocess, "run", side_effect=completed), \
                mock.patch.object(warp, "write_report") as write, \
                mock.patch("builtins.print"):
            self.assertEqual(warp.main(), 0)
        self.assertEqual(write.call_args.args[1], report)
        self.assertFalse(directories[0].exists())

    def test_workflow_preserves_default_rehearsal_and_artifact_scope(self):
        text = (ROOT / ".github/workflows/release-rehearsal.yml").read_text()
        self.assertIn("type: boolean\n        required: false\n        default: false", text)
        self.assertIn("preflight:\n    if: ${{ inputs.warp_symbols_only != true }}", text)
        self.assertIn("rehearse:\n    if: ${{ inputs.warp_symbols_only != true }}", text)
        diagnostic = text.split("  warp-symbols:\n", 1)[1]
        self.assertIn("if: ${{ inputs.warp_symbols_only == true }}", diagnostic)
        self.assertIn("permissions:\n      contents: read", diagnostic)
        self.assertIn("persist-credentials: false", diagnostic)
        self.assertIn("ref: ${{ github.sha }}", diagnostic)
        self.assertIn("path: warp-symbol-qualification.json", diagnostic)
        self.assertNotIn("secrets:", diagnostic)
        self.assertNotIn("build-release-candidate", diagnostic)
        self.assertNotIn("smoke_windows_review_bundle", diagnostic)


if __name__ == "__main__":
    unittest.main()
