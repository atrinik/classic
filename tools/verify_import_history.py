#!/usr/bin/env python3
"""Verify the immutable evidence for the classic history consolidation."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs" / "history" / "imports.json"
COMPONENT_RELEASE_MAP = ROOT / "docs" / "history" / "component-release-map.json"
RELEASE_CONFIG = ROOT / ".releaserc.cjs"
SEMVER_RE = re.compile(r"v(\d+)\.(\d+)\.(\d+)")
LAST_COMPONENT_TAGS = {
    "client": "v5.3.1",
    "server": "v5.5.1",
    "editor": "v1.0.7",
    "libatrinik": "v1.1.6",
    "protocol": "v1.0.11",
}


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


def load_release_config() -> dict[str, Any]:
    result = subprocess.run(
        [
            "node",
            "-e",
            "const c=require(process.argv[1]);"
            "process.stdout.write(JSON.stringify(c,(_k,v)=>"
            "typeof v==='function'?'__function__':v));",
            str(RELEASE_CONFIG),
        ],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(
            "cannot load semantic-release configuration: "
            + (result.stderr.strip() or result.stdout.strip())
        )
    value = json.loads(result.stdout)
    require(isinstance(value, dict), "release configuration is not an object")
    return value


def semantic_version(tag: str) -> tuple[int, int, int]:
    match = SEMVER_RE.fullmatch(tag)
    if match is None:
        raise RuntimeError(f"release tag is not an unprefixed semantic version: {tag}")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def is_ancestor(older: str, newer: str) -> bool:
    return (
        subprocess.run(
            ["git", "-C", str(ROOT), "merge-base", "--is-ancestor", older, newer],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0
    )


def first_parent_commits(revision: str) -> set[str]:
    return set(git("rev-list", "--first-parent", revision).splitlines())


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
    require(value.get("schema_version") == 3, "unsupported import manifest")
    require(
        value.get("source_history_policy")
        == {
            "classic_history_refs": "retired",
            "original_graphs": "archived-source-repositories",
        },
        "unexpected source-history policy",
    )
    retired_refs = value.get("retired_history_refs")
    require(isinstance(retired_refs, dict) and retired_refs, "no retired history refs")
    require(
        all(
            isinstance(ref, str)
            and ref.startswith("history/")
            and isinstance(commit, str)
            and re.fullmatch(r"[0-9a-f]{40}", commit)
            for ref, commit in retired_refs.items()
        ),
        "invalid retired history ref evidence",
    )
    components = value.get("components")
    require(isinstance(components, list) and components, "no imported components")
    component_names = {component["name"] for component in components}
    expected_refs = {
        f"history/{name}/main" for name in component_names
    } | {
        f"history/original/{name}/main" for name in component_names
    }
    expected_refs.update(
        {
            "history/client/pr-48",
            "history/server/feat/stable-content-identities-port",
        }
    )
    require(
        set(retired_refs) == expected_refs,
        "retired history ref evidence is incomplete or unexpected",
    )
    return value


def verify_component(
    component: dict[str, Any], retired_refs: dict[str, str]
) -> None:
    name = component["name"]
    prefix = component["prefix"]
    integration = component["integration_commit"]
    rewritten = component["rewritten_tip"]
    source = component["source_tip"]
    require(
        retired_refs.get(f"history/{name}/main") == rewritten,
        f"{name}: rewritten retired-ref target changed",
    )
    require(
        retired_refs.get(f"history/original/{name}/main") == source,
        f"{name}: original retired-ref target changed",
    )

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

    # The immutable map records rewritten commits from every imported ref, while
    # only the component main graph is integrated into the monorepo main line.
    # Branch-only rewritten objects are intentionally allowed to disappear
    # after the history namespace is retired; their originals remain in the
    # archived source repository.
    verify_commits(name, [rewritten])


def verify_component_release_map(manifest: dict[str, Any]) -> None:
    value = json.loads(COMPONENT_RELEASE_MAP.read_text(encoding="utf-8"))
    require(value.get("schema_version") == 1, "unsupported component release map")
    require(
        value.get("unified_release")
        == {
            "first_tag": "v5.6.0",
            "ancestry_floor": "6960d16988e6925d7e421dc549780ac5feb0914d",
            "repository": "atrinik/classic",
            "version_source": "semantic-release",
        },
        "unexpected unified component-release boundary",
    )
    records = value.get("components")
    require(isinstance(records, list), "component release map has no records")
    by_name = {
        record.get("name"): record for record in records if isinstance(record, dict)
    }
    require(len(records) == len(by_name), "component release map has duplicate records")
    require(set(by_name) == set(LAST_COMPONENT_TAGS), "component release map names differ")
    for component in manifest["components"]:
        name = component["name"]
        record = by_name[name]
        require(
            record
            == {
                "name": name,
                "archived_repository": component["source_repository"],
                "last_component_tag": LAST_COMPONENT_TAGS[name],
                "original_commit": component["source_tip"],
                "rewritten_commit": component["rewritten_tip"],
                "unified_artifact": f"atrinik-classic-{name}-${{version}}.tar.gz",
            },
            f"{name}: component release boundary changed",
        )


def verify_release_tags(manifest: dict[str, Any], release_history_ref: str) -> None:
    policy_path = ROOT / manifest["active_release_tags"]
    policy = json.loads(policy_path.read_text(encoding="utf-8"))
    require(policy.get("schema_version") == 2, "unsupported release-tag policy")
    require(policy.get("policy") == "unified-classic", "unexpected release-tag policy")
    historical_tags = policy.get("tags")
    require(
        isinstance(historical_tags, dict) and historical_tags,
        "no historical release tags",
    )
    require(
        policy.get("baseline")
        == {
            "tag": "v5.0.19",
            "commit": "f2cdf68710d157d4fae44a0582972129e6c4db9e",
        },
        "unexpected release-tag baseline",
    )
    require(
        all(SEMVER_RE.fullmatch(tag) for tag in historical_tags),
        "release tags must be unprefixed semantic versions",
    )
    require(
        all(
            isinstance(commit, str) and re.fullmatch(r"[0-9a-f]{40}", commit)
            for commit in historical_tags.values()
        ),
        "release tag targets must be full lowercase commit IDs",
    )
    require(
        len(historical_tags) == len(set(historical_tags.values())),
        "historical release tags must have unique targets",
    )

    future_policy = policy.get("future_tags")
    require(isinstance(future_policy, dict), "no future release-tag policy")
    require(
        future_policy
        == {
            "first_version": "v5.6.0",
            "minimum_version": "v5.6.0",
            "maximum_major": 5,
            "ancestry_floor": "6960d16988e6925d7e421dc549780ac5feb0914d",
            "branch": "main",
            "driver": "semantic-release",
        },
        "unexpected future release-tag policy",
    )
    first_version = semantic_version(future_policy["first_version"])
    minimum_version = semantic_version(future_policy["minimum_version"])
    maximum_major = future_policy["maximum_major"]
    require(
        first_version == minimum_version,
        "first and minimum future versions differ",
    )
    require(
        first_version[0] == maximum_major,
        "first version exceeds the major-version cap",
    )
    floor = future_policy["ancestry_floor"]
    require(
        is_ancestor(floor, release_history_ref),
        f"future-tag ancestry floor is not in {release_history_ref}",
    )

    failed_releases = policy.get("failed_releases")
    require(
        failed_releases
        == {
            "v5.8.1": {
                "commit": "4653cb0a5f8bb11f5f3b522008bdd28c39d8c14c",
                "disposition": "delete-empty-draft",
                "empty_draft_id": 367395490,
                "failed_package_run_ids": [31298735525, 31341539056],
                "server_image_conclusion": "failure",
            },
            "v5.10.0": {
                "commit": "ebfe6588cf64f42c44715bcf45ec50cc056a91a5",
                "disposition": "delete-empty-draft",
                "empty_draft_id": 368181077,
                "failed_package_run_ids": [31429488922],
                "server_image_conclusion": "success",
            },
        },
        "unexpected failed-release policy",
    )
    for tag, record in failed_releases.items():
        require(
            git("rev-parse", f"{tag}^{{commit}}") == record["commit"],
            f"{tag}: failed release target changed",
        )
        require(
            is_ancestor(record["commit"], release_history_ref),
            f"{tag}: target is not in {release_history_ref}",
        )

    head_first_parent_commits = first_parent_commits("HEAD")
    release_first_parent_commits = first_parent_commits(release_history_ref)
    release_config = load_release_config()
    require(
        release_config.get("branches") == ["main"],
        "unexpected release branches",
    )
    require(release_config.get("tagFormat") == "v${version}", "unexpected tag format")
    plugins = release_config.get("plugins")
    require(
        isinstance(plugins, list) and plugins,
        "release configuration has no plugins",
    )
    analyzer = plugins[0]
    require(
        isinstance(analyzer, list)
        and len(analyzer) == 2
        and analyzer[0] == "@semantic-release/commit-analyzer"
        and isinstance(analyzer[1], dict),
        "unexpected commit analyzer configuration",
    )
    release_rules = analyzer[1].get("releaseRules")
    require(
        isinstance(release_rules, list) and release_rules,
        "commit analyzer has no release rules",
    )
    require(
        release_rules[0] == {"release": False},
        "non-first-parent commits are not denied by default",
    )
    expected_rules = [
        {"breaking": True, "release": "minor"},
        {"type": "feat", "release": "minor"},
        {"type": "*", "release": "patch"},
    ]
    rules_by_hash: dict[str, list[dict[str, Any]]] = {}
    for rule in release_rules[1:]:
        require(isinstance(rule, dict), "release rule is not an object")
        commit = rule.get("hash")
        require(
            isinstance(commit, str) and re.fullmatch(r"[0-9a-f]{40}", commit),
            "release rule has no full commit ID",
        )
        rules_by_hash.setdefault(commit, []).append(
            {key: value for key, value in rule.items() if key != "hash"}
        )
    require(
        set(rules_by_hash) == head_first_parent_commits,
        "release rules do not select exactly HEAD's first-parent history",
    )
    require(
        all(rules == expected_rules for rules in rules_by_hash.values()),
        "a first-parent commit has unexpected release rules",
    )
    notes_plugins = [
        plugin
        for plugin in plugins
        if isinstance(plugin, list)
        and len(plugin) == 2
        and plugin[0] == "@semantic-release/release-notes-generator"
        and isinstance(plugin[1], dict)
    ]
    require(len(notes_plugins) == 1, "release configuration has no unique notes plugin")
    require(
        notes_plugins[0][1].get("writerOpts", {}).get("skip") == "__function__",
        "release notes do not filter non-first-parent commits",
    )
    exec_plugins = [
        plugin
        for plugin in plugins
        if isinstance(plugin, list)
        and len(plugin) == 2
        and plugin[0] == "@semantic-release/exec"
        and isinstance(plugin[1], dict)
    ]
    require(len(exec_plugins) == 1, "release configuration has no unique exec plugin")
    github_plugins = [
        plugin
        for plugin in plugins
        if isinstance(plugin, list)
        and len(plugin) == 2
        and plugin[0] == "@semantic-release/github"
        and plugin[1]
        == {
            "draftRelease": True,
            "failCommentCondition": False,
            "successCommentCondition": False,
        }
    ]
    require(
        len(github_plugins) == 1,
        "semantic-release must create a draft before artifact publication",
    )
    require(
        exec_plugins[0][1].get("verifyReleaseCmd")
        == "python3 tools/release/verify_next_version.py ${nextRelease.version}",
        "release configuration has no immutable pre-tag version guard",
    )
    require(
        exec_plugins[0][1].get("publishCmd")
        == "tools/release/queue_package_release.sh ${nextRelease.version}",
        "release configuration does not queue the unified package workflow",
    )

    actual_refs = git(
        "for-each-ref", "--format=%(refname:short)", "refs/tags/"
    ).splitlines()
    actual = set(actual_refs)
    historical = set(historical_tags)
    require(historical <= actual, "an immutable historical release tag is missing")
    for tag, commit in historical_tags.items():
        require(git("rev-parse", f"{tag}^{{commit}}") == commit, f"{tag}: target changed")
        require(
            is_ancestor(commit, release_history_ref),
            f"{tag}: target is not in {release_history_ref}",
        )

    future_tags = sorted(actual - historical, key=semantic_version)
    if future_tags:
        require(
            semantic_version(future_tags[0]) == first_version,
            "the first post-consolidation release must be v5.6.0",
        )
    previous_commit = floor
    targets = set(historical_tags.values())
    for tag in future_tags:
        require(
            semantic_version(tag) >= minimum_version,
            f"{tag}: future release version predates v5.6.0",
        )
        require(
            semantic_version(tag)[0] == maximum_major,
            f"{tag}: classic releases must remain on major version 5",
        )
        commit = git("rev-parse", f"{tag}^{{commit}}")
        require(commit not in targets, f"{tag}: release tag target is not unique")
        require(
            commit in release_first_parent_commits,
            f"{tag}: target is not on {release_history_ref}'s first-parent line",
        )
        require(is_ancestor(previous_commit, commit), f"{tag}: versions are not ancestry ordered")
        require(
            is_ancestor(commit, release_history_ref),
            f"{tag}: target is not in {release_history_ref}",
        )
        targets.add(commit)
        previous_commit = commit


def parse_args(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--release-history-ref",
        default="HEAD",
        help="trusted branch topology used to validate release-tag ancestry",
    )
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    args = parse_args(arguments)
    try:
        manifest = load_manifest()
        for component in manifest["components"]:
            verify_component(component, manifest["retired_history_refs"])
        verify_component_release_map(manifest)
        verify_release_tags(manifest, args.release_history_ref)
    except (OSError, KeyError, ValueError, RuntimeError) as error:
        print(f"history verification failed: {error}", file=sys.stderr)
        return 1
    print(f"verified {len(manifest['components'])} imported component histories")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
