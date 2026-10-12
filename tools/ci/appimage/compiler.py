#!/usr/bin/env python3
"""Verify the LLVM cohort copied into the distribution packaging producer."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


IMAGE_PATTERN = r'ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}'


def image_reference(value):
    if not re.fullmatch(IMAGE_PATTERN, value):
        raise ValueError('compiler image must be an immutable published classic-build reference')
    return value


def verify(prefix, compiler):
    manifest = json.loads((prefix / 'share/atrinik/llvm-toolchain.json').read_text())
    if any(manifest.get(key) != compiler[key] for key in ('version', 'prefix', 'source', 'defaults', 'tools')):
        raise ValueError('compiler producer identity differs from the locked LLVM cohort')
    for name in compiler['tools']:
        tool = prefix / 'bin' / name
        if not tool.is_file():
            raise ValueError(f'compiler producer has a missing LLVM tool: {name}')
        output = subprocess.check_output([str(prefix / 'bin' / name), '--version'], text=True, stderr=subprocess.STDOUT, timeout=30)
        if not re.search(r'(?:[Vv]ersion|LLD) 23\.1\.3(?:[^0-9.]|$)', output) and not (name == 'llvm-config' and output.strip() == '23.1.3'):
            raise ValueError(f'compiler producer has a mismatched LLVM tool: {name}')
    for name, expected in compiler['notices'].items():
        notice = prefix / 'share/licenses/llvm' / name
        if notice.is_symlink() or not notice.is_file() or hashlib.sha256(notice.read_bytes()).hexdigest() != expected:
            raise ValueError(f'compiler producer has a missing or changed notice: {name}')
    source = prefix / 'share/atrinik/sources/llvm-project-23.1.3.src.tar.xz'
    with source.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != compiler['source']['sha256']:
            raise ValueError('compiler producer source archive differs from the locked LLVM source')


if __name__ == '__main__':
    lock = json.loads(Path(sys.argv[1]).read_text())
    verify(Path(lock['compiler']['prefix']), lock['compiler'])
