# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure fixtures: no network, system DLLs, or application execution."""

import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import uuid


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "warp_symbols", ROOT / "tools/ci/qualify_warp_symbols.py")
warp = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(warp)


def fixture(block=4096, guid=warp.GUID, age=warp.AGE):
    # Header, two FPMs, blockmap, directory, info stream.
    data = bytearray(block * 6)
    data[:32] = warp.MAGIC
    struct.pack_into("<6I", data, 32, block, 1, 6, 16, 0, 3)
    struct.pack_into("<I", data, block * 3, 4)
    struct.pack_into("<4I", data, block * 4, 2, 0xFFFFFFFF, 28, 5)
    struct.pack_into("<3I", data, block * 5, 20000404, 0, age)
    data[block * 5 + 12:block * 5 + 28] = uuid.UUID(hex=guid).bytes_le
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
                                 {"guid": warp.GUID, "age": 1})

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
        struct.pack_into("<I", data, 44, 20)
        struct.pack_into("<5I", data, 4 * 4096, 2, 1, 28, 5, 5)
        with self.assertRaisesRegex(warp.Inconclusive, "overlap"):
            warp.pdb_identity(bytes(data))

    def test_noncontiguous_directory_and_info(self):
        data = bytearray(fixture())
        # Stream 1 crosses pages; the second page precedes its first on disk.
        data += bytes(4096)
        struct.pack_into("<I", data, 40, 7)
        struct.pack_into("<I", data, 44, 20)
        struct.pack_into("<5I", data, 4 * 4096, 2, 0xFFFFFFFF, 4097, 6, 5)
        data[6 * 4096:6 * 4096 + 28] = data[5 * 4096:5 * 4096 + 28]
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
            warp.NoRedirect().redirect_request(None, None, 302, "", {}, warp.URL)

    def test_redirect_evidence_is_hostname_only(self):
        handler = warp.NoRedirect()
        for url, expected in (("https://user:secret@symbols.example.invalid/private?token=secret#x",
                               "symbols.example.invalid"),
                              ("http://example.invalid/token", None),
                              ("https://bad%2fhost/private", None),
                              ("https://[invalid/private", None)):
            with self.assertRaises(warp.Inconclusive) as error:
                handler.redirect_request(None, None, 302, "", {}, url)
            self.assertEqual(error.exception.redirect_host, expected)
            self.assertEqual(str(error.exception), "download_redirect_rejected")

    def test_opener_has_no_proxy_or_auth_handlers(self):
        opener = warp.make_opener()
        self.assertFalse(any(isinstance(h, warp.urllib.request.ProxyHandler)
                             and h.proxies for h in opener.handlers))
        self.assertFalse(any("Auth" in type(h).__name__ for h in opener.handlers))
        self.assertTrue(any(isinstance(h, warp.NoRedirect) for h in opener.handlers))


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
