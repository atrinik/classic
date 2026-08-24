#!/usr/bin/env python3
"""Verify published Sound releases and update Classic's sound lock."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import BinaryIO, Callable, Iterable
import urllib.error
import urllib.parse
import urllib.request


SOUND_REPOSITORY = "atrinik/sound"
LOCK_PATH = Path("client/dependencies.lock.json")
MAX_RELEASE_PAGES = 100
RELEASES_PER_PAGE = 100
MAX_CHECKSUM_BYTES = 1024 * 1024
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024
COPY_CHUNK_BYTES = 1024 * 1024
SEMVER_RE = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")


class UpdateError(RuntimeError):
    """A release discovery, verification, or lock mutation failure."""


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
        response_url = urllib.parse.urlparse(response.geturl())
        if response_url.scheme != "https":
            raise UpdateError("asset download changed the canonical HTTPS scheme")
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


def parse_checksums(data: bytes, asset_name: str) -> str:
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as error:
        raise UpdateError("SHA256SUMS is not ASCII") from error
    matches: list[str] = []
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._-]*)", line)
        if match is None:
            raise UpdateError("SHA256SUMS contains a malformed line")
        if match.group(2) == asset_name:
            matches.append(match.group(1))
    if len(matches) != 1:
        raise UpdateError(f"SHA256SUMS must contain exactly one entry for {asset_name}")
    return matches[0]


def download_asset(
    asset: dict[str, object], url: str, path: Path, maximum: int,
    downloader: Callable[[str, BinaryIO, int], tuple[str, int]],
) -> str:
    with path.open("xb") as output:
        digest, size = downloader(url, output, maximum)
    if size != asset["size"]:
        raise UpdateError("downloaded asset size differs from GitHub metadata")
    return digest


def release_by_tag(releases: list[dict[str, object]], tag: str) -> dict[str, object]:
    matches = [release for release in releases if release.get("tag_name") == tag]
    if len(matches) != 1:
        raise UpdateError(f"expected exactly one GitHub release for {tag}")
    release = matches[0]
    if release.get("draft") is not False or not release.get("published_at"):
        raise UpdateError(f"{tag}: release is not published and non-draft")
    return release


def sound_archive_name(tag: str) -> str:
    version = semantic_version(tag)
    return f"atrinik-sound-{version[0]}.{version[1]}.{version[2]}.tar.gz"


def checksum_for_release(
    release: dict[str, object], tag: str, directory: Path,
    downloader: Callable[[str, BinaryIO, int], tuple[str, int]],
) -> tuple[str, dict[str, object], str]:
    assets = asset_map(release)
    archive_name = sound_archive_name(tag)
    archive = assets.get(archive_name)
    checksums = assets.get("SHA256SUMS")
    if archive is None or checksums is None:
        raise UpdateError(f"{tag}: required source archive or SHA256SUMS asset is missing")
    base = f"https://github.com/{SOUND_REPOSITORY}/releases/download/{tag}"
    archive_url = asset_url(archive, f"{base}/{archive_name}")
    checksum_url = asset_url(checksums, f"{base}/SHA256SUMS")
    checksum_path = directory / f"{tag}-SHA256SUMS"
    download_asset(checksums, checksum_url, checksum_path, MAX_CHECKSUM_BYTES, downloader)
    expected = parse_checksums(checksum_path.read_bytes(), archive_name)
    return expected, archive, archive_url


def verify_candidate(
    release: dict[str, object], api: GitHubAPI, directory: Path,
    downloader: Callable[[str, BinaryIO, int], tuple[str, int]],
) -> dict[str, object]:
    tag = str(release.get("tag_name"))
    version = semantic_version(tag)
    commit = api.tag_commit(SOUND_REPOSITORY, tag)
    expected_digest, archive, archive_url = checksum_for_release(
        release, tag, directory, downloader
    )
    archive_path = directory / f"{tag}-source.tar.gz"
    actual_digest = download_asset(
        archive, archive_url, archive_path, MAX_ARCHIVE_BYTES, downloader
    )
    if actual_digest != expected_digest:
        raise UpdateError(f"{tag}: source archive digest differs from SHA256SUMS")
    return {
        "tag": tag,
        "version": list(version),
        "commit": commit,
        "url": archive_url,
        "sha256": actual_digest,
    }


def coordinate_context(dependency: dict[str, object]) -> str:
    return (
        f"sound dependency: tag={dependency.get('tag')}; "
        f"url={dependency.get('url')}; "
        f"expected_sha256={dependency.get('sha256')}"
    )


def load_lock(path: Path) -> tuple[dict[str, object], dict[str, object]]:
    serialized = path.read_bytes()
    value = load_json_bytes(serialized, str(path))
    if not isinstance(value, dict):
        raise UpdateError("dependency lock has an invalid root")
    require_keys(value, {"schema_version", "dependencies"}, "dependency lock")
    if (
        value["schema_version"] != 1
        or isinstance(value["schema_version"], bool)
        or not isinstance(value["dependencies"], list)
    ):
        raise UpdateError("dependency lock has an unsupported schema")
    if serialized != (json.dumps(value, indent=2) + "\n").encode():
        raise UpdateError("dependency lock formatting is non-canonical; refusing to rewrite it")
    matches = [
        item for item in value["dependencies"]
        if isinstance(item, dict) and item.get("name") == "sound"
    ]
    if len(matches) != 1:
        raise UpdateError("dependency lock must contain exactly one sound record")
    return value, matches[0]


def verify_current_coordinate(
    current: dict[str, object], releases: list[dict[str, object]], api: GitHubAPI,
    directory: Path, downloader: Callable[[str, BinaryIO, int], tuple[str, int]],
) -> tuple[int, int, int]:
    context = coordinate_context(current)
    try:
        tag = current.get("tag")
        commit = current.get("commit")
        digest = current.get("sha256")
        url = current.get("url")
        version = semantic_version(tag)
        if not isinstance(commit, str) or COMMIT_RE.fullmatch(commit) is None:
            raise UpdateError("lock commit is malformed")
        if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
            raise UpdateError("lock SHA-256 is malformed")
        if not isinstance(url, str):
            raise UpdateError("lock URL is malformed")
        archive_name = sound_archive_name(str(tag))
        expected_url = (
            f"https://github.com/{SOUND_REPOSITORY}/releases/download/"
            f"{tag}/{archive_name}"
        )
        if url != expected_url:
            raise UpdateError("lock URL differs from canonical coordinates")
        release = release_by_tag(releases, str(tag))
        if api.tag_commit(SOUND_REPOSITORY, str(tag)) != commit:
            raise UpdateError("tag was reused for a different commit")
        expected, _, actual_url = checksum_for_release(
            release, str(tag), directory, downloader
        )
        archive_name = sound_archive_name(str(tag))
        assets = asset_map(release)
        archive = assets[archive_name]
        archive_path = directory / f"{tag}-current.tar.gz"
        actual_digest = download_asset(
            archive, actual_url, archive_path, MAX_ARCHIVE_BYTES, downloader
        )
        if expected != digest:
            raise UpdateError("release SHA256SUMS digest differs from the lock")
        if actual_digest != expected:
            raise UpdateError("downloaded source archive digest differs from SHA256SUMS")
        if actual_url != url:
            raise UpdateError("release asset URL differs from the lock")
        return version
    except UpdateError as error:
        raise UpdateError(f"{context}; {error}") from error


def validate_lock_with_loader(root: Path, value: dict[str, object]) -> None:
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", prefix=".sound-lock-", suffix=".json",
        dir=root / "client", delete=False,
    ) as stream:
        temporary = Path(stream.name)
        stream.write(json.dumps(value, indent=2) + "\n")
    try:
        result = subprocess.run(
            [sys.executable, "server/tools/dependencies.py", "validate", "--lock", str(temporary)],
            cwd=root, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        if result.returncode:
            raise UpdateError(
                f"server dependency loader rejected updated lock: {result.stderr.strip()}"
            )
    finally:
        temporary.unlink(missing_ok=True)


def update_lock(
    root: Path, value: dict[str, object], sound: dict[str, object],
    selected: dict[str, object], *, apply: bool,
) -> dict[str, object]:
    before = {key: sound.get(key) for key in ("tag", "commit", "url", "sha256")}
    after = {key: selected[key] for key in ("tag", "commit", "url", "sha256")}
    if before == after:
        return {"changed": False, "old": before, "new": after}
    for key, new_value in after.items():
        sound[key] = new_value
    validate_lock_with_loader(root, value)
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


def preflight_current(
    root: Path, *, api: GitHubAPI | None = None,
    downloader: Callable[[str, BinaryIO, int], tuple[str, int]] = download_bounded,
) -> dict[str, object]:
    api = api or GitHubAPI()
    _, current = load_lock(root / LOCK_PATH)
    releases = api.releases(SOUND_REPOSITORY)
    with tempfile.TemporaryDirectory(prefix="atrinik-sound-preflight-") as directory_name:
        version = verify_current_coordinate(
            current, releases, api, Path(directory_name), downloader
        )
    return {
        "schema_version": 1,
        "repository": SOUND_REPOSITORY,
        "verified": True,
        "coordinate": {
            "tag": current["tag"],
            "version": list(version),
            "commit": current["commit"],
            "url": current["url"],
            "sha256": current["sha256"],
        },
    }


def execute(
    root: Path, *, apply: bool, api: GitHubAPI | None = None,
    downloader: Callable[[str, BinaryIO, int], tuple[str, int]] = download_bounded,
) -> dict[str, object]:
    api = api or GitHubAPI()
    lock, current = load_lock(root / LOCK_PATH)
    releases = api.releases(SOUND_REPOSITORY)
    with tempfile.TemporaryDirectory(prefix="atrinik-sound-update-") as directory_name:
        directory = Path(directory_name)
        current_version = verify_current_coordinate(
            current, releases, api, directory, downloader
        )
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
                candidate = verify_candidate(release, api, directory, downloader)
                if api.compare(SOUND_REPOSITORY, str(current["commit"]), str(candidate["commit"])) != "ahead":
                    raise UpdateError("candidate commit is not a strict descendant of the current lock")
                accepted.append(candidate)
            except UpdateError as error:
                rejected.append({"tag": str(tag), "reason": str(error)})
        if not accepted:
            return {
                "schema_version": 1,
                "repository": SOUND_REPOSITORY,
                "changed": False,
                "old": {key: current.get(key) for key in ("tag", "commit", "url", "sha256")},
                "new": None,
                "rejected": rejected,
            }
        selected = max(accepted, key=lambda item: tuple(item["version"]))
        mutation = update_lock(root, lock, current, selected, apply=apply)
        return {
            "schema_version": 1,
            "repository": SOUND_REPOSITORY,
            **mutation,
            "rejected": rejected,
        }


def write_pr_body(evidence: dict[str, object], path: Path) -> None:
    old = evidence.get("old")
    new = evidence.get("new")
    if evidence.get("changed") is not True or not isinstance(old, dict) or not isinstance(new, dict):
        raise UpdateError("pull-request evidence requires a verified lock change")
    lines = [
        "Updates the Classic client sound lock after complete Sound release verification.",
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
        "- Required one published, non-draft Sound release for every selected tag.",
        "- Verified the immutable tag commit, canonical source archive URL, SHA256SUMS entry, and downloaded archive digest.",
        "- Re-ran the authoritative Classic dependency-lock loader before mutation.",
        "- Classic consumers continue to use the digest-pinned offline dependency bundle.",
        "",
        "Generated by the repository-owned verified Sound updater. This pull request is never approved or merged by that automation.",
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
    parser.add_argument("--verify", action="store_true", help="verify only the current published lock coordinate")
    parser.add_argument("--evidence", type=Path, help="write machine-readable verification evidence")
    parser.add_argument("--pr-body", type=Path, help="write a complete pull-request body")
    parser.add_argument("--github-output", type=Path, help="append bounded workflow outputs")
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    if arguments.apply and arguments.verify:
        print("sound update error: --apply and --verify are mutually exclusive", file=sys.stderr)
        return 2
    try:
        root = arguments.root.resolve(strict=True)
        evidence = (
            preflight_current(root)
            if arguments.verify
            else execute(root, apply=arguments.apply)
        )
        serialized = json.dumps(evidence, indent=2, sort_keys=True) + "\n"
        if arguments.evidence:
            arguments.evidence.parent.mkdir(parents=True, exist_ok=True)
            arguments.evidence.write_text(serialized, encoding="utf-8")
        if arguments.pr_body and evidence.get("changed") is True:
            write_pr_body(evidence, arguments.pr_body)
        if arguments.github_output:
            write_github_output(evidence, arguments.github_output)
        print(serialized, end="")
    except (OSError, UpdateError, urllib.error.URLError) as error:
        print(f"sound update error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
