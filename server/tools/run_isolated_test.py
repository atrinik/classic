#!/usr/bin/env python3
"""Run one server test with private writable runtime state and process ownership."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time


TEST_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


def _remove_owned(path: Path, parent: Path) -> None:
    if path.parent != parent:
        raise RuntimeError(f"refusing to remove runtime outside {parent}: {path}")
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def _link_directory(source: Path, destination: Path) -> None:
    destination.symlink_to(source.resolve(), target_is_directory=True)


def _copy_python_maps(seed_maps: Path, runtime_maps: Path) -> None:
    runtime_maps.mkdir()
    for source in seed_maps.iterdir():
        destination = runtime_maps / source.name
        if source.name == "python":
            if not source.is_dir():
                raise RuntimeError(f"missing Python map tree: {source}")
            shutil.copytree(source, destination, symlinks=True)
        elif source.is_dir():
            _link_directory(source, destination)
        else:
            destination.symlink_to(source.resolve())


def prepare_runtime(
    seed: Path, runtimes: Path, name: str, copy_python_maps: bool
) -> Path:
    if not TEST_NAME.fullmatch(name) or ".." in name:
        raise RuntimeError(f"invalid server test name: {name!r}")

    seed = seed.resolve(strict=True)
    seed_server = seed / "server"
    if not (seed / ".prepared").is_file() or not seed_server.is_dir():
        raise RuntimeError(f"invalid server test runtime seed: {seed}")

    runtimes.mkdir(parents=True, exist_ok=True)
    runtimes = runtimes.resolve(strict=True)
    destination = runtimes / name

    staging = Path(tempfile.mkdtemp(prefix=f".{name}-", dir=runtimes))
    try:
        runtime_server = staging / "server"
        runtime_server.mkdir()
        for source in seed_server.iterdir():
            target = runtime_server / source.name
            if source.name in {"maps", "lib", "resources"}:
                if source.name == "maps" and copy_python_maps:
                    _copy_python_maps(source, target)
                else:
                    _link_directory(source, target)
            elif source.name in {"assets", "tests"}:
                # These are output-only directories and must start empty.
                continue
            elif source.is_dir():
                shutil.copytree(source, target, symlinks=True)
            else:
                shutil.copy2(source, target, follow_symlinks=False)

        (staging / "tmp").mkdir()
        _remove_owned(destination, runtimes)
        staging.rename(destination)
    except BaseException:
        _remove_owned(staging, runtimes)
        raise

    return destination


def _signal_group(group_id: int, sig: int) -> None:
    try:
        os.killpg(group_id, sig)
    except ProcessLookupError:
        return


def _group_exists(group_id: int) -> bool:
    try:
        os.killpg(group_id, 0)
    except ProcessLookupError:
        return False
    return True


def _clean_process_group(group_id: int, grace: float = 5) -> None:
    if not _group_exists(group_id):
        return
    _signal_group(group_id, signal.SIGTERM)
    deadline = time.monotonic() + grace
    while _group_exists(group_id) and time.monotonic() < deadline:
        time.sleep(0.05)
    if _group_exists(group_id):
        _signal_group(group_id, signal.SIGKILL)


def run_owned(command: list[str], cwd: Path, timeout: float, env: dict[str, str]) -> int:
    process = subprocess.Popen(command, cwd=cwd, env=env, start_new_session=True)
    interrupted = 0

    def forward(signum: int, _frame: object) -> None:
        nonlocal interrupted
        interrupted = signum
        _signal_group(process.pid, signum)

    previous = {
        signum: signal.signal(signum, forward)
        for signum in (signal.SIGINT, signal.SIGTERM)
    }
    try:
        deadline = time.monotonic() + timeout
        while process.poll() is None and not interrupted:
            try:
                process.wait(timeout=min(0.1, max(0.0, deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                pass
            if time.monotonic() >= deadline:
                break

        timed_out = process.poll() is None and not interrupted
        if timed_out:
            print(
                f"isolated server test exceeded {timeout:g} seconds; terminating process group",
                file=sys.stderr,
            )
            _signal_group(process.pid, signal.SIGTERM)

        if process.poll() is None:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                _signal_group(process.pid, signal.SIGKILL)
                process.wait()
        _clean_process_group(process.pid)
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)

    if interrupted:
        return 128 + interrupted
    if timed_out:
        return 124
    return process.returncode


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=Path, required=True)
    parser.add_argument("--runtimes", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--timeout", type=float, default=270)
    parser.add_argument("--copy-python-maps", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if not args.command or args.command[0] != "--" or len(args.command) == 1:
        parser.error("a command must follow --")
    args.command = args.command[1:]
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    runtime = prepare_runtime(
        args.seed, args.runtimes, args.name, args.copy_python_maps
    )
    environment = os.environ.copy()
    environment["TMPDIR"] = str(runtime / "tmp")
    environment["ATRINIK_TEST_ARTIFACT_DIR"] = str(runtime)
    return run_owned(
        args.command, runtime / "server", args.timeout, environment
    )


if __name__ == "__main__":
    raise SystemExit(main())
