#!/usr/bin/env python3
"""Select a fully verified content@main Classic release and update its lock."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile
import tempfile
from typing import BinaryIO, Callable, Iterable
import urllib.parse
import urllib.request


CONTENT_REPOSITORY = "atrinik/content"
CONTENT_BRANCH = "main"
CONTENT_TARGET = "classic"
LOCK_PATH = Path("server/dependencies.lock.json")
RUNTIME_FORMAT = "atrinik-classic-runtime-content-v1"
CONTENT_FORMAT = "classic-ads-v1"
CONSUMERS = ["classic/client", "classic/editor", "classic/server"]
MAX_RELEASE_PAGES = 100
RELEASES_PER_PAGE = 100
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024
MAX_EXPANDED_BYTES = 4 * 1024 * 1024 * 1024
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_MEMBERS = 100_000
COPY_CHUNK_BYTES = 1024 * 1024
SEMVER_RE = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")


class UpdateError(RuntimeError):
    """Release discovery, verification, or lock mutation failed closed."""


def reject_duplicate_keys(pairs: Iterable[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise UpdateError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json_bytes(data: bytes, context: str) -> object:
    try:
        return json.loads(data, object_pairs_hook=reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise UpdateError(f"{context} is not canonical UTF-8 JSON: {error}") from error


def require_keys(value: dict[str, object], expected: set[str], context: str) -> None:
    actual = set(value)
    if actual != expected:
        raise UpdateError(
            f"{context} keys differ: missing={sorted(expected - actual)}, "
            f"unexpected={sorted(actual - expected)}"
        )


def semantic_version(tag: object) -> tuple[int, int, int]:
    if not isinstance(tag, str):
        raise UpdateError("release tag must be a string")
    match = SEMVER_RE.fullmatch(tag)
    if match is None:
        raise UpdateError(f"release tag is not canonical semantic version: {tag}")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def run_json(arguments: list[str], *, cwd: Path | None = None) -> object:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise UpdateError(f"command failed ({arguments[0]}): {message}")
    return load_json_bytes(result.stdout, arguments[0])


class GitHubAPI:
    """Small explicit GitHub API client using the runner's authenticated gh CLI."""

    def get(self, endpoint: str) -> object:
        return run_json(["gh", "api", "--method", "GET", endpoint])

    def releases(self, repository: str) -> list[dict[str, object]]:
        releases: list[dict[str, object]] = []
        for page in range(1, MAX_RELEASE_PAGES + 1):
            value = self.get(
                f"repos/{repository}/releases?per_page={RELEASES_PER_PAGE}&page={page}"
            )
            if not isinstance(value, list):
                raise UpdateError("GitHub releases response must be an array")
            if any(not isinstance(item, dict) for item in value):
                raise UpdateError("GitHub releases response contains a non-object")
            releases.extend(value)  # type: ignore[arg-type]
            if len(value) < RELEASES_PER_PAGE:
                return releases
        raise UpdateError("GitHub release pagination exceeded its explicit limit")

    def tag_commit(self, repository: str, tag: str) -> str:
        value = self.get(f"repos/{repository}/git/ref/tags/{tag}")
        seen: set[str] = set()
        for _ in range(8):
            if not isinstance(value, dict) or not isinstance(value.get("object"), dict):
                raise UpdateError(f"{tag}: malformed Git tag response")
            target = value["object"]
            target_type = target.get("type")
            sha = target.get("sha")
            if not isinstance(sha, str) or COMMIT_RE.fullmatch(sha) is None:
                raise UpdateError(f"{tag}: malformed Git tag target")
            if target_type == "commit":
                return sha
            if target_type != "tag" or sha in seen:
                raise UpdateError(f"{tag}: unsupported or cyclic Git tag target")
            seen.add(sha)
            value = self.get(f"repos/{repository}/git/tags/{sha}")
        raise UpdateError(f"{tag}: annotated tag chain is too deep")

    def compare(self, repository: str, base: str, head: str) -> str:
        value = self.get(f"repos/{repository}/compare/{base}...{head}")
        if not isinstance(value, dict) or value.get("status") not in {
            "ahead", "behind", "diverged", "identical"
        }:
            raise UpdateError("GitHub compare response is malformed")
        return str(value["status"])


