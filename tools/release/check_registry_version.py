#!/usr/bin/env python3
"""Fail closed when a versioned GHCR tag already exists or cannot be audited."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess


TAG_RE = re.compile(
    r"(?:[0-9]+\.[0-9]+\.[0-9]+|latest|materials-[0-9a-f]{64})"
)
DIGEST_RE = re.compile(r"sha256:[0-9a-f]{64}")


def request(endpoint: str) -> tuple[int, object, str]:
    result = subprocess.run(
        ["gh", "api", endpoint],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        detail = result.stderr.strip() or result.stdout.strip() or "empty response"
        raise RuntimeError(f"cannot audit GHCR package: {detail}") from error
    return result.returncode, value, result.stderr.strip()


def find_version(value: object, tag: str) -> str | None:
    if not isinstance(value, list):
        raise RuntimeError("GHCR package API returned a non-list response")
    matches = []
    for item in value:
        if not isinstance(item, dict):
            raise RuntimeError("GHCR package API returned invalid version metadata")
        metadata = item.get("metadata")
        if not isinstance(metadata, dict):
            raise RuntimeError("GHCR package API returned invalid version metadata")
        container = metadata.get("container")
        if not isinstance(container, dict):
            raise RuntimeError("GHCR package API returned invalid container metadata")
        tags = container.get("tags")
        if not isinstance(tags, list) or not all(
            isinstance(item, str) for item in tags
        ):
            raise RuntimeError("GHCR package API returned invalid tag metadata")
        if tag in tags:
            digest = item.get("name")
            if not isinstance(digest, str) or DIGEST_RE.fullmatch(digest) is None:
                raise RuntimeError("GHCR package version has no immutable digest")
            matches.append(digest)
    if len(matches) > 1:
        raise RuntimeError(f"multiple GHCR versions use tag {tag}")
    return matches[0] if matches else None


def write_output(
    path: Path, package_exists: bool, exists: bool, digest: str
) -> None:
    with path.open("a", encoding="utf-8") as stream:
        stream.write(f"package_exists={str(package_exists).lower()}\n")
        stream.write(f"exists={str(exists).lower()}\n")
        stream.write(f"digest={digest}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--organization", required=True)
    parser.add_argument("--package", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--github-output", type=Path)
    arguments = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", arguments.organization):
        parser.error("invalid organization")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", arguments.package):
        parser.error("invalid package")
    if not TAG_RE.fullmatch(arguments.tag):
        parser.error(
            "--tag must be MAJOR.MINOR.PATCH, latest, or materials-SHA256"
        )

    page = 1
    while True:
        endpoint = (
            f"orgs/{arguments.organization}/packages/container/{arguments.package}/versions"
            f"?per_page=100&page={page}"
        )
        returncode, value, detail = request(endpoint)
        if returncode:
            if (
                page == 1
                and isinstance(value, dict)
                and str(value.get("status")) == "404"
                and value.get("message") == "Package not found."
            ):
                if arguments.github_output is not None:
                    write_output(arguments.github_output, False, False, "")
                print("GHCR package does not exist yet")
                return 0
            raise RuntimeError(f"cannot audit GHCR package: {detail or value}")
        digest = find_version(value, arguments.tag)
        if digest is not None:
            if arguments.github_output is not None:
                write_output(arguments.github_output, True, True, digest)
            print(
                f"GHCR tag already exists at {digest}: "
                f"{arguments.package}:{arguments.tag}"
            )
            return 0
        if len(value) < 100:
            break
        page += 1
    if arguments.github_output is not None:
        write_output(arguments.github_output, True, False, "")
    print(f"GHCR tag is available: {arguments.package}:{arguments.tag}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
