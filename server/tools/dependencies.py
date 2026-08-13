#!/usr/bin/env python3
"""Fetch and verify release artifacts pinned by dependencies.lock.json."""

from __future__ import annotations

import argparse
import datetime
import email.utils
import hashlib
import http.client
import json
import os
from pathlib import Path, PurePosixPath
import random
import shutil
import ssl
import stat
import tarfile
import tempfile
import time
from typing import BinaryIO, Iterable
import urllib.parse
import urllib.request
import urllib.error


LOCK_SCHEMA_VERSION = 1
MARKER_NAME = ".atrinik-dependency.json"
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024
MAX_EXPANDED_BYTES = 4 * 1024 * 1024 * 1024
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_MEMBERS = 100_000
COPY_CHUNK_BYTES = 1024 * 1024
DOWNLOAD_ATTEMPTS = 4
RETRY_BASE_SECONDS = 0.5
RETRY_MAX_SECONDS = 30.0


class DependencyError(RuntimeError):
    """A dependency lock or artifact failed validation."""


def _reject_duplicate_keys(pairs: Iterable[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise DependencyError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _require_keys(value: dict[str, object], expected: set[str], context: str) -> None:
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        details = []
        if missing:
            details.append(f"missing {', '.join(missing)}")
        if extra:
            details.append(f"unexpected {', '.join(extra)}")
        raise DependencyError(f"{context}: {'; '.join(details)}")


def _validate_text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value or value != value.strip():
        raise DependencyError(f"{field} must be a non-empty trimmed string")
    return value


def load_lock(path: Path, *, allow_file_urls: bool = False) -> list[dict[str, object]]:
    try:
        with path.open(encoding="utf-8") as stream:
            root = json.load(stream, object_pairs_hook=_reject_duplicate_keys)
    except (OSError, json.JSONDecodeError) as error:
        raise DependencyError(f"cannot read {path}: {error}") from error

    if not isinstance(root, dict):
        raise DependencyError("lock root must be an object")
    _require_keys(root, {"schema_version", "dependencies"}, "lock root")
    if root["schema_version"] != LOCK_SCHEMA_VERSION:
        raise DependencyError(
            f"unsupported lock schema version: {root['schema_version']}"
        )
    dependencies = root["dependencies"]
    if not isinstance(dependencies, list) or not dependencies:
        raise DependencyError("dependencies must be a non-empty array")

    expected = {
        "name",
        "repository",
        "tag",
        "commit",
        "url",
        "sha256",
        "destination",
        "strip_components",
    }
    names: set[str] = set()
    destinations: set[str] = set()
    validated: list[dict[str, object]] = []
    for index, item in enumerate(dependencies):
        context = f"dependency {index}"
        if not isinstance(item, dict):
            raise DependencyError(f"{context} must be an object")
        _require_keys(item, expected, context)
        values = {key: _validate_text(item[key], f"{context}.{key}") for key in expected - {"strip_components"}}

        name = values["name"]
        if not all(char.islower() or char.isdigit() or char == "-" for char in name):
            raise DependencyError(f"{context}.name must use lowercase kebab-case")
        if name in names:
            raise DependencyError(f"duplicate dependency name: {name}")
        names.add(name)

        repository = values["repository"]
        if repository.count("/") != 1 or repository.startswith("/"):
            raise DependencyError(f"{context}.repository must have owner/name form")
        tag = values["tag"]
        if not tag.startswith("v"):
            raise DependencyError(f"{context}.tag must be an immutable v-prefixed tag")
        commit = values["commit"]
        if len(commit) != 40 or any(char not in "0123456789abcdef" for char in commit):
            raise DependencyError(f"{context}.commit must be a full lowercase Git SHA")
        digest = values["sha256"]
        if len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest):
            raise DependencyError(f"{context}.sha256 must be a lowercase SHA-256")

        parsed_url = urllib.parse.urlparse(values["url"])
        allowed_schemes = {"https"}
        if allow_file_urls:
            allowed_schemes.add("file")
        if parsed_url.scheme not in allowed_schemes or not parsed_url.path.endswith(".tar.gz"):
            raise DependencyError(f"{context}.url must identify an allowed .tar.gz asset")

        destination = PurePosixPath(values["destination"])
        if (
            destination.is_absolute()
            or len(destination.parts) < 1
            or any(part in {"", ".", ".."} for part in destination.parts)
            or destination.parts[0].startswith(".")
        ):
            raise DependencyError(f"{context}.destination must be a safe relative path")
        normalized_destination = destination.as_posix()
        if normalized_destination in destinations:
            raise DependencyError(f"duplicate dependency destination: {normalized_destination}")
        destinations.add(normalized_destination)

        strip_components = item["strip_components"]
        if not isinstance(strip_components, int) or not 1 <= strip_components <= 8:
            raise DependencyError(f"{context}.strip_components must be between 1 and 8")

        validated.append({**values, "strip_components": strip_components})
    return validated


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(COPY_CHUNK_BYTES), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_url(url: str) -> str:
    parsed = urllib.parse.urlsplit(url)
    return urllib.parse.urlunsplit((parsed.scheme, parsed.hostname or "", parsed.path, "", ""))


def _retry_after(headers: object) -> float | None:
    value = getattr(headers, "get", lambda _name: None)("Retry-After")
    if value is None:
        return None
    try:
        return max(0.0, min(float(value), RETRY_MAX_SECONDS))
    except ValueError:
        try:
            when = email.utils.parsedate_to_datetime(value)
        except (TypeError, ValueError):
            return None
        if when.tzinfo is None:
            when = when.replace(tzinfo=datetime.UTC)
        return max(0.0, min((when - datetime.datetime.now(datetime.UTC)).total_seconds(), RETRY_MAX_SECONDS))


def _retryable(error: BaseException) -> tuple[bool, str, float | None]:
    if isinstance(error, urllib.error.HTTPError):
        return error.code in {408, 429} or 500 <= error.code < 600, f"HTTP {error.code}", _retry_after(error.headers)
    if isinstance(error, (ssl.SSLError, urllib.error.ContentTooShortError)):
        return False, type(error).__name__, None
    if isinstance(error, urllib.error.URLError):
        reason = error.reason
        if isinstance(reason, ssl.SSLError):
            return False, "TLS verification", None
        return True, type(reason).__name__, None
    if isinstance(error, (
        ConnectionError, TimeoutError, EOFError, OSError,
        http.client.IncompleteRead, http.client.RemoteDisconnected,
    )):
        return True, type(error).__name__, None
    return False, type(error).__name__, None


def _download(
    dependency: dict[str, object],
    cache_dir: Path,
    *,
    opener: object = urllib.request.urlopen,
    sleeper: object = time.sleep,
    jitter: object = random.uniform,
) -> Path:
    cache_dir.mkdir(parents=True, exist_ok=True)
    expected = str(dependency["sha256"])
    archive = cache_dir / f"{dependency['name']}-{expected}.tar.gz"
    if archive.exists():
        if archive.is_file() and archive.stat().st_size <= MAX_ARCHIVE_BYTES and sha256_file(archive) == expected:
            print(
                f"dependency fetch: {dependency['name']}: cache hit {_safe_url(str(dependency['url']))}",
                file=os.sys.stderr,
            )
            return archive
        archive.unlink()

    request = urllib.request.Request(
        str(dependency["url"]),
        headers={"User-Agent": "Atrinik dependency fetcher/1"},
    )
    for attempt in range(1, DOWNLOAD_ATTEMPTS + 1):
        descriptor, temporary_name = tempfile.mkstemp(prefix=f".{archive.name}.part-", dir=cache_dir)
        temporary = Path(temporary_name)
        try:
            with os.fdopen(descriptor, "wb") as output, opener(request, timeout=60) as response:
                requested_scheme = urllib.parse.urlparse(str(dependency["url"])).scheme
                response_scheme = urllib.parse.urlparse(response.geturl()).scheme
                if requested_scheme not in {"https", "file"} or response_scheme != requested_scheme:
                    raise DependencyError("download changed URL scheme")
                content_length = response.headers.get("Content-Length")
                if content_length is not None:
                    declared_size = int(content_length)
                    if declared_size < 0 or declared_size > MAX_ARCHIVE_BYTES:
                        raise DependencyError("archive exceeds size limit")
                total = 0
                while chunk := response.read(COPY_CHUNK_BYTES):
                    total += len(chunk)
                    if total > MAX_ARCHIVE_BYTES:
                        raise DependencyError("archive exceeds size limit")
                    output.write(chunk)
            if sha256_file(temporary) != expected:
                raise DependencyError("downloaded SHA-256 does not match lock")
            temporary.replace(archive)
            print(
                f"dependency fetch: {dependency['name']}: cache miss {_safe_url(str(dependency['url']))}; "
                f"attempt {attempt}/{DOWNLOAD_ATTEMPTS}; verified",
                file=os.sys.stderr,
            )
            return archive
        except Exception as error:
            temporary.unlink(missing_ok=True)
            retryable, category, retry_after = _retryable(error)
            prefix = f"{dependency['name']}: cache miss {_safe_url(str(dependency['url']))}; attempt {attempt}/{DOWNLOAD_ATTEMPTS}; {category}"
            if not retryable:
                raise DependencyError(f"{prefix}; terminal policy or integrity failure: {error}") from error
            if attempt == DOWNLOAD_ATTEMPTS:
                raise DependencyError(f"{prefix}; retry limit exhausted: {error}") from error
            delay = retry_after if retry_after is not None else min(
                RETRY_MAX_SECONDS, RETRY_BASE_SECONDS * (2 ** (attempt - 1))
            ) * float(jitter(0.5, 1.5))
            print(f"dependency fetch: {prefix}; retrying in {delay:.2f}s", file=os.sys.stderr)
            sleeper(delay)
    raise AssertionError("retry loop must return or raise")


def _stripped_path(name: str, count: int) -> PurePosixPath | None:
    if not name or "\0" in name or "\\" in name or ":" in name:
        raise DependencyError(f"unsafe archive member path: {name}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise DependencyError(f"unsafe archive member path: {name}")
    if len(path.parts) <= count:
        return None
    return PurePosixPath(*path.parts[count:])


def _copy_member(source: BinaryIO, destination: Path, expected_size: int) -> None:
    written = 0
    with destination.open("xb") as output:
        while written < expected_size:
            chunk = source.read(min(COPY_CHUNK_BYTES, expected_size - written))
            if not chunk:
                raise DependencyError(f"truncated archive member: {destination.name}")
            output.write(chunk)
            written += len(chunk)
        if source.read(1):
            raise DependencyError(f"archive member exceeds declared size: {destination.name}")


def extract_archive(archive_path: Path, destination: Path, strip_components: int) -> None:
    seen: set[str] = set()
    expanded_bytes = 0
    member_count = 0
    try:
        archive = tarfile.open(archive_path, mode="r:gz")
    except (OSError, tarfile.TarError) as error:
        raise DependencyError(f"cannot open {archive_path}: {error}") from error

    with archive:
        for member in archive:
            member_count += 1
            if member_count > MAX_MEMBERS:
                raise DependencyError("archive has too many members")
            relative = _stripped_path(member.name, strip_components)
            if relative is None:
                continue
            key = relative.as_posix().casefold()
            if key in seen:
                raise DependencyError(f"duplicate archive output path: {relative}")
            seen.add(key)
            output_path = destination.joinpath(*relative.parts)
            if member.isdir():
                output_path.mkdir(parents=True, exist_ok=True)
                continue
            if not member.isfile():
                raise DependencyError(f"unsupported archive member type: {member.name}")
            if member.size < 0 or member.size > MAX_FILE_BYTES:
                raise DependencyError(f"archive member exceeds size limit: {member.name}")
            expanded_bytes += member.size
            if expanded_bytes > MAX_EXPANDED_BYTES:
                raise DependencyError("archive exceeds expanded size limit")
            output_path.parent.mkdir(parents=True, exist_ok=True)
            source = archive.extractfile(member)
            if source is None:
                raise DependencyError(f"cannot read archive member: {member.name}")
            with source:
                _copy_member(source, output_path, member.size)
            mode = stat.S_IMODE(member.mode) & 0o755
            output_path.chmod(mode if mode else 0o644)
    if not seen:
        raise DependencyError("archive contains no files after stripping its prefix")


def source_tree_sha256(source: Path) -> str:
    manifest = ""
    for path in sorted(path for path in source.rglob("*") if path.is_file()):
        relative = path.relative_to(source).as_posix()
        if relative in {".atrinik-source-sha256", ".atrinik-mingw-patch-sha256"}:
            continue
        manifest += f"{sha256_file(path)}  {relative}\n"
    return hashlib.sha256(manifest.encode()).hexdigest()


def load_source_lock(path: Path, name: str) -> dict[str, str]:
    try:
        with path.open(encoding="utf-8") as stream:
            root = json.load(stream, object_pairs_hook=_reject_duplicate_keys)
    except (OSError, json.JSONDecodeError) as error:
        raise DependencyError(f"cannot read {path}: {error}") from error
    if not isinstance(root, dict):
        raise DependencyError("immutable source lock root must be an object")
    _require_keys(root, {"schema_version", "sources"}, "immutable source lock root")
    if root["schema_version"] != LOCK_SCHEMA_VERSION or not isinstance(root["sources"], dict):
        raise DependencyError("immutable source lock has an unsupported schema")
    source = root["sources"].get(name)
    if not isinstance(source, dict):
        raise DependencyError(f"immutable source {name} is missing")
    expected = {"url", "sha256", "tree_sha256", "mingw_tree_sha256"}
    _require_keys(source, expected, f"immutable source {name}")
    values = {key: _validate_text(value, f"immutable source {name}.{key}") for key, value in source.items()}
    parsed = urllib.parse.urlsplit(values["url"])
    if (parsed.scheme not in {"https", "file"} or parsed.query or parsed.fragment or
            not parsed.path.endswith(".tar.gz")):
        raise DependencyError(f"immutable source {name}.url must identify a canonical archive")
    for key in expected - {"url"}:
        if len(values[key]) != 64 or any(character not in "0123456789abcdef" for character in values[key]):
            raise DependencyError(f"immutable source {name}.{key} must be a lowercase SHA-256")
    return {"name": name, **values}


def fetch_source(
    *, name: str, url: str, sha256: str, tree_sha256: str, cache_dir: Path
) -> Path:
    if not name or not all(character.islower() or character.isdigit() or character == "-" for character in name):
        raise DependencyError("source name must use lowercase kebab-case")
    if len(sha256) != 64 or len(tree_sha256) != 64:
        raise DependencyError(f"{name}: source digests must be SHA-256")
    source_root = cache_dir / "sources-v1" / f"{name}-{sha256}"
    marker = source_root / ".atrinik-source-sha256"
    expected_marker = f"{sha256}:{tree_sha256}\n"
    if source_root.exists():
        if not marker.is_file() or not (source_root / "CMakeLists.txt").is_file():
            raise DependencyError(f"{name}: incomplete shared source cache")
        if marker.read_text(encoding="utf-8") != expected_marker:
            raise DependencyError(f"{name}: mismatched shared source cache")
        if source_tree_sha256(source_root) != tree_sha256:
            raise DependencyError(f"{name}: mismatched shared source content")
        return source_root
    archive = _download({"name": name, "url": url, "sha256": sha256}, cache_dir / "downloads")
    source_root.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{name}-staging-", dir=source_root.parent))
    try:
        extract_archive(archive, staging, 1)
        if not (staging / "CMakeLists.txt").is_file():
            raise DependencyError(f"{name}: verified archive has no source root")
        if source_tree_sha256(staging) != tree_sha256:
            raise DependencyError(f"{name}: verified archive has unexpected source content")
        (staging / ".atrinik-source-sha256").write_text(expected_marker, encoding="utf-8")
        staging.replace(source_root)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return source_root


def marker_for(dependency: dict[str, object]) -> dict[str, object]:
    return {
        "schema_version": LOCK_SCHEMA_VERSION,
        "name": dependency["name"],
        "repository": dependency["repository"],
        "tag": dependency["tag"],
        "commit": dependency["commit"],
        "sha256": dependency["sha256"],
    }


def read_marker(destination: Path) -> dict[str, object] | None:
    marker = destination / MARKER_NAME
    try:
        with marker.open(encoding="utf-8") as stream:
            value = json.load(stream, object_pairs_hook=_reject_duplicate_keys)
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError, DependencyError) as error:
        raise DependencyError(f"invalid managed dependency marker at {marker}: {error}") from error
    return value if isinstance(value, dict) else None


def install_dependency(
    root: Path,
    cache_dir: Path,
    dependency: dict[str, object],
    *,
    refresh: bool = False,
) -> str:
    root = root.resolve(strict=True)
    destination = (root / str(dependency["destination"])).resolve(strict=False)
    try:
        destination.relative_to(root)
    except ValueError as error:
        raise DependencyError(f"destination escapes repository: {destination}") from error
    expected_marker = marker_for(dependency)
    existing_marker = read_marker(destination) if destination.exists() else None
    if destination.exists() and existing_marker == expected_marker and not refresh:
        return "current"
    if destination.exists() and existing_marker is None:
        raise DependencyError(f"refusing to replace unmanaged destination: {destination}")

    archive = _download(dependency, cache_dir)
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{dependency['name']}-staging-", dir=destination.parent))
    backup: Path | None = None
    try:
        extract_archive(archive, staging, int(dependency["strip_components"]))
        marker = staging / MARKER_NAME
        marker.write_text(json.dumps(expected_marker, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if destination.exists():
            backup = Path(tempfile.mkdtemp(prefix=f".{dependency['name']}-backup-", dir=destination.parent))
            backup.rmdir()
            destination.replace(backup)
        staging.replace(destination)
        if backup is not None:
            shutil.rmtree(backup)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        if backup is not None and backup.exists() and not destination.exists():
            backup.replace(destination)
        raise
    return "installed"


def verify_dependency(root: Path, dependency: dict[str, object]) -> None:
    destination = root / str(dependency["destination"])
    if not destination.is_dir():
        raise DependencyError(f"missing dependency {dependency['name']} at {destination}")
    if read_marker(destination) != marker_for(dependency):
        raise DependencyError(f"dependency {dependency['name']} does not match the lock")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("validate", "sync", "verify", "list", "source"))
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--lock", type=Path)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--refresh", action="store_true", help="reinstall even when the marker is current")
    parser.add_argument("--name")
    parser.add_argument("--url")
    parser.add_argument("--sha256")
    parser.add_argument("--tree-sha256")
    parser.add_argument("--source-lock", type=Path)
    parser.add_argument("--source-name")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve(strict=True)
    lock_path = args.lock or root / "dependencies.lock.json"
    cache_dir = args.cache or root / "build" / "dependencies" / "downloads"
    try:
        if args.command == "source":
            if not all((args.source_lock, args.source_name, args.cache)):
                raise DependencyError("source requires --source-lock, --source-name, and --cache")
            source = load_source_lock(args.source_lock, args.source_name)
            print(fetch_source(
                name=source["name"], url=source["url"], sha256=source["sha256"],
                tree_sha256=source["tree_sha256"], cache_dir=args.cache,
            ))
            return 0
        dependencies = load_lock(lock_path)
        if args.command == "list":
            for dependency in dependencies:
                print(f"{dependency['name']}\t{dependency['tag']}\t{dependency['destination']}")
        elif args.command == "sync":
            for dependency in dependencies:
                status = install_dependency(root, cache_dir, dependency, refresh=args.refresh)
                print(f"{dependency['name']}: {status}")
        elif args.command == "verify":
            for dependency in dependencies:
                verify_dependency(root, dependency)
                print(f"{dependency['name']}: verified")
        else:
            print(f"{lock_path}: valid ({len(dependencies)} dependencies)")
    except (DependencyError, OSError, urllib.error.URLError) as error:
        print(f"dependency error: {error}", file=os.sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
