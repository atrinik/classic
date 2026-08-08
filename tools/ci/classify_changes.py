#!/usr/bin/env python3
"""Classify a trusted Git diff for path-aware classic CI and CodeQL."""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
FULL_EVENTS = {"merge_group", "push", "schedule", "workflow_dispatch"}
SHA_RE = re.compile(r"[0-9a-f]{40}")
NATIVE_SHARED_PREFIXES = ("libatrinik/", "protocol/")
NATIVE_SHARED_PATHS = {
    ".clang-format",
    ".gitattributes",
    ".releaserc.cjs",
    "ATTRIBUTIONS.md",
    "CMakeLists.txt",
    "CMakePresets.json",
    "LICENSE.md",
    "docs/history/component-release-map.json",
    "docs/history/release-tags.json",
}
NATIVE_SHARED_TOOL_PREFIXES = (
    ".github/workflows/",
    "cmake/",
    "tools/ci/",
    "tools/release/",
)


class ClassificationError(RuntimeError):
    """Raised when CI cannot safely determine the changed paths."""


def safe_path(value: str) -> str:
    path = PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts:
        raise ClassificationError(f"unsafe changed path: {value!r}")
    return value


def changed_paths(base: str, head: str, root: Path = ROOT) -> list[str]:
    for label, revision in (("base", base), ("head", head)):
        if SHA_RE.fullmatch(revision) is None:
            raise ClassificationError(f"{label} must be a full lowercase commit ID")
    result = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "diff",
            "--name-only",
            "--no-renames",
            "-z",
            f"{base}...{head}",
            "--",
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise ClassificationError(
            "git diff failed: " + result.stderr.decode("utf-8", errors="replace").strip()
        )
    return sorted(
        safe_path(value.decode("utf-8", errors="surrogateescape"))
        for value in result.stdout.split(b"\0")
        if value
    )


def is_native_shared(path: str) -> bool:
    return (
        path in NATIVE_SHARED_PATHS
        or path.startswith(NATIVE_SHARED_PREFIXES)
        or path.startswith(NATIVE_SHARED_TOOL_PREFIXES)
    )


def classify(
    paths: list[str], full: bool = False, codeql_scope: str = "union"
) -> dict[str, object]:
    checked = [safe_path(path) for path in paths]
    client = full or any(
        path.startswith("client/") or is_native_shared(path) for path in checked
    )
    server = full or any(
        path.startswith("server/") or is_native_shared(path) for path in checked
    )

    codeql_client = full or any(path.startswith("client/") for path in checked)
    codeql_server = full or any(path.startswith("server/") for path in checked)
    codeql_core_cpp = full or any(
        path.startswith(("libatrinik/", "protocol/", "cmake/"))
        or path in {".clang-format", "CMakeLists.txt", "CMakePresets.json"}
        for path in checked
    )
    c_cpp = codeql_client or codeql_server or codeql_core_cpp
    python = full or any(path.endswith(".py") for path in checked)
    actions = full or any(
        path.startswith(".github/workflows/")
        and (path.endswith(".yml") or path.endswith(".yaml"))
        for path in checked
    )
    languages = [
        language
        for language, selected in (
            ("c-cpp", c_cpp),
            ("python", python),
            ("actions", actions),
        )
        if selected
    ]

    if codeql_scope not in {
        "actions",
        "client",
        "core-cpp",
        "python",
        "server",
        "union",
    }:
        raise ClassificationError(f"unsupported CodeQL scope: {codeql_scope}")
    codeql_paths: set[str] = set()
    if codeql_scope in {"client", "union"} and codeql_client:
        codeql_paths.add("client")
    if codeql_scope in {"server", "union"} and codeql_server:
        codeql_paths.add("server")
    if codeql_scope in {"core-cpp", "union"} and codeql_core_cpp:
        codeql_paths.update(("libatrinik", "protocol"))
    if codeql_scope in {"python", "union"} and python:
        codeql_paths.add(".")
    if codeql_scope in {"actions", "union"} and actions:
        codeql_paths.add(".github/workflows")
    if "." in codeql_paths:
        codeql_paths = {"."}

    return {
        "client": client,
        "server": server,
        "codeql_run": bool(languages),
        "codeql_languages": ",".join(languages),
        "codeql_client": codeql_client,
        "codeql_server": codeql_server,
        "codeql_core_cpp": codeql_core_cpp,
        "codeql_python": python,
        "codeql_actions": actions,
        "codeql_paths": sorted(codeql_paths),
    }


