#!/usr/bin/env python3
"""Derive the content key that names prebuilt AppImage builder/runtime images.

The key covers every regular file of a prepared build context (its relative
path and SHA-256) plus the graphical Dockerfile, which is built from the same
context. A prebuilt image is reused only when the release prepares exactly the
same inputs, so a different recipe, lock, compiler image or downloaded input
always selects a different key.
"""
import argparse
import hashlib
from pathlib import Path
import sys

SCHEMA = 'atrinik-appimage-context-v1'
TARGETS = ('build', 'smoke-ubuntu24', 'smoke-ubuntu26', 'smoke-debian13', 'graphical')


def file_digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def context_key(context, graphical):
    context = Path(context)
    graphical = Path(graphical)
    if context.is_symlink() or not context.is_dir():
        raise ValueError('prepared context must be a regular directory')
    if graphical.is_symlink() or not graphical.is_file():
        raise ValueError('graphical Dockerfile must be a regular file')
    entries = []
    for path in sorted(context.rglob('*')):
        relative = path.relative_to(context).as_posix()
        if path.is_symlink():
            raise ValueError(f'prepared context contains a symlink: {relative}')
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError(f'prepared context contains a non-regular file: {relative}')
        entries.append(f'context/{relative}\0{file_digest(path)}\n')
    if not entries:
        raise ValueError('prepared context is empty')
    entries.append(f'graphical.Dockerfile\0{file_digest(graphical)}\n')
    hasher = hashlib.sha256(f'{SCHEMA}\n'.encode())
    for entry in entries:
        hasher.update(entry.encode())
    return hasher.hexdigest()


def image_reference(repository, target, key):
    if target not in TARGETS:
        raise ValueError(f'unknown AppImage image target: {target}')
    return f'{repository}:{target}-{key}'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--context', type=Path, required=True)
    parser.add_argument('--graphical-dockerfile', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        print(context_key(args.context, args.graphical_dockerfile))
    except ValueError as error:
        print(f'context key failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
