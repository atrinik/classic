#!/usr/bin/env python3
"""Compare server probes using each revision's authenticated benchmark producer."""
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

WORKFLOW = ".github/workflows/pr-benchmarks.yml"
RUNNER = "tools/ci/run_linux_check.sh"
LLVM_IMAGE = "ghcr.io/atrinik/classic-build:1.18.0@sha256:86d3c4182dc786db8ace56d9d8dc57ff4f14c1c4f517b728547626f576280449"
GCC_IMAGE = "ghcr.io/atrinik/classic-build:1.16.0@sha256:e1c366dbf83ef987765ff913bbb193868314a139ae1e00cc134b22a3159464f2"
LLVM_COMPILER = "clang-23.1.3-lld-compiler-rt"
GCC_COMPILER = "gcc-15.2.0"
# Authenticate the original producer contract, not a whitelist of source commits.
HISTORICAL_CONTRACT = {
    WORKFLOW: "c656631f0d880e0d6f09ab524015a2114ffb19d559e8fd534316583b90115878",
    RUNNER: "3df551f030bf55ee1dd23579572727d26865f3d8320e3dc6e1e60d17495817c8",
}
LOCKS = ("client/dependencies.lock.json", "server/dependencies.lock.json",
         "server/cmake/immutable_sources.lock.json")


