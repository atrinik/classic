#!/usr/bin/env python3
"""Verify the immutable evidence for the classic history consolidation."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs" / "history" / "imports.json"


def git(*arguments: str, check: bool = True) -> str:
    result = subprocess.run(
        ["git", "-C", str(ROOT), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if check and result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"git {' '.join(arguments)}: {detail}")
    return result.stdout.strip()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def verify_commits(name: str, commits: list[str]) -> None:
    result = subprocess.run(
        ["git", "-C", str(ROOT), "cat-file", "--batch-check=%(objectname) %(objecttype)"],
        check=False,
        input="".join(f"{commit}^{{commit}}\n" for commit in commits),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(f"{name}: cannot inspect mapped commits: {result.stderr.strip()}")
    rows = result.stdout.splitlines()
    require(len(rows) == len(commits), f"{name}: incomplete object check")
    require(
        all(row.endswith(" commit") and " missing" not in row for row in rows),
        f"{name}: a mapped commit object is missing",
    )


def load_manifest() -> dict[str, Any]:
    value = json.loads(MANIFEST.read_text(encoding="utf-8"))
    require(value.get("schema_version") == 2, "unsupported import manifest")
    components = value.get("components")
    require(isinstance(components, list) and components, "no imported components")
    return value


def verify_component(component: dict[str, Any]) -> None:
    name = component["name"]
    prefix = component["prefix"]
    integration = component["integration_commit"]
    rewritten = component["rewritten_tip"]
    source = component["source_tip"]
    original_history_ref = component["original_history_ref"]

    original_ref_targets = [
        git("rev-parse", ref, check=False)
        for ref in (
            f"refs/heads/{original_history_ref}",
            f"refs/remotes/origin/{original_history_ref}",
        )
    ]
    require(source in original_ref_targets, f"{name}: original history ref is missing")

    require(
        subprocess.run(
            ["git", "-C", str(ROOT), "merge-base", "--is-ancestor", rewritten, integration],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0,
        f"{name}: rewritten tip is not integrated",
    )
    require(
        subprocess.run(
            ["git", "-C", str(ROOT), "merge-base", "--is-ancestor", integration, "HEAD"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0,
        f"{name}: integration commit is not in HEAD",
    )
    require(
        git("rev-parse", f"{integration}:{prefix}") == component["source_tree"],
        f"{name}: import-time subtree changed",
    )
    require(
        git("rev-parse", f"{integration}:{prefix}/AGENTS.md")
        == component["agents_blob"],
        f"{name}: imported AGENTS.md blob does not match",
    )

    map_path = ROOT / component["commit_map"]
    digest = hashlib.sha256(map_path.read_bytes()).hexdigest()
    require(digest == component["commit_map_sha256"], f"{name}: commit-map digest changed")
    lines = map_path.read_text(encoding="utf-8").splitlines()
    require(lines and lines[0].split() == ["old", "new"], f"{name}: invalid map header")
    pairs = [line.split() for line in lines[1:] if line.strip()]
    require(all(len(pair) == 2 for pair in pairs), f"{name}: malformed map row")
    mapping = dict(pairs)
    require(len(mapping) == len(pairs), f"{name}: duplicate original commit")
    require(len(set(mapping.values())) == len(pairs), f"{name}: duplicate rewritten commit")
    require(len(pairs) == component["commit_count"], f"{name}: commit count changed")
    require(mapping.get(source) == rewritten, f"{name}: source tip mapping changed")

    verify_commits(name, [mapped for _, mapped in pairs] + [source])



def verify_release_tags(manifest: dict[str, Any]) -> None:
    policy_path = ROOT / manifest["active_release_tags"]
    policy = json.loads(policy_path.read_text(encoding="utf-8"))
    require(policy.get("schema_version") == 1, "unsupported release-tag policy")
    require(policy.get("policy") == "unified-classic", "unexpected release-tag policy")
    tags = policy.get("tags")
    require(isinstance(tags, dict) and tags, "no active release tags")
    require(
        policy.get("baseline")
        == {
            "tag": "v5.0.19",
            "commit": "f2cdf68710d157d4fae44a0582972129e6c4db9e",
        },
        "unexpected release-tag baseline",
    )
    require(
        all(re.fullmatch(r"v\d+\.\d+\.\d+", tag) for tag in tags),
        "release tags must be unprefixed semantic versions",
    )
    require(
        all(isinstance(commit, str) and re.fullmatch(r"[0-9a-f]{40}", commit) for commit in tags.values()),
        "release tag targets must be full lowercase commit IDs",
    )
    require(len(tags) == len(set(tags.values())), "release tags must have unique targets")

    actual_refs = git("for-each-ref", "--format=%(refname:short)", "refs/tags/").splitlines()
    require(sorted(actual_refs) == sorted(tags), "active tag names differ from release-tag policy")
    for tag, commit in tags.items():
        require(git("rev-parse", f"{tag}^{{commit}}") == commit, f"{tag}: target changed")
        require(
            subprocess.run(
                ["git", "-C", str(ROOT), "merge-base", "--is-ancestor", commit, "HEAD"],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            ).returncode
            == 0,
            f"{tag}: target is not in HEAD",
        )


def main() -> int:
    try:
        manifest = load_manifest()
        for component in manifest["components"]:
            verify_component(component)
        verify_release_tags(manifest)
    except (OSError, KeyError, ValueError, RuntimeError) as error:
        print(f"history verification failed: {error}", file=sys.stderr)
        return 1
    print(f"verified {len(manifest['components'])} imported component histories")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
