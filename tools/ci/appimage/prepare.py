#!/usr/bin/env python3
"""Materialize hash-locked, public AppImage build inputs (no Docker required)."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import urllib.request


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cache', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    lock = json.loads((root / 'packaging.lock.json').read_text())
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
    (args.output / 'dependency-inputs.lock.json').write_text(json.dumps({k: lock[k] for k in ('sources', 'tools', 'runtime', 'license_inputs', 'runtime_sources')}, sort_keys=True) + '\n')
    probe = root / 'runtime-probe.c'
    if probe.is_file() and probe.resolve() != (args.output / probe.name).resolve():
        shutil.copyfile(probe, args.output / probe.name)
    for name in ('Dockerfile', 'build-dependencies.sh', 'packaging.lock.json'):
        if (root / name).resolve() != (args.output / name).resolve():
            shutil.copyfile(root / name, args.output / name)


if __name__ == '__main__':
    main()