def download_bounded(url: str, output: BinaryIO, maximum: int) -> tuple[str, int]:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https" or parsed.netloc != "github.com":
        raise UpdateError(f"asset URL is not canonical GitHub HTTPS: {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "Atrinik updater/1"})
    digest = hashlib.sha256()
    total = 0
    with urllib.request.urlopen(request, timeout=60) as response:
        if urllib.parse.urlparse(response.geturl()).scheme != "https":
            raise UpdateError("asset download changed URL scheme")
        declared = response.headers.get("Content-Length")
        if declared is not None:
            try:
                declared_size = int(declared)
            except ValueError as error:
                raise UpdateError("asset has invalid Content-Length") from error
            if declared_size < 0 or declared_size > maximum:
                raise UpdateError("asset exceeds size limit")
        while True:
            chunk = response.read(COPY_CHUNK_BYTES)
            if not chunk:
                break
            total += len(chunk)
            if total > maximum:
                raise UpdateError("asset exceeds size limit")
            digest.update(chunk)
            output.write(chunk)
    return digest.hexdigest(), total


def asset_map(release: dict[str, object]) -> dict[str, dict[str, object]]:
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise UpdateError("release assets must be an array")
    result: dict[str, dict[str, object]] = {}
    for asset in assets:
        if not isinstance(asset, dict) or not isinstance(asset.get("name"), str):
            raise UpdateError("release contains a malformed asset")
        name = str(asset["name"])
        if name in result:
            raise UpdateError(f"release has duplicate asset name: {name}")
        result[name] = asset
    return result


def asset_url(asset: dict[str, object], expected: str) -> str:
    url = asset.get("browser_download_url")
    size = asset.get("size")
    if not isinstance(url, str) or url != expected:
        raise UpdateError(f"asset URL differs from canonical release URL: {url}")
    if not isinstance(size, int) or isinstance(size, bool) or size < 1:
        raise UpdateError("asset size is invalid")
    return url


def parse_checksums(data: bytes, runtime_name: str) -> str:
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as error:
        raise UpdateError("SHA256SUMS is not ASCII") from error
    matches: list[str] = []
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._-]*)", line)
        if match is None:
            raise UpdateError("SHA256SUMS contains a malformed line")
        if match.group(2) == runtime_name:
            matches.append(match.group(1))
    if len(matches) != 1:
        raise UpdateError("SHA256SUMS must contain exactly one runtime entry")
    return matches[0]