def write_codeql_config(path: Path, selected_paths: list[str]) -> None:
    lines = ['name: "Atrinik Classic path-aware analysis"']
    if selected_paths:
        lines.append("paths:")
        lines.extend(f"  - {json.dumps(value)}" for value in selected_paths)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_outputs(path: Path, result: dict[str, object], config: Path) -> None:
    entries = []
    if result["codeql_client"]:
        entries.append(
            {
                "category": "/partition:client/language:c-cpp",
                "display": "C/C++ client",
                "language": "c-cpp",
                "run": True,
                "scope": "client",
            }
        )
    if result["codeql_server"]:
        entries.append(
            {
                "category": "/partition:server/language:c-cpp",
                "display": "C/C++ server",
                "language": "c-cpp",
                "run": True,
                "scope": "server",
            }
        )
    if result["codeql_core_cpp"]:
        entries.append(
            {
                "category": "/partition:core/language:c-cpp",
                "display": "C/C++ core",
                "language": "c-cpp",
                "run": True,
                "scope": "core-cpp",
            }
        )
    if result["codeql_python"]:
        entries.append(
            {
                "category": "/partition:core/language:python",
                "display": "Python",
                "language": "python",
                "run": True,
                "scope": "python",
            }
        )
    if result["codeql_actions"]:
        entries.append(
            {
                "category": "/partition:core/language:actions",
                "display": "GitHub Actions",
                "language": "actions",
                "run": True,
                "scope": "actions",
            }
        )
    matrix = {
        "include": (
            entries
            or [
                {
                    "category": "/noop",
                    "display": "No supported changes",
                    "language": "none",
                    "run": False,
                    "scope": "union",
                }
            ]
        )
    }
    values = {
        "client": str(result["client"]).lower(),
        "server": str(result["server"]).lower(),
        "codeql_run": str(result["codeql_run"]).lower(),
        "codeql_languages": str(result["codeql_languages"]),
        "codeql_matrix": json.dumps(matrix, separators=(",", ":")),
        "codeql_config": str(config.resolve()),
    }
    with path.open("a", encoding="utf-8") as stream:
        for key, value in values.items():
            if "\n" in value or "\r" in value:
                raise ClassificationError(f"invalid multiline output: {key}")
            stream.write(f"{key}={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--event", required=True)
    parser.add_argument("--base", default="")
    parser.add_argument("--head", default="")
    parser.add_argument("--github-output", type=Path, required=True)
    parser.add_argument("--codeql-config", type=Path, required=True)
    parser.add_argument(
        "--codeql-scope",
        choices=("actions", "client", "core-cpp", "python", "server", "union"),
        default="union",
    )
    arguments = parser.parse_args()

    if arguments.event in FULL_EVENTS:
        full = True
        paths: list[str] = []
    elif arguments.event == "pull_request":
        full = False
        paths = changed_paths(arguments.base, arguments.head)
    else:
        parser.error(f"unsupported event: {arguments.event}")

    result = classify(paths, full=full, codeql_scope=arguments.codeql_scope)
    write_codeql_config(arguments.codeql_config, result["codeql_paths"])
    write_outputs(arguments.github_output, result, arguments.codeql_config)
    print(json.dumps({"event": arguments.event, "paths": paths, **result}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ClassificationError as error:
        raise SystemExit(f"change classification failed: {error}") from error