def run(arguments: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(arguments, check=True, **kwargs)


def git(root: Path, *arguments: str) -> bytes:
    return run(["git", "-C", str(root), *arguments], capture_output=True, timeout=30).stdout


def clean_revision(root: Path, revision: str) -> None:
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise ValueError("benchmark revision must be an exact lowercase commit SHA")
    if git(root, "rev-parse", "--verify", revision + "^{commit}").decode().strip() != revision:
        raise ValueError("benchmark revision is not the selected commit")
    if git(root, "rev-parse", "HEAD").decode().strip() != revision or git(
            root, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("benchmark checkout must be clean at its exact revision")


def committed(root: Path, revision: str, path: str) -> bytes:
    entry = git(root, "ls-tree", revision, "--", path).decode().split()
    if entry[:2] not in (["100644", "blob"], ["100755", "blob"]):
        raise ValueError(f"benchmark contract must be a regular Git blob: {path}")
    content = git(root, "show", revision + ":" + path)
    material = root / path
    if material.is_symlink() or material.read_bytes() != content:
        raise ValueError(f"benchmark contract differs from its committed bytes: {path}")
    return content


def resolve(root: Path, revision: str) -> dict:
    clean_revision(root, revision)
    workflow = committed(root, revision, WORKFLOW)
    fields = {}
    for key in ("CLASSIC_LINUX_IMAGE", "CLASSIC_LINUX_IMAGE_DIGEST", "CLASSIC_LINUX_CCACHE_COMPILER"):
        values = re.findall(rf"^  {key}: ([^\s]+)$", workflow.decode(), re.MULTILINE)
        if len(values) != 1:
            raise ValueError(f"benchmark workflow has no unique producer field: {key}")
        fields[key] = values[0]
    image, compiler = fields["CLASSIC_LINUX_IMAGE"], fields["CLASSIC_LINUX_CCACHE_COMPILER"]
    if fields["CLASSIC_LINUX_IMAGE_DIGEST"] != image.split("@")[-1]:
        raise ValueError("benchmark image and digest differ")
    if (image, compiler) == (GCC_IMAGE, GCC_COMPILER):
        for path, digest in HISTORICAL_CONTRACT.items():
            if hashlib.sha256(committed(root, revision, path)).hexdigest() != digest:
                raise ValueError("baseline has no approved historical benchmark producer contract")
    elif (image, compiler) == (LLVM_IMAGE, LLVM_COMPILER):
        committed(root, revision, "cmake/toolchains/llvm.cmake")
        committed(root, revision, "cmake/AtrinikLLVM.cmake")
    else:
        raise ValueError("benchmark source has no approved compiler/image cohort")
    return {"revision": revision, "image": image, "compiler": compiler,
            "workflow_sha256": hashlib.sha256(workflow).hexdigest(),
            "locks": {path: hashlib.sha256(committed(root, revision, path)).hexdigest()
                      for path in LOCKS}}


def baseline_probe(root: Path, revision: str, trusted_root: Path, evidence: Path,
                   inventory: Path = Path("/usr/local/share/atrinik/classic-toolchain.json")) -> None:
    evidence.unlink(missing_ok=True)
    descriptor = resolve(root, revision)
    toolchain = json.loads(inventory.read_text())
    if toolchain["target"] != "classic-final" or toolchain["platform"] != "linux/amd64":
        raise ValueError("baseline producer inventory has the wrong target/platform")
    llvm = descriptor["compiler"] == LLVM_COMPILER
    tool, version = ("clang", "23.1.3") if llvm else ("gcc", "15.2.0")
    if toolchain["tools"].get(tool) != version:
        raise ValueError("baseline producer inventory has the wrong compiler")
    Path(os.environ.get("HOME", "/tmp/classic-home")).mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, CC="clang" if llvm else "gcc",
                       CXX="clang++" if llvm else "g++",
                       ATRINIK_LLVM_TOOLCHAIN="1" if llvm else "0",
                       CCACHE_BASEDIR=str(root), CCACHE_COMPILERCHECK="content",
                       CCACHE_NOHASHDIR="true", CCACHE_TEMPDIR=str(root / "build/ccache-temp"))
    Path(environment["CCACHE_TEMPDIR"]).mkdir(parents=True, exist_ok=True)
    dependencies = trusted_root / "server/tools/dependencies.py"
    bundle = trusted_root / "build/dependency-inputs"
    run(["python3", str(dependencies), "bundle-verify", "--client-lock", str(root / LOCKS[0]),
         "--server-lock", str(root / LOCKS[1]), "--source-lock", str(root / LOCKS[2]),
         "--bundle", str(bundle)], env=environment)
    for command in ("sync", "verify"):
        arguments = ["python3", str(dependencies), command, "--root", str(root / "server"),
                     "--lock", str(root / LOCKS[1])]
        if command == "sync":
            arguments += ["--cache", str(bundle / "downloads"), "--refresh", "--offline"]
        run(arguments, env=environment)
    pcp = run(["python3", str(dependencies), "source", "--source-lock", str(root / LOCKS[2]),
               "--source-name", "libpcpnatpmp", "--cache", str(root / "build/dependency-source-cache"),
               "--downloads", str(bundle / "downloads"), "--offline"],
              env=environment, capture_output=True, text=True).stdout.strip()
    build = root / "server/build" / ("llvm-benchmark-baseline" if llvm else "gcc-benchmark-baseline")
    arguments = ["cmake", "--preset", "linux-release", "-B", str(build),
                 "-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
                 f"-DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL={root / 'protocol'}",
                 f"-DFETCHCONTENT_SOURCE_DIR_LIBATRINIK={root / 'libatrinik'}",
                 f"-DFETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP={pcp}"]
    if llvm:
        arguments.append(f"-DCMAKE_TOOLCHAIN_FILE={root / 'cmake/toolchains/llvm.cmake'}")
    run(arguments, cwd=root / "server", env=environment)
    run(["cmake", "--build", str(build), "--parallel", str(len(os.sched_getaffinity(0)))], env=environment)
    run(["ctest", "--test-dir", str(build), "--verbose", "--no-tests=error",
         "-R", "^server-content-benchmark$"], env=environment)
    clean_revision(root, revision)
    evidence.write_text(json.dumps(dict(descriptor, completed=True), indent=2) + "\n")


def container(image: str, root: Path, cache: Path, environment: dict[str, str],
              command: list[str]) -> list[str]:
    arguments = ["docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}", "--network", "none",
                 "--volume", f"{root}:{root}", "--volume", f"{cache}:/cache/ccache", "--workdir", str(root)]
    for key, value in dict(CCACHE_DIR="/cache/ccache", CCACHE_MAXSIZE="500M",
                           HOME="/tmp/classic-home", **environment).items():
        arguments += ["--env", f"{key}={value}"]
    return arguments + [image, *command]


def compare(root: Path, base: str, head: str, cache: Path) -> None:
    if re.fullmatch(r"[0-9a-f]{40}", base) is None:
        raise ValueError("benchmark base must be an exact lowercase commit SHA")
    if git(root, "rev-parse", "--verify", base + "^{commit}").decode().strip() != base:
        raise ValueError("benchmark base is not the selected commit")
    candidate = resolve(root, head)
    if candidate["compiler"] != LLVM_COMPILER:
        raise ValueError("current benchmark candidate must use the strict LLVM producer")
    baseline = root / "build/ci-server-benchmark-baseline"
    evidence = root / "build/ci-evidence"
    evidence.mkdir(parents=True, exist_ok=True)
    summary = evidence / "server-content-benchmark-summary.md"
    summary.unlink(missing_ok=True)
    base_receipt = evidence / "server-content-benchmark-base.json"
    base_receipt.unlink(missing_ok=True)
    (evidence / "server-content-benchmark-head.json").unlink(missing_ok=True)
    run(["git", "-C", str(root), "worktree", "add", "--detach", str(baseline), base])
    try:
        original = resolve(baseline, base)
        for side, descriptor in (("base", original), ("head", candidate)):
            side_cache = cache / side / descriptor["compiler"]
            side_cache.mkdir(parents=True, exist_ok=True)
            side_cache.chmod(0o1777)
            # Pulls remain anonymous and happen on the host, never in a build container.
            run(["docker", "pull", descriptor["image"]])
            if side == "base":
                command = ["python3", str(root / "tools/ci/server_benchmark.py"), "probe",
                           "--source-root", str(baseline), "--revision", base,
                           "--trusted-root", str(root), "--evidence", str(base_receipt)]
                environment = {"ATRINIK_LLVM_TOOLCHAIN": "1" if original["compiler"] == LLVM_COMPILER else "0"}
            else:
                command = [str(root / RUNNER), "server-benchmark", str(root)]
                environment = {"ATRINIK_LLVM_TOOLCHAIN": "1", "ATRINIK_BENCHMARK_BASE_SHA": base,
                               "ATRINIK_BENCHMARK_REVISION": head}
            with (evidence / f"server-content-benchmark-{side}.log").open("w") as log:
                run(container(descriptor["image"], root, side_cache, environment, command),
                    stdout=log, stderr=subprocess.STDOUT)
            clean_revision(baseline if side == "base" else root, descriptor["revision"])
            if side == "base":
                if json.loads(base_receipt.read_text()) != dict(original, completed=True):
                    raise ValueError("baseline probe did not produce the exact completed producer receipt")
        if json.loads(base_receipt.read_text()) != dict(original, completed=True):
            raise ValueError("baseline producer receipt changed during candidate execution")
        clean_revision(baseline, base)
        (evidence / "server-content-benchmark-head.json").write_text(
            json.dumps(dict(candidate, completed=True), indent=2) + "\n")
        summary.write_text("## Server content benchmark\n\n"
                           f"- Base: `{base}` ({original['compiler']}, `{original['image']}`)\n"
                           f"- Head: `{head}` ({candidate['compiler']}, `{candidate['image']}`)\n"
                           "- Result: both isolated content-loader probes completed successfully.\n"
                           "- Each compiler/image is a separate environment cohort; cross-cohort timings are informational.\n"
                           "- Evidence: `server-content-benchmark-base.log`, `server-content-benchmark-head.log` and producer JSON receipts.\n")
    finally:
        # No force: unexpected baseline changes remain available for inspection.
        run(["git", "-C", str(root), "worktree", "remove", str(baseline)])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    compare_parser = sub.add_parser("compare")
    compare_parser.add_argument("--base", required=True)
    compare_parser.add_argument("--head", required=True)
    compare_parser.add_argument("--cache", type=Path, required=True)
    probe_parser = sub.add_parser("probe")
    probe_parser.add_argument("--revision", required=True)
    probe_parser.add_argument("--trusted-root", type=Path, required=True)
    probe_parser.add_argument("--evidence", type=Path, required=True)
    for child in (compare_parser, probe_parser):
        child.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "compare":
            compare(args.source_root.resolve(), args.base, args.head, args.cache.resolve())
        else:
            baseline_probe(args.source_root.resolve(), args.revision, args.trusted_root.resolve(), args.evidence)
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"server benchmark failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
