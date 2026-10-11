#!/usr/bin/env python3
"""Canonicalize the sole mutable checksum field in the locked AppImage runtime."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct


def finalize(path: Path, runtime: dict) -> None:
    size = runtime['size']
    offset = runtime['digest_md5_offset']
    if (runtime.get('digest_policy') != 'zero-field-md5-v1'
            or type(size) is not int or not 64 <= size <= 16 * 1024**2
            or type(offset) is not int or not 64 <= offset <= size - 16
            or runtime.get('digest_md5_size') != 16):
        raise ValueError('unsupported locked runtime checksum contract')
    descriptor = os.open(path, os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, 'r+b') as stream:
        identity = os.fstat(stream.fileno())
        if not stat.S_ISREG(identity.st_mode) or not size < identity.st_size <= 2 * 1024**3:
            raise ValueError('AppImage must be a bounded regular runtime plus payload')
        prefix = bytearray(stream.read(size))
        prefix[offset:offset + 16] = bytes(16)
        if hashlib.sha256(prefix).hexdigest() != runtime['sha256']:
            raise ValueError('AppImage runtime differs outside its locked checksum field')
        # The header and section names are now bound by the trusted runtime hash.
        # Independently verify that the exact mutable range is the ELF checksum.
        section_table = struct.unpack_from('<Q', prefix, 40)[0]
        entry_size, count, names_index = struct.unpack_from('<HHH', prefix, 58)
        if entry_size != 64 or count > 4096 or names_index >= count or section_table + count * 64 > size:
            raise ValueError('invalid locked ELF section table')
        names_header = section_table + names_index * 64
        names_offset, names_size = struct.unpack_from('<QQ', prefix, names_header + 24)
        if names_offset + names_size > size:
            raise ValueError('invalid locked ELF string table')
        names = prefix[names_offset:names_offset + names_size]
        matches = []
        for index in range(count):
            header = section_table + index * 64
            name_offset = struct.unpack_from('<I', prefix, header)[0]
            name = names[name_offset:].split(b'\0', 1)[0]
            if name == b'.digest_md5':
                matches.append(struct.unpack_from('<QQ', prefix, header + 24))
        if matches != [(offset, 16)]:
            raise ValueError('locked checksum coordinates differ from ELF section')
        # appimagetool 1.9.1 src/digest.c hashes uninitialized skipped/past-EOF
        # bytes. Define deterministic checksum metadata instead of inheriting it:
        # https://github.com/AppImage/appimagetool/blob/1.9.1/src/digest.c
        # SHA-256 remains the authoritative integrity/provenance hash.
        digest = hashlib.md5(usedforsecurity=False)
        digest.update(prefix)
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
        current = os.fstat(stream.fileno())
        if (current.st_size, current.st_mtime_ns, current.st_ctime_ns) != (
                identity.st_size, identity.st_mtime_ns, identity.st_ctime_ns):
            raise ValueError('AppImage changed during checksum generation')
        stream.seek(offset)
        stream.write(digest.digest())
        stream.flush()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    args = parser.parse_args()
    lock = json.loads((Path(__file__).with_name('packaging.lock.json')).read_text())
    finalize(args.path, lock['runtime'])
