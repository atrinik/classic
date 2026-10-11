#!/usr/bin/env python3
"""Materialize hash-locked, public AppImage build inputs (no Docker required)."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import re
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'release'))
from resolve_compiler_image import HISTORICAL_APPIMAGE_FILES, HISTORICAL_IMAGE


def historical_recipe():
    root = Path(__file__).resolve().parent / 'historical'
    if root.is_symlink() or not root.is_dir():
        raise ValueError('retained historical producer must be a regular directory')
    for name, expected in HISTORICAL_APPIMAGE_FILES.items():
        path = root / name
        if path.is_symlink() or not path.is_file() or digest(path) != expected:
            raise ValueError(f'retained historical producer integrity mismatch: {name}')
    return root


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def download(entry, destination, cache):
    expected = entry['sha256']
    cached = cache / expected
    if cached.exists() and digest(cached) != expected:
        raise ValueError(f"corrupt cached input: {entry['name']}")
    if not cached.exists():
        fd, temporary = tempfile.mkstemp(dir=cache)
        os.close(fd)
        try:
            with urllib.request.urlopen(entry['url'], timeout=120) as response, open(temporary, 'wb') as output:
                shutil.copyfileobj(response, output)
            if digest(Path(temporary)) != expected:
                raise ValueError(f"SHA256 mismatch: {entry['name']}")
            os.replace(temporary, cached)
        finally:
            Path(temporary).unlink(missing_ok=True)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(cached, destination)


def select_packaging_lock(trusted_lock, image, source_lock_path=None):
    """Preserve exact release lock bytes only for the supported SDK recipe."""
    if not re.fullmatch(r'ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}', image):
        raise ValueError('--compiler-image must be an immutable published classic-build reference')
    if source_lock_path is None:
        if trusted_lock['compiler'].get('image') is not None and image != trusted_lock['compiler']['image']:
            raise ValueError('--compiler-image differs from the trusted packaging lock')
        return trusted_lock, None
    source_fd = os.open(source_lock_path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(source_fd, 'rb') as stream:
        identity = os.fstat(stream.fileno())
        if not stat.S_ISREG(identity.st_mode) or identity.st_size > 4 * 1024 * 1024:
            raise ValueError('source packaging lock must be a bounded regular file')
        data = stream.read(4 * 1024 * 1024 + 1)
        if len(data) > 4 * 1024 * 1024:
            raise ValueError('source packaging lock exceeds inspection limits')
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate key in source packaging lock')
            result[key] = value
        return result
    source_lock = json.loads(data, object_pairs_hook=pairs)
    if not isinstance(source_lock, dict):
        raise ValueError('source packaging lock requires a compiler descriptor')
    if 'compiler' not in source_lock:
        if image != HISTORICAL_IMAGE or hashlib.sha256(data).hexdigest() != HISTORICAL_APPIMAGE_FILES['packaging.lock.json']:
            raise ValueError('source packaging lock differs from the supported pre-LLVM recipe')
        historical_recipe()
        return source_lock, data
    if not isinstance(source_lock['compiler'], dict):
        raise ValueError('source packaging lock requires a compiler descriptor')
    if source_lock['compiler'].get('image') != image:
        raise ValueError('source packaging lock compiler image differs from --compiler-image')
    def recipe(lock):
        return dict(lock, compiler={key: value for key, value in lock['compiler'].items() if key != 'image'})
    if json.dumps(recipe(source_lock), sort_keys=True) != json.dumps(recipe(trusted_lock), sort_keys=True):
        raise ValueError('source packaging lock differs from the supported SDK recipe or LLVM cohort')
    return source_lock, data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cache', type=Path)
    parser.add_argument('--compiler-image', required=True)
    parser.add_argument('--source-packaging-lock', type=Path,
                        help='exact release lock bytes extracted by the trusted resolver')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    original_lock = (root / 'packaging.lock.json').read_text()
    trusted_lock = json.loads(original_lock)
    try:
        publication_lock, selected_lock_bytes = select_packaging_lock(trusted_lock, args.compiler_image, args.source_packaging_lock)
        recipe_root = root if 'compiler' in publication_lock else historical_recipe()
    except (OSError, ValueError, UnicodeError) as error:
        parser.error(str(error))
    lock = publication_lock
    if args.output.resolve() == root:
        parser.error('--output must be a separate generated context directory')
    args.output.mkdir(parents=True, exist_ok=True)
    cache = args.cache or args.output / '.cache'
    cache.mkdir(parents=True, exist_ok=True)
    jobs = []
    for entry in lock['runtime_recipes']:
        jobs.append((entry, args.output / 'runtime-recipes' / entry['filename']))
    for entry in lock['runtime_sources']:
        jobs.append((entry, args.output / 'runtime-sources' / entry['filename']))
    for entry in lock['sources']:
        jobs.append((entry, args.output / 'sources' / (entry['name'] + '.tar.gz')))
    for entry in lock['tools'] + [dict(lock['runtime'], name='runtime')] + lock['bootstrap'] + lock['license_inputs']:
        jobs.append((entry, args.output / 'tools' / entry['name']))
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda job: download(job[0], job[1], cache), jobs))
    input_keys = ('sources', 'tools', 'runtime', 'license_inputs', 'runtime_sources', 'base_image')
    if 'compiler' in lock:
        input_keys += ('compiler',)
    (args.output / 'dependency-inputs.lock.json').write_text(json.dumps({k: lock[k] for k in input_keys}, sort_keys=True) + '\n')
    (args.output / 'compiler-image.txt').write_text(args.compiler_image + '\n')
    for name in ('runtime-probe.c', 'graphical-probe.c'):
        probe = recipe_root / name
        if probe.is_file() and probe.resolve() != (args.output / probe.name).resolve():
            shutil.copyfile(probe, args.output / probe.name)
    for name in ('build-dependencies.sh',) + (('compiler.py',) if 'compiler' in lock else ()):
        if (recipe_root / name).resolve() != (args.output / name).resolve():
            shutil.copyfile(recipe_root / name, args.output / name)
    (args.output / 'packaging.lock.json').write_bytes(selected_lock_bytes if selected_lock_bytes is not None else original_lock.encode())
    dockerfile = (recipe_root / 'Dockerfile').read_bytes()
    if 'compiler' in lock:
        dockerfile = dockerfile.replace(b'FROM ${ATRINIK_COMPILER_IMAGE} AS compiler', f'FROM {args.compiler_image} AS compiler'.encode())
    (args.output / 'Dockerfile').write_bytes(dockerfile)


if __name__ == '__main__':
    main()
