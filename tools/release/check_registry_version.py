#!/usr/bin/env python3
"""Fail closed when a versioned GHCR tag already exists or cannot be audited."""

from __future__ import annotations

import argparse
import json
import re
import subprocess


TAG_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--organization", required=True)
    parser.add_argument("--package", required=True)
    parser.add_argument("--tag", required=True)
    arguments = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", arguments.organization):
        parser.error("invalid organization")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", arguments.package):
        parser.error("invalid package")
    if not TAG_RE.fullmatch(arguments.tag):
        parser.error("--tag must be MAJOR.MINOR.PATCH")

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
                print("GHCR package does not exist yet")
                return 0
            raise RuntimeError(f"cannot audit GHCR package: {detail or value}")
        if not isinstance(value, list):
            raise RuntimeError("GHCR package API returned a non-list response")
        for item in value:
            if not isinstance(item, dict):
                raise RuntimeError("GHCR package API returned invalid version metadata")
            metadata = item.get("metadata", {})
            container = metadata.get("container", {}) if isinstance(metadata, dict) else {}
            tags = container.get("tags", []) if isinstance(container, dict) else []
            if not isinstance(tags, list):
                raise RuntimeError("GHCR package API returned invalid tag metadata")
            if arguments.tag in tags:
                raise RuntimeError(
                    f"GHCR tag already exists: {arguments.package}:{arguments.tag}"
                )
        if len(value) < 100:
            break
        page += 1
    print(f"GHCR tag is available: {arguments.package}:{arguments.tag}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
