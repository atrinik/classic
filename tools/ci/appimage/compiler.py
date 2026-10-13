#!/usr/bin/env python3
"""Verify the LLVM cohort copied into the distribution packaging producer."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


IMAGE_PATTERN = r'ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}'
PINNED_INSPECTOR = Path('/opt/llvm-23.1.3/bin/llvm-readelf')
VALIDATOR_INSPECTOR = Path('/usr/bin/llvm-readelf')


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


def provision_inspector():
    """Provide the validator's fixed coordinate in the trusted root-owned image."""
    if os.path.lexists(VALIDATOR_INSPECTOR):
        raise ValueError(f'trusted inspector coordinate already exists: {VALIDATOR_INSPECTOR}')
    if not PINNED_INSPECTOR.is_file() or not os.access(PINNED_INSPECTOR, os.X_OK):
        raise ValueError(f'pinned LLVM inspector is not executable: {PINNED_INSPECTOR}')
    # Exclusive creation also rejects a coordinate claimed since the check.
    # The image builder owns /usr/bin; candidate containers cannot change it.
    VALIDATOR_INSPECTOR.symlink_to(PINNED_INSPECTOR)
    # Exercise exactly the absolute coordinate and environment policy used by
    # tools/release/appimage.py, rather than relying on the producer's PATH.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('LD_', 'GIT_')) and k not in {'PYTHONPATH', 'PYTHONHOME'}}
    env.update(PATH='/usr/bin:/bin', LC_ALL='C', TZ='UTC', GIT_NO_REPLACE_OBJECTS='1')
    try:
        result = subprocess.run([str(VALIDATOR_INSPECTOR), '--version'], capture_output=True,
                                env=env, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ValueError('trusted LLVM inspector preflight failed') from exc
    if result.returncode or not re.search(rb'\bLLVM version 23\.1\.3(?:[^0-9.]|$)', result.stdout):
        raise ValueError('trusted inspector does not execute the pinned LLVM 23.1.3 cohort')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('lock', type=Path)
    parser.add_argument('--provision-inspector', action='store_true')
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text())
    verify(Path(lock['compiler']['prefix']), lock['compiler'])
    if args.provision_inspector:
        provision_inspector()


if __name__ == '__main__':
    main()
