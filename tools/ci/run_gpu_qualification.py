#!/usr/bin/env python3
"""Run one Linux GPU qualification process in its pinned compatible runtime.

The assigned runner supplies the workspace module and actual desktop/GPU
coordinates. Its capability report grants neither resource authority nor
hardware qualification; admission remains with the coordinator.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


FORWARDED_ENVIRONMENT = (
    "ATRINIK_BENCHMARK_REVISION", "ATRINIK_BENCHMARK_DIRTY",
    "ATRINIK_GPU_CONFORMANCE_DRIVER", "ATRINIK_GPU_CONFORMANCE_HARDWARE_TIER",
    "ATRINIK_GPU_CONFORMANCE_QUALIFIED_HARDWARE", "ATRINIK_GPU_CONFORMANCE_OUTPUT",
    "ATRINIK_GPU_CONFORMANCE_LIFECYCLE_OUTPUT",
    "ATRINIK_GPU_CONFORMANCE_REVIEW_DIRECTORY",
)


def desktop_arguments(environment: dict[str, str]) -> list[str]:
    workspace = Path(environment["ATRINIK_QUALIFICATION_WORKSPACE_ROOT"])
    if not workspace.is_absolute() or not (workspace / "atrinik_workspace/linux_platform.py").is_file():
        raise ValueError("qualified runner must supply its verified workspace module")
    gpu = environment["ATRINIK_QUALIFICATION_GPU_KIND"]
    if gpu not in ("mesa", "nvidia"):
        raise ValueError("assigned GPU kind must be mesa or nvidia")
    command = [sys.executable, "-m", "atrinik_workspace.linux_platform",
               "--desktop", "x11", "--gpu", gpu, "--audio"]
    if gpu == "mesa":
        command += ["--render-device", environment["ATRINIK_QUALIFICATION_RENDER_DEVICE"]]
    # Check completion before parsing: failed session probes produce no report.
    result = subprocess.run(command, cwd=workspace, env=environment,
                            check=True, capture_output=True, text=True, timeout=30)
    report = json.loads(result.stdout)
    arguments = report.get("docker_arguments")
    if (report.get("schema_version") != 1 or
            report.get("delivery_authority") is not False or
            report.get("hardware_qualified") is not False or
            not isinstance(arguments, list) or not arguments or
            any(not isinstance(value, str) or "\x00" in value for value in arguments)):
        raise ValueError("invalid desktop capability report")
    return arguments


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--check-runtime", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    options = parser.parse_args(argv)
    try:
        environment = dict(os.environ)
        root = options.source_root.resolve(strict=True)
        if any(character in str(root) for character in ",\n\r"):
            raise ValueError("ambiguous source mount")
        image = environment["CLASSIC_LINUX_RUNTIME_IMAGE"]
        if not re.fullmatch(r"ghcr\.io/atrinik/linux-build:[^\s@]+@sha256:[0-9a-f]{64}", image):
            raise ValueError("qualification requires the immutable qualified runtime image")
        capabilities = desktop_arguments(environment)
        if options.check_runtime:
            if options.command:
                raise ValueError("runtime check takes no executable")
            subprocess.run(["docker", "info", "--format", "{{.ServerVersion}}"],
                           check=True, capture_output=True, text=True, timeout=30)
            return 0
        command = options.command
        if command and command[0] == "--":
            command = command[1:]
        if not command or command[0] not in ("atrinik", "client-gpu-renderer-integration-tests"):
            raise ValueError("unsupported qualification executable")
        executable = root / "client/build/gpu-qualification" / command[0]
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise ValueError("missing qualification executable")
        arguments = ["docker", "run", "--rm", "--network", "none",
                     "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
                     "--tmpfs", "/tmp:rw,nosuid,nodev,mode=1777", *capabilities,
                     "--mount", f"type=bind,source={root},target=/workspace",
                     "--workdir", "/workspace/client", "--env", "HOME=/tmp/classic-home",
                     "--env", "ATRINIK_CONFIG_DIR=/tmp/classic-home/config"]
        for name in FORWARDED_ENVIRONMENT:
            if name in environment:
                arguments += ["--env", f"{name}={environment[name]}"]
        arguments += [image, "bash", "-c", 'mkdir -p "$HOME"; exec "$@"', "--",
                      "/workspace/client/build/gpu-qualification/" + command[0], *command[1:]]
        return subprocess.run(arguments, check=False).returncode
    except (KeyError, ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"GPU qualification runtime: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
