#!/usr/bin/env python3
"""Select the trusted release-history topology for a Check event."""

from __future__ import annotations

import argparse


CANONICAL_MAIN = "refs/remotes/origin/main"


class ReleaseHistoryRefError(RuntimeError):
    """Raised when a Check event has no release-history policy."""


def release_history_ref(event: str) -> str:
    if event in {"pull_request", "workflow_dispatch"}:
        return CANONICAL_MAIN
    if event in {"push", "merge_group"}:
        return "HEAD"
    raise ReleaseHistoryRefError(f"unsupported Check event: {event}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--event", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    print(release_history_ref(args.event))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ReleaseHistoryRefError as error:
        raise SystemExit(str(error)) from error
