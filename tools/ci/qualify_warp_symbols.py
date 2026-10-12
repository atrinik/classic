#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Hosted-only, offline symbol qualification for the fixed 554 Windows trace.

No process is attached, no game/WARP image is executed, and no symbols are fetched
by DbgHelp. Only the fixed Microsoft PDB and its one pinned storage hop are read.
Format: https://llvm.org/docs/PDB/MsfFile.html and PdbStream.html.
API: https://learn.microsoft.com/windows/win32/api/dbghelp/nf-dbghelp-symloadmoduleex
"""

import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


GUID = "5356169f5b76b468d395b180d53682f1"
AGE = 1
IMAGE_SIZE = 0x5AE000
RVAS = (0x671EB, 0x66CB5, 0x6A9D7, 0x6B25B, 0xB0526,
        0xA6000, 0x161903, 0x94C08, 0x94938)
URL = ("https://msdl.microsoft.com/download/symbols/d3d10warp.pdb/"
       + GUID.upper() + "1/d3d10warp.pdb")
# Observed from the canonical URL in hosted run38248071891/job114806917500.
# This is an exact host, not an Azure suffix allowlist.
SYMBOL_BLOB_HOST = "vsblobprodscussu5shard18.blob.core.windows.net"
MAX_PDB = 256 * 1024 * 1024
MAX_DIRECTORY = 4 * 1024 * 1024
MAX_INFO = 1024 * 1024
MAX_REPORT = 32 * 1024
DOWNLOAD_SECONDS = 90
WORKER_SECONDS = 120
REPORT = "warp-symbol-qualification.json"
MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"
TRACE = {
    "source": "554efe80e24e4196d04a507ae5bfa5902972bfce",
    "run": 38212864197,
    "job": 114692900077,
    "log_sha256": "afc1c543f2c125049156a26a1f6b219e3dd213a940775536b7981e0cb66abb16",
    "module": "d3d10warp.dll",
    "module_sha256": "633aa84f4963823346b3dd62ce285bb8745e7ce270cee2ea12d30f9ddec8df12",
    "module_timestamp": "68769840",
    "image_size": IMAGE_SIZE,
    "pdb": "d3d10warp.pdb",
    "guid": GUID,
    "age": AGE,
    "rvas": list(RVAS),
}


class Inconclusive(Exception):
    """Only fixed public reason codes may cross the report boundary."""

    def __init__(self, code, redirect_host=None):
        super().__init__(code)
        self.redirect_host = redirect_host


def require(condition, code):
    if not condition:
        raise Inconclusive(code)


def pdb_identity(data):
    """Validate MSF structure and read only the bounded PDB info stream."""
    require(56 <= len(data) <= MAX_PDB and data[:32] == MAGIC, "pdb_header")
    block, free, count, directory_size, reserved, block_map = struct.unpack_from(
        "<6I", data, 32)
    require(block in (512, 1024, 2048, 4096) and free in (1, 2)
            and reserved == 0 and count * block == len(data), "pdb_layout")
    require(12 <= directory_size <= MAX_DIRECTORY, "pdb_directory_size")
    directory_count = (directory_size + block - 1) // block
    require(directory_count * 4 <= block, "pdb_directory_map_size")

    def valid_page(page):
        return 0 < page < count and page % block not in (1, 2)

    require(valid_page(block_map), "pdb_block_map")
    directory_pages = struct.unpack_from(
        "<" + "I" * directory_count, data, block_map * block)
    used = {0, block_map}

    def claim(pages):
        for page in pages:
            require(valid_page(page) and page not in used, "pdb_page_overlap_or_range")
            used.add(page)

    claim(directory_pages)
    directory = b"".join(data[p * block:(p + 1) * block]
                         for p in directory_pages)[:directory_size]
    streams = struct.unpack_from("<I", directory)[0]
    require(4 <= streams <= 262144 and 4 + streams * 4 <= len(directory),
            "pdb_stream_count")
    sizes = struct.unpack_from("<" + "I" * streams, directory, 4)
    require(28 <= sizes[1] <= MAX_INFO, "pdb_info_size")
    require(64 <= sizes[3] <= MAX_PDB, "pdb_dbi_size")
    cursor = 4 + streams * 4
    info_pages = ()
    dbi_header = None
    for index, size in enumerate(sizes):
        require(size == 0xFFFFFFFF or size <= MAX_PDB, "pdb_stream_size")
        pages = 0 if size == 0xFFFFFFFF else (size + block - 1) // block
        require(cursor + pages * 4 <= len(directory), "pdb_stream_pages")
        indices = struct.unpack_from("<" + "I" * pages, directory, cursor)
        claim(indices)
        if index == 1:
            info_pages = indices
        elif index == 3:
            # Only the fixed DBI header is needed; do not materialize its stream.
            start = indices[0] * block
            dbi_header = data[start:start + 64]
        cursor += pages * 4
    require(cursor == len(directory), "pdb_directory_trailing_data")
    info = b"".join(data[p * block:(p + 1) * block]
                    for p in info_pages)[:sizes[1]]
    version, _signature, info_age = struct.unpack_from("<3I", info)
    require(version == 20000404, "pdb_info_version")
    guid = uuid.UUID(bytes_le=info[12:28]).hex
    signature, dbi_version, dbi_age = struct.unpack_from("<iII", dbi_header)
    require(signature == -1 and dbi_version == 19990903, "pdb_dbi_version")
    # Microsoft PDB1::OpenValidate4: Info age may advance without relinking;
    # DBI age identifies the image. Require the modern exact match, not its
    # legacy DBI-age-zero exception. Expected PE GUID/age remain unchanged.
    # https://github.com/microsoft/microsoft-pdb/blob/master/PDB/dbi/pdb.cpp
    require(guid == GUID and info_age >= AGE and dbi_age == AGE,
            "pdb_identity_mismatch")
    return {"guid": guid, "age": dbi_age, "info_age": info_age}


class SymbolRedirect(urllib.request.HTTPRedirectHandler):
    def __init__(self):
        self.accepted_url = None

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # Retain only a bounded HTTPS DNS hostname for separate endpoint review.
        # Never retain a signed query, path, userinfo, fragment, or headers.
        hostname = None
        parsed = None
        try:
            parsed = urllib.parse.urlsplit(newurl)
            candidate = parsed.hostname or ""
            if (parsed.scheme == "https" and len(candidate) <= 253
                    and re.fullmatch(r"[A-Za-z0-9]+(?:[A-Za-z0-9.-]*[A-Za-z0-9])?", candidate)):
                hostname = candidate.lower()
        except ValueError:
            pass
        if (req is not None and req.full_url == URL and req.get_method() == "GET"
                and code in (301, 302, 303, 307, 308) and self.accepted_url is None
                and parsed is not None and len(newurl) <= 4096
                and re.fullmatch(r"[\x21-\x7e]+", newurl)
                and "\\" not in newurl and parsed.scheme == "https"
                and parsed.netloc == SYMBOL_BLOB_HOST and parsed.path.startswith("/")
                and "#" not in newurl):
            # The signed storage query is opaque and remains in memory only.
            # Construct fresh headers: never forward authorization or cookies.
            self.accepted_url = newurl
            return urllib.request.Request(newurl, headers={"Accept-Encoding": "identity"},
                                          method="GET")
        raise Inconclusive("download_redirect_rejected", hostname)

    def http_error_302(self, req, fp, code, msg, headers):
        # urllib's default handler drains the redirect body without a byte cap.
        # We need none of that body; close it before the sole permitted hop.
        try:
            locations = headers.get_all("Location", [])
            require(len(locations) == 1, "download_redirect_location")
            redirected = self.redirect_request(req, fp, code, msg, headers, locations[0])
        finally:
            fp.close()
        return self.parent.open(redirected, timeout=req.timeout)

    http_error_301 = http_error_302
    http_error_303 = http_error_302
    http_error_307 = http_error_302
    http_error_308 = http_error_302


def make_opener():
    # Do not inherit proxy credentials or install HTTP authentication handlers.
    redirect = SymbolRedirect()
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), redirect,
        urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    opener.symbol_redirect = redirect
    return opener


def download_pdb(destination, *, opener=None, clock=time.monotonic):
    started = clock()
    opener = make_opener() if opener is None else opener
    request = urllib.request.Request(URL, headers={"Accept-Encoding": "identity"})
    digest = hashlib.sha256()
    total = 0
    with opener.open(request, timeout=10) as response:
        redirect = getattr(opener, "symbol_redirect", None)
        accepted_url = redirect.accepted_url if isinstance(redirect, SymbolRedirect) else None
        require(response.status == 200 and (response.geturl() == URL
                or (accepted_url is not None and response.geturl() == accepted_url)),
                "download_status_or_url")
        require(response.headers.get("Content-Encoding", "identity") == "identity",
                "download_encoding")
        length = response.headers.get("Content-Length")
        require(length is None or (re.fullmatch(r"[0-9]{1,10}", length)
                                   and 56 <= int(length) <= MAX_PDB),
                "download_content_length")
        with destination.open("xb") as output:
            while True:
                require(clock() - started < DOWNLOAD_SECONDS, "download_deadline")
                # One underlying read per iteration, so a trickling peer cannot
                # hide indefinitely inside read(n)'s fill loop. The parent is
                # the definitive hard deadline, including native symbol work.
                chunk = response.read1(min(65536, MAX_PDB - total + 1))
                require(clock() - started < DOWNLOAD_SECONDS, "download_deadline")
                if not chunk:
                    break
                total += len(chunk)
                require(total <= MAX_PDB, "download_size")
                output.write(chunk)
                digest.update(chunk)
        require(total >= 56 and (length is None or int(length) == total),
                "download_truncated")
    return {"bytes": total, "sha256": digest.hexdigest()}


class SymbolInfo(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", ctypes.c_uint32), ("TypeIndex", ctypes.c_uint32),
        ("Reserved", ctypes.c_uint64 * 2), ("Index", ctypes.c_uint32),
        ("Size", ctypes.c_uint32), ("ModBase", ctypes.c_uint64),
        ("Flags", ctypes.c_uint32), ("Value", ctypes.c_uint64),
        ("Address", ctypes.c_uint64), ("Register", ctypes.c_uint32),
        ("Scope", ctypes.c_uint32), ("Tag", ctypes.c_uint32),
        ("NameLen", ctypes.c_uint32), ("MaxNameLen", ctypes.c_uint32),
        ("Name", ctypes.c_char * 1),
    ]


def symbol_record(rva, name, address, module_base, displacement):
    base = 0x10000000
    require(rva in RVAS and module_base == base
            and base <= address <= base + rva
            and displacement == base + rva - address, "symbol_address")
    require(0 < len(name) <= 512 and re.fullmatch(rb"[A-Za-z0-9_?$@:.<>~,+*()&= !\[\]-]+", name),
            "symbol_name")
    return {"rva": rva, "symbol": name.decode("ascii"),
            "symbol_rva": address - base, "displacement": displacement}


def resolve_symbols(pdb):
    require(sys.platform == "win32" and ctypes.sizeof(ctypes.c_void_p) == 8,
            "requires_windows_x64")
    # LOAD_LIBRARY_SEARCH_SYSTEM32; never search the checkout/current directory.
    dbg = ctypes.WinDLL("dbghelp.dll", winmode=0x800, use_last_error=True)
    kernel = ctypes.WinDLL("kernel32.dll", winmode=0x800, use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    process = kernel.GetCurrentProcess()
    dbg.SymSetOptions.argtypes = [ctypes.c_uint32]
    dbg.SymSetOptions.restype = ctypes.c_uint32
    dbg.SymInitializeW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    dbg.SymInitializeW.restype = ctypes.c_int
    dbg.SymLoadModuleExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint64, ctypes.c_uint32,
        ctypes.c_void_p, ctypes.c_uint32]
    dbg.SymLoadModuleExW.restype = ctypes.c_uint64
    dbg.SymFromAddr.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SymbolInfo)]
    dbg.SymFromAddr.restype = ctypes.c_int
    dbg.SymCleanup.argtypes = [ctypes.c_void_p]
    dbg.SymCleanup.restype = ctypes.c_int
    # EXACT_SYMBOLS | FAIL_CRITICAL_ERRORS | NO_PROMPTS | IGNORE_NT_SYMPATH.
    # No deferred loading, process invasion, remote search path, or symsrv.
    dbg.SymSetOptions(0x400 | 0x200 | 0x80000 | 0x1000)
    require(dbg.SymInitializeW(process, str(pdb.parent), False), "symbol_initialize")
    try:
        base = 0x10000000
        require(dbg.SymLoadModuleExW(process, None, str(pdb), None,
                                    base, IMAGE_SIZE, None, 0) == base,
                "symbol_load")
        records = []
        for rva in RVAS:
            buffer = ctypes.create_string_buffer(ctypes.sizeof(SymbolInfo) + 512)
            pointer = ctypes.cast(buffer, ctypes.POINTER(SymbolInfo))
            pointer.contents.SizeOfStruct = ctypes.sizeof(SymbolInfo)
            pointer.contents.MaxNameLen = 513
            displacement = ctypes.c_uint64()
            require(dbg.SymFromAddr(process, base + rva, ctypes.byref(displacement),
                                   pointer), "symbol_unresolved")
            info = pointer.contents
            require(0 < info.NameLen <= 512, "symbol_name_length")
            name = ctypes.string_at(ctypes.addressof(buffer) + SymbolInfo.Name.offset,
                                    info.NameLen)
            records.append(symbol_record(rva, name, info.Address, info.ModBase,
                                         displacement.value))
        return records
    finally:
        require(dbg.SymCleanup(process), "symbol_cleanup")


def qualify(directory):
    result = {"status": "inconclusive", "trace": TRACE,
              "scope": "Offline names for one sampled stack; no shutdown or JIT attribution."}
    try:
        pdb = directory / "d3d10warp.pdb"
        result["download"] = download_pdb(pdb)
        result["identity"] = pdb_identity(pdb.read_bytes())
        result["symbols"] = resolve_symbols(pdb)
        result["status"] = "resolved"
    except Inconclusive as error:
        result["reason"] = str(error)
        if error.redirect_host is not None:
            result["redirect_hostname"] = error.redirect_host
    except (urllib.error.URLError, OSError, ValueError, struct.error):
        result["reason"] = "acquisition_or_native_error"
    return result


def write_report(path, result):
    encoded = json.dumps(result, sort_keys=True, indent=2) + "\n"
    require(len(encoded.encode("ascii")) <= MAX_REPORT, "report_size")
    path.write_text(encoded, encoding="ascii")


def main():
    # This command is deliberately unavailable as a general local symbol fetcher.
    require(sys.platform == "win32" and os.environ.get("GITHUB_ACTIONS") == "true"
            and os.environ.get("RUNNER_OS") == "Windows", "requires_hosted_windows")
    if sys.argv[1:] == ["--worker"]:
        write_report(Path(REPORT), qualify(Path.cwd()))
        return 0
    require(not sys.argv[1:], "no_overrides")
    result = {"status": "inconclusive", "trace": TRACE, "reason": "worker_failed"}
    with tempfile.TemporaryDirectory(prefix="atrinik-warp-symbols-") as temporary:
        environment = {key: value for key, value in os.environ.items()
                       if key.upper() not in ("_NT_SYMBOL_PATH", "_NT_ALT_SYMBOL_PATH")
                       and "TOKEN" not in key.upper() and "SECRET" not in key.upper()}
        try:
            completed = subprocess.run(
                [sys.executable, "-I", str(Path(__file__).resolve()), "--worker"],
                cwd=temporary, env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=WORKER_SECONDS, check=False)
            report = Path(temporary) / REPORT
            if completed.returncode == 0 and report.is_file() and report.stat().st_size <= MAX_REPORT:
                result = json.loads(report.read_text(encoding="ascii"))
        except subprocess.TimeoutExpired:
            result["reason"] = "worker_deadline"
        except (OSError, ValueError):
            result["reason"] = "worker_report_error"
    write_report(Path(REPORT), result)
    print(json.dumps(result, sort_keys=True))
    return 0 if result["status"] == "resolved" else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Inconclusive as failure:
        print(json.dumps({"status": "inconclusive", "reason": str(failure)}))
        sys.exit(1)