def safe_member_path(name: str, root: str) -> str | None:
    if not name or "\0" in name or "\\" in name or ":" in name:
        raise UpdateError(f"unsafe archive member path: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise UpdateError(f"unsafe archive member path: {name!r}")
    if path.parts[0] != root:
        raise UpdateError(f"archive member is outside the canonical root: {name}")
    if len(path.parts) == 1:
        return None
    return PurePosixPath(*path.parts[1:]).as_posix()


def hash_stream(stream: BinaryIO, size: int) -> str:
    digest = hashlib.sha256()
    remaining = size
    while remaining:
        chunk = stream.read(min(COPY_CHUNK_BYTES, remaining))
        if not chunk:
            raise UpdateError("archive member is truncated")
        digest.update(chunk)
        remaining -= len(chunk)
    if stream.read(1):
        raise UpdateError("archive member exceeds its declared size")
    return digest.hexdigest()


def validate_file_entry(entry: object, context: str) -> tuple[str, str, int]:
    if not isinstance(entry, dict):
        raise UpdateError(f"{context} must be an object")
    require_keys(entry, {"path", "sha256", "size"}, context)
    path, digest, size = entry["path"], entry["sha256"], entry["size"]
    if not isinstance(path, str) or safe_member_path(f"root/{path}", "root") != path:
        raise UpdateError(f"{context}.path is not canonical")
    if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
        raise UpdateError(f"{context}.sha256 is invalid")
    if not isinstance(size, int) or isinstance(size, bool) or not 0 <= size <= MAX_FILE_BYTES:
        raise UpdateError(f"{context}.size is invalid")
    return path, digest, size


def validate_manifest(
    data: bytes,
    files: dict[str, tuple[str, int]],
    *,
    version: str,
    commit: str,
    classic_version: tuple[int, int, int],
) -> dict[str, object]:
    value = load_json_bytes(data, "runtime manifest")
    if not isinstance(value, dict):
        raise UpdateError("runtime manifest root must be an object")
    require_keys(value, {
        "schema_version", "target", "source", "release_version",
        "content_format", "artifact_format", "compatible_classic_releases",
        "consumers", "replacement_ready", "replacement_toolkit_package",
        "license_files", "files",
    }, "runtime manifest")
    source = value["source"]
    if not isinstance(source, dict):
        raise UpdateError("runtime manifest source must be an object")
    require_keys(source, {"repository", "branch", "commit"}, "runtime manifest source")
    expected = {
        "schema_version": 2,
        "target": CONTENT_TARGET,
        "release_version": version,
        "content_format": CONTENT_FORMAT,
        "artifact_format": RUNTIME_FORMAT,
        "consumers": CONSUMERS,
        "replacement_ready": False,
        "replacement_toolkit_package": False,
    }
    for key, wanted in expected.items():
        if value[key] != wanted:
            raise UpdateError(f"runtime manifest {key} does not match the Classic contract")
    if source != {"repository": CONTENT_REPOSITORY, "branch": CONTENT_BRANCH, "commit": commit}:
        raise UpdateError("runtime manifest source coordinate is incorrect")
    compatibility = value["compatible_classic_releases"]
    match = (
        re.fullmatch(
            r">=(\d+)\.(\d+)\.(\d+) <(\d+)\.(\d+)\.(\d+)",
            compatibility,
        )
        if isinstance(compatibility, str)
        else None
    )
    if match is None:
        raise UpdateError("runtime manifest compatibility range is unsupported")
    lower = tuple(int(part) for part in match.groups()[:3])
    upper = tuple(int(part) for part in match.groups()[3:])
    if not lower <= classic_version < upper:
        raise UpdateError("current Classic release does not satisfy content compatibility")

    entries = value["files"]
    if not isinstance(entries, list) or not entries:
        raise UpdateError("runtime manifest files must be non-empty")
    validated = [validate_file_entry(item, f"files[{index}]") for index, item in enumerate(entries)]
    paths = [item[0] for item in validated]
    if paths != sorted(paths) or len(paths) != len(set(path.casefold() for path in paths)):
        raise UpdateError("runtime manifest file paths are not sorted and unique")
    manifest_files = {path: (digest, size) for path, digest, size in validated}
    if manifest_files != files:
        raise UpdateError("runtime archive files do not exactly match the manifest")

    licenses = value["license_files"]
    if not isinstance(licenses, list) or not licenses:
        raise UpdateError("runtime manifest license_files must be non-empty")
    validated_licenses = [
        validate_file_entry(item, f"license_files[{index}]")
        for index, item in enumerate(licenses)
    ]
    expected_licenses = [
        item for item in validated
        if item[0].startswith("attribution/")
        and PurePosixPath(item[0]).name in {"COPYING", "LICENSE"}
    ]
    if validated_licenses != expected_licenses:
        raise UpdateError("runtime license attribution entries are not complete and digest-bound")
    return value


def verify_archive(
    path: Path, *, version: str, commit: str, classic_version: tuple[int, int, int]
) -> None:
    root = f"atrinik-content-{version}-{CONTENT_TARGET}-runtime"
    seen: set[str] = set()
    file_paths: set[str] = set()
    files: dict[str, tuple[str, int]] = {}
    manifest_data: bytes | None = None
    expanded = 0
    count = 0
    try:
        archive = tarfile.open(path, mode="r:gz")
    except (OSError, tarfile.TarError) as error:
        raise UpdateError(f"runtime archive cannot be opened: {error}") from error
    with archive:
        for member in archive:
            count += 1
            if count > MAX_MEMBERS:
                raise UpdateError("runtime archive has too many members")
            relative = safe_member_path(member.name, root)
            key = (relative or "").casefold()
            if key in seen:
                raise UpdateError(f"runtime archive has duplicate output path: {relative}")
            if relative is None and not member.isdir():
                raise UpdateError("runtime archive root must be a directory")
            prefixes = PurePosixPath(relative).parts if relative is not None else ()
            for index in range(1, len(prefixes)):
                parent = PurePosixPath(*prefixes[:index]).as_posix().casefold()
                if parent in file_paths:
                    raise UpdateError(f"runtime archive has a file/directory collision: {relative}")
            if member.isfile() and any(path.startswith(f"{key}/") for path in seen):
                raise UpdateError(f"runtime archive has a file/directory collision: {relative}")
            seen.add(key)
            if relative is None or member.isdir():
                continue
            if not member.isfile():
                raise UpdateError(f"runtime archive has unsafe member type: {member.name}")
            if member.size < 0 or member.size > MAX_FILE_BYTES:
                raise UpdateError(f"runtime archive member exceeds size limit: {member.name}")
            file_paths.add(key)
            expanded += member.size
            if expanded > MAX_EXPANDED_BYTES:
                raise UpdateError("runtime archive exceeds expanded size limit")
            stream = archive.extractfile(member)
            if stream is None:
                raise UpdateError(f"runtime archive member cannot be read: {member.name}")
            with stream:
                if relative == "manifest.json":
                    manifest_data = stream.read(MAX_FILE_BYTES + 1)
                    if len(manifest_data) != member.size or len(manifest_data) > MAX_FILE_BYTES:
                        raise UpdateError("runtime manifest size is invalid")
                else:
                    files[relative] = (hash_stream(stream, member.size), member.size)
    if manifest_data is None:
        raise UpdateError("runtime archive has no manifest at its canonical location")
    validate_manifest(
        manifest_data, files, version=version, commit=commit,
        classic_version=classic_version,
    )


def release_by_tag(releases: list[dict[str, object]], tag: str) -> dict[str, object]:
    matches = [release for release in releases if release.get("tag_name") == tag]
    if len(matches) != 1:
        raise UpdateError(f"expected exactly one GitHub release for {tag}")
    release = matches[0]
    if release.get("draft") is not False or not release.get("published_at"):
        raise UpdateError(f"{tag}: release is not published and non-draft")
    return release


def download_asset(
    asset: dict[str, object], url: str, path: Path, maximum: int,
    downloader: Callable[[str, BinaryIO, int], tuple[str, int]],
) -> str:
    with path.open("xb") as output:
        digest, size = downloader(url, output, maximum)
    if size != asset["size"]:
        raise UpdateError("downloaded asset size differs from GitHub metadata")
    return digest


def checksum_for_release(
    release: dict[str, object], tag: str, version: str, directory: Path,
    downloader: Callable[[str, BinaryIO, int], tuple[str, int]],
    *, classic_target: bool,
) -> tuple[str, dict[str, object], str]:
    assets = asset_map(release)
    suffix = f"-{CONTENT_TARGET}" if classic_target else ""
    runtime_name = f"atrinik-content-{version}{suffix}-runtime.tar.gz"
    runtime = assets.get(runtime_name)
    checksums = assets.get("SHA256SUMS")
    if runtime is None or checksums is None:
        raise UpdateError(f"{tag}: required runtime assets are missing")
    base = f"https://github.com/{CONTENT_REPOSITORY}/releases/download/{tag}"
    runtime_url = asset_url(runtime, f"{base}/{runtime_name}")
    checksum_url = asset_url(checksums, f"{base}/SHA256SUMS")
    checksum_path = directory / f"{tag}-SHA256SUMS"
    download_asset(checksums, checksum_url, checksum_path, 1024 * 1024, downloader)
    expected = parse_checksums(checksum_path.read_bytes(), runtime_name)
    return expected, runtime, runtime_url


def verify_candidate(
    release: dict[str, object], api: GitHubAPI, directory: Path,
    classic_version: tuple[int, int, int],
    downloader: Callable[[str, BinaryIO, int], tuple[str, int]],
) -> dict[str, object]:
    tag = str(release.get("tag_name"))
    semver = semantic_version(tag)
    version = tag[1:]
    commit = api.tag_commit(CONTENT_REPOSITORY, tag)
    expected_digest, runtime, runtime_url = checksum_for_release(
        release, tag, version, directory, downloader, classic_target=True
    )
    runtime_path = directory / f"{tag}-runtime.tar.gz"
    actual_digest = download_asset(runtime, runtime_url, runtime_path, MAX_ARCHIVE_BYTES, downloader)
    if actual_digest != expected_digest:
        raise UpdateError(f"{tag}: runtime digest differs from SHA256SUMS")
    verify_archive(runtime_path, version=version, commit=commit, classic_version=classic_version)
    return {
        "tag": tag,
        "version": list(semver),
        "commit": commit,
        "url": runtime_url,
        "sha256": actual_digest,
    }


def load_lock(path: Path) -> tuple[dict[str, object], dict[str, object]]:
    serialized = path.read_bytes()
    value = load_json_bytes(serialized, str(path))
    if not isinstance(value, dict) or not isinstance(value.get("dependencies"), list):
        raise UpdateError("dependency lock has an invalid root")
    if serialized != (json.dumps(value, indent=2) + "\n").encode():
        raise UpdateError("dependency lock formatting is non-canonical; refusing to rewrite it")
    matches = [
        item for item in value["dependencies"]
        if isinstance(item, dict) and item.get("name") == "content"
    ]
    if len(matches) != 1:
        raise UpdateError("dependency lock must contain exactly one content record")
    return value, matches[0]


def current_classic_version(root: Path) -> tuple[int, int, int]:
    result = subprocess.run(
        ["git", "describe", "--tags", "--abbrev=0", "--match", "v[0-9]*"],
        cwd=root, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise UpdateError(f"cannot determine current Classic release: {result.stderr.strip()}")
    return semantic_version(result.stdout.strip())


def verify_current_coordinate(
    current: dict[str, object], releases: list[dict[str, object]], api: GitHubAPI,
    directory: Path, downloader: Callable[[str, BinaryIO, int], tuple[str, int]],
) -> tuple[tuple[int, int, int], bool]:
    tag = current.get("tag")
    commit = current.get("commit")
    digest = current.get("sha256")
    url = current.get("url")
    semver = semantic_version(tag)
    if not isinstance(commit, str) or COMMIT_RE.fullmatch(commit) is None:
        raise UpdateError("current content lock coordinate is malformed")
    if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
        raise UpdateError("current content lock coordinate is malformed")
    if not isinstance(url, str):
        raise UpdateError("current content lock coordinate is malformed")
    version = str(tag)[1:]
    base = f"https://github.com/{CONTENT_REPOSITORY}/releases/download/{tag}"
    legacy_url = f"{base}/atrinik-content-{version}-runtime.tar.gz"
    classic_url = (
        f"{base}/atrinik-content-{version}-{CONTENT_TARGET}-runtime.tar.gz"
    )
    if url == legacy_url:
        legacy = True
    elif url == classic_url:
        legacy = False
    else:
        raise UpdateError(
            "current content release URL differs from canonical coordinates"
        )
    release = release_by_tag(releases, str(tag))
    if api.tag_commit(CONTENT_REPOSITORY, str(tag)) != commit:
        raise UpdateError("current content tag was reused for a different commit")
    expected, _, expected_url = checksum_for_release(
        release, str(tag), version, directory, downloader,
        classic_target=not legacy,
    )
    if expected != digest:
        raise UpdateError("current content release digest differs from the lock")
    if url != expected_url:
        raise UpdateError("current content release URL differs from the lock")
    return semver, legacy


def validate_lock_with_server(root: Path, value: dict[str, object]) -> None:
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", prefix=".content-lock-", suffix=".json",
        dir=root / "server", delete=False,
    ) as stream:
        temporary = Path(stream.name)
        stream.write(json.dumps(value, indent=2) + "\n")
    try:
        result = subprocess.run(
            [sys.executable, "server/tools/dependencies.py", "validate", "--lock", str(temporary)],
            cwd=root, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        if result.returncode:
            raise UpdateError(f"server dependency loader rejected updated lock: {result.stderr.strip()}")
    finally:
        temporary.unlink(missing_ok=True)


def update_lock(
    root: Path, value: dict[str, object], content: dict[str, object],
    selected: dict[str, object], *, apply: bool,
) -> dict[str, object]:
    before = {key: content.get(key) for key in ("tag", "commit", "url", "sha256")}
    after = {key: selected[key] for key in ("tag", "commit", "url", "sha256")}
    if before == after:
        return {"changed": False, "old": before, "new": after}
    for key, new_value in after.items():
        content[key] = new_value
    validate_lock_with_server(root, value)
    if apply:
        lock_path = root / LOCK_PATH
        mode = lock_path.stat().st_mode
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", prefix=".dependencies-lock-", suffix=".json",
            dir=lock_path.parent, delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(json.dumps(value, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(mode)
        temporary.replace(lock_path)
    return {"changed": True, "old": before, "new": after}


def execute(
    root: Path, *, apply: bool, api: GitHubAPI | None = None,
    downloader: Callable[[str, BinaryIO, int], tuple[str, int]] = download_bounded,
) -> dict[str, object]:
    api = api or GitHubAPI()
    lock, current = load_lock(root / LOCK_PATH)
    releases = api.releases(CONTENT_REPOSITORY)
    published_tags: set[str] = set()
    for release in releases:
        if release.get("draft") is not False or not release.get("published_at"):
            continue
        tag = release.get("tag_name")
        if not isinstance(tag, str):
            raise UpdateError("published release has no string tag")
        if tag in published_tags:
            raise UpdateError(f"published release tag is ambiguous: {tag}")
        published_tags.add(tag)
    classic_version = current_classic_version(root)
    with tempfile.TemporaryDirectory(prefix="atrinik-content-update-") as directory_name:
        directory = Path(directory_name)
        current_version, current_is_legacy = verify_current_coordinate(
            current, releases, api, directory, downloader
        )
        accepted: list[dict[str, object]] = []
        rejected: list[dict[str, str]] = []
        for release in releases:
            tag = release.get("tag_name")
            if release.get("draft") is not False or not release.get("published_at"):
                continue
            try:
                version = semantic_version(tag)
            except UpdateError:
                continue
            if version <= current_version:
                continue
            try:
                candidate = verify_candidate(
                    release, api, directory, classic_version, downloader
                )
                if (
                    not current_is_legacy
                    and api.compare(
                        CONTENT_REPOSITORY,
                        str(current["commit"]),
                        str(candidate["commit"]),
                    ) != "ahead"
                ):
                    raise UpdateError(
                        "candidate commit is not a strict descendant of the current lock"
                    )
                accepted.append(candidate)
            except UpdateError as error:
                rejected.append({"tag": str(tag), "reason": str(error)})
        if not accepted:
            evidence = {
                "schema_version": 1,
                "repository": CONTENT_REPOSITORY,
                "changed": False,
                "old": {key: current.get(key) for key in ("tag", "commit", "url", "sha256")},
                "new": None,
                "rejected": rejected,
            }
            return evidence
        selected = max(accepted, key=lambda item: tuple(item["version"]))
        mutation = update_lock(root, lock, current, selected, apply=apply)
        return {
            "schema_version": 1,
            "repository": CONTENT_REPOSITORY,
            **mutation,
            "rejected": rejected,
        }


def write_pr_body(evidence: dict[str, object], path: Path) -> None:
    old = evidence.get("old")
    new = evidence.get("new")
    if evidence.get("changed") is not True or not isinstance(old, dict) or not isinstance(new, dict):
        raise UpdateError("pull-request evidence requires a verified lock change")
    lines = [
        "Updates the Classic server content lock after complete release verification.",
        "",
        "## Verified coordinate",
        "",
        "| Field | Old | New |",
        "| --- | --- | --- |",
    ]
    for field in ("tag", "commit", "url", "sha256"):
        lines.append(f"| `{field}` | `{old.get(field)}` | `{new.get(field)}` |")
    lines.extend([
        "",
        "## Verification",
        "",
        "- Enumerated all published content releases with bounded pagination.",
        "- Verified the tag, runtime asset, SHA256SUMS, archive safety, manifest, compatibility, file digests, and license attributions.",
        "- Required the Classic target to originate from `content@main`; subsequent updates must be strict descendants of the current main coordinate.",
        "- Re-ran the existing server dependency-lock loader before mutation.",
        "",
        "Generated by the repository-owned verified content updater. This pull request is never approved or merged by that automation.",
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_github_output(evidence: dict[str, object], path: Path) -> None:
    changed = evidence.get("changed") is True
    new = evidence.get("new")
    values = {"changed": str(changed).lower()}
    if changed:
        if not isinstance(new, dict):
            raise UpdateError("changed evidence has no new coordinate")
        for key in ("tag", "commit", "sha256"):
            value = new.get(key)
            if not isinstance(value, str) or "\n" in value or "\r" in value:
                raise UpdateError(f"new evidence {key} is invalid")
            values[key] = value
    with path.open("a", encoding="utf-8") as stream:
        for key, value in values.items():
            stream.write(f"{key}={value}\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--apply", action="store_true", help="atomically update the verified lock")
    parser.add_argument("--evidence", type=Path, help="write machine-readable old/new evidence")
    parser.add_argument("--pr-body", type=Path, help="write a complete pull-request body")
    parser.add_argument("--github-output", type=Path, help="append bounded workflow outputs")
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    try:
        evidence = execute(arguments.root.resolve(strict=True), apply=arguments.apply)
        serialized = json.dumps(evidence, indent=2, sort_keys=True) + "\n"
        if arguments.evidence:
            arguments.evidence.parent.mkdir(parents=True, exist_ok=True)
            arguments.evidence.write_text(serialized, encoding="utf-8")
        if arguments.pr_body and evidence.get("changed") is True:
            write_pr_body(evidence, arguments.pr_body)
        if arguments.github_output:
            write_github_output(evidence, arguments.github_output)
        print(serialized, end="")
    except (OSError, UpdateError, urllib.error.URLError, tarfile.TarError) as error:
        print(f"content update error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
