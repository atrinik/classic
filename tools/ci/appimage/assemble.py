#!/usr/bin/env python3
"""Assemble the complete application closure from the locked Ubuntu builder."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


def sha256(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(*args: str) -> str:
    return subprocess.check_output(args, text=True, timeout=120)


def dynamic(path: Path) -> tuple[list[str], str | None]:
    result = run('readelf', '-dW', str(path))
    needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', result)
    soname = re.findall(r'\(SONAME\).*?\[(.*?)\]', result)
    return needed, soname[0] if soname else None


def assemble(appdir: Path, prefix: Path, version: str, revision: str) -> None:
    lock_path = HERE / 'packaging.lock.json'
    lock = json.loads(lock_path.read_text())
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('ATRINIK_SOURCE_REVISION must be a full source commit')
    # The image's actual dependency/tool recipe must equal the reviewed lock.
    prepared_lock = prefix / 'packaging.lock.json'
    if sha256(prepared_lock) != sha256(lock_path):
        raise ValueError('dependency image packaging lock differs from source lock')
    libdir = appdir / 'usr/lib'
    # linuxdeploy may leave version aliases or intentionally omitted libraries.
    # Materialize one flat, audited SONAME provider set from the pinned builder.
    if libdir.exists():
        shutil.rmtree(libdir)
    libdir.mkdir(parents=True)
    docdir = appdir / 'usr/share/doc/atrinik'
    metadir = appdir / 'usr/share/atrinik'
    metadir.mkdir(parents=True, exist_ok=True)
    if (appdir / 'AppRun').is_symlink():
        (appdir / 'AppRun').unlink()
    shutil.copyfile(HERE / 'AppRun', appdir / 'AppRun')
    (appdir / 'AppRun').chmod(0o755)
    shutil.copyfile(HERE / 'openssl.cnf', metadir / 'openssl.cnf')
    shutil.copyfile(ROOT / 'client/ca-bundle.crt', metadir / 'ca-bundle.crt')
    shutil.copyfile(prefix / 'bin/openssl', appdir / 'usr/bin/openssl')
    (appdir / 'usr/bin/openssl').chmod(0o755)
    modules = prefix / 'lib/ossl-modules'
    if not (modules / 'legacy.so').is_file():
        raise ValueError('OpenSSL legacy provider is missing')
    shutil.copytree(modules, libdir / 'ossl-modules', dirs_exist_ok=True)
    shutil.copytree(prefix / 'share/licenses', docdir / 'licenses', dirs_exist_ok=True)
    # Debian copyright stanzas can refer to these complete standard texts.
    shutil.copytree('/usr/share/common-licenses', docdir / 'licenses/system/common-licenses',
                    dirs_exist_ok=True)
    shutil.copyfile('/build-packages.tsv', metadir / 'build-packages.tsv')
    shutil.copyfile(lock_path, metadir / 'packaging.lock.json')
    shutil.copytree('/usr/share/alsa', appdir / 'usr/share/alsa', dirs_exist_ok=True)
    desktop = (appdir / 'usr/share/applications/atrinik.desktop').read_text()
    desktop = desktop.replace('Icon=atrinik.png', 'Icon=atrinik')
    (appdir / 'atrinik.desktop').write_text(desktop)
    (appdir / 'usr/share/applications/atrinik.desktop').write_text(desktop)
    shutil.copyfile(appdir / 'usr/share/pixmaps/atrinik.png', appdir / 'atrinik.png')
    icon = appdir / '.DirIcon'
    if icon.is_symlink() or icon.exists():
        icon.unlink()
    icon.symlink_to('atrinik.png')

    available: dict[str, Path] = {}
    # System loader inventory is trusted builder output, never payload execution.
    for line in run('/sbin/ldconfig', '-p').splitlines():
        match = re.match(r'\s*(\S+) \(.*x86-64.*\) => (\S+)$', line)
        if match:
            available.setdefault(match[1], Path(match[2]))
    for path in Path('/usr/lib/x86_64-linux-gnu/pulseaudio').glob('*.so*'):
        if path.is_file():
            available[path.name] = path.resolve()
    for path in sorted((prefix / 'lib').glob('*.so*')):
        if path.is_file():
            available[path.name] = path.resolve()
    host = set(lock['host_libraries'])
    queue = [appdir / 'usr/bin/atrinik', appdir / 'usr/bin/openssl']
    queue.extend((libdir / 'ossl-modules').glob('*.so'))
    required = list(lock['required_libraries']) + list(lock['dlopen_libraries'])
    copied: dict[str, Path] = {}
    notices: set[Path] = {Path('/usr/share/doc/libasound2-data/copyright')}
    library_owners: dict[str, str] = {}

    def copy_library(name: str) -> None:
        if name in copied:
            return
        if name in host:
            return
        if '/' in name or name not in available:
            raise ValueError(f'unresolved required application library: {name}')
        source = available[name].resolve(strict=True)
        destination = libdir / name
        if destination.is_symlink():
            destination.unlink()
        shutil.copyfile(source, destination)
        destination.chmod(0o755)
        copied[name] = source
        queue.append(destination)
        if not source.is_relative_to(prefix):
            # Debian merged-/usr can record either spelling in its package db.
            candidates = [str(source)]
            if str(source).startswith('/usr/lib/'):
                candidates.append(str(source)[4:])
            owners = None
            for candidate in candidates:
                result = subprocess.run(['dpkg-query', '-S', candidate], capture_output=True,
                                        text=True, timeout=30, check=False)
                if result.returncode == 0:
                    owners = result.stdout.splitlines()
                    break
            if not owners:
                raise ValueError(f'no package copyright owner for {source}')
            for owner in owners:
                package = owner.split(': ', 1)[0].split(':', 1)[0]
                notice = Path('/usr/share/doc') / package / 'copyright'
                if not notice.is_file():
                    raise ValueError(f'missing required copyright notice: {notice}')
                notices.add(notice)
                library_owners[name] = package

    for name in required:
        copy_library(name)
    checked: set[Path] = set()
    while queue:
        path = queue.pop()
        if path in checked:
            continue
        checked.add(path)
        needed, _ = dynamic(path)
        for name in needed:
            copy_library(name)
        rpath = '$ORIGIN/../lib' if path.parent.name == 'bin' else (
            '$ORIGIN/..' if path.parent.name == 'ossl-modules' else '$ORIGIN')
        subprocess.run(['patchelf', '--set-rpath', rpath, str(path)], check=True, timeout=30)
    for notice in sorted(notices):
        destination = docdir / 'licenses/system' / notice.parent.name / 'copyright'
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(notice, destination)
    # Verify provider SONAME names are materialized even if dependencies reached
    # them through a development symlink. No host driver/glibc can be bundled.
    for path in libdir.glob('*.so*'):
        if path.name in host:
            path.unlink()
    bundled = sorted(path.name for path in libdir.glob('*.so*') if path.is_file())
    source_sonames = {
        'libSDL3.so.0': 'SDL3', 'libSDL3_image.so.0': 'SDL3_image',
        'libSDL3_ttf.so.0': 'SDL3_ttf', 'libSDL3_mixer.so.0': 'SDL3_mixer',
        'libssl.so.3': 'openssl', 'libcrypto.so.3': 'openssl',
        'libcurl.so.4': 'curl', 'libcares.so.2': 'c-ares',
    }
    sources = {item['name']: item for item in lock['sources']}
    library_records = []
    for name in bundled:
        if name in source_sonames:
            source_name = source_sonames[name]
            source = sources[source_name]
            library_version, license_id = source['version'], source['license']
        else:
            package = library_owners.get(name)
            if package is None or lock['system_library_packages'].get(name) != package:
                raise ValueError(f'unlocked system library owner: {name}')
            library_version = run('dpkg-query', '-W', '-f=${Version}', package).strip()
            if library_version != lock['system_package_versions'].get(package):
                raise ValueError(f'unlocked system package version: {package}')
            source_name = f'deb:{package}'
            license_id = lock['system_package_licenses'][package]
            if sha256(Path('/usr/share/doc') / package / 'copyright') != lock['system_package_copyright_sha256'][package]:
                raise ValueError(f'system package notice differs from lock: {package}')
        library_records.append({
            'name': name, 'path': f'usr/lib/{name}', 'version': library_version,
            'sha256': sha256(libdir / name), 'license': license_id, 'source': source_name,
        })
    sys.path.insert(0, str(ROOT / 'tools/release'))
    from appimage import system_license_notices
    packages = {item['source'][4:] for item in library_records
                if item['source'].startswith('deb:')}
    packages.add('libasound2-data')
    manifest = {
        'schema': 1, 'version': version, 'revision': revision,
        'source_date_epoch': int(os.environ['SOURCE_DATE_EPOCH']),
        'lock_sha256': sha256(lock_path),
        'build_features': {'testing': False, 'coverage': False, 'sanitizers': False},
        'bundled_libraries': bundled,
        'bundled_library_records': library_records,
        'system_license_notices': system_license_notices(appdir, lock, packages),
        'native_inputs': {name: lock[name] for name in ('sources', 'tools', 'runtime')},
        'files': {},
    }
    for path in sorted(appdir.rglob('*')):
        if path.is_file() and not path.is_symlink():
            manifest['files'][path.relative_to(appdir).as_posix()] = sha256(path)
    (metadir / 'appimage-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('appdir', type=Path)
    parser.add_argument('prefix', type=Path)
    parser.add_argument('version')
    parser.add_argument('revision')
    args = parser.parse_args()
    assemble(args.appdir.resolve(), args.prefix.resolve(), args.version, args.revision)
