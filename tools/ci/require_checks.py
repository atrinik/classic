#!/usr/bin/env python3
"""Fail closed when aggregating path-aware classic component checks."""

from __future__ import annotations

import argparse


class CheckResultError(RuntimeError):
    """Raised when a required-check input is missing, malformed, or failed."""


def require_success(label: str, result: str) -> None:
    if result != "success":
        raise CheckResultError(f"{label} did not succeed: {result!r}")


def require_component(label: str, required: str, result: str) -> None:
    if required not in {"true", "false"}:
        raise CheckResultError(
            f"{label} classifier output is not exactly true or false: {required!r}"
        )
    if required == "true":
        require_success(label, result)
    elif result != "skipped":
        raise CheckResultError(
            f"unselected {label} check has an unexpected result: {result!r}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    classic = commands.add_parser("classic")
    classic.add_argument("--classifier-result", required=True)
    classic.add_argument("--core-result", required=True)
    classic.add_argument("--client-required", required=True)
    classic.add_argument("--client-result", required=True)
    classic.add_argument("--server-required", required=True)
    classic.add_argument("--server-result", required=True)
    classic.add_argument("--windows-required", required=True)
    classic.add_argument("--windows-result", required=True)
    optional = commands.add_parser("optional")
    optional.add_argument("--label", required=True)
    optional.add_argument("--classifier-result", required=True)
    optional.add_argument("--required", required=True)
    optional.add_argument("--result", required=True)
    arguments = parser.parse_args()

    try:
        require_success("change classification", arguments.classifier_result)
        if arguments.command == "classic":
            require_success("core validation", arguments.core_result)
            require_component(
                "client", arguments.client_required, arguments.client_result
            )
            require_component(
                "server", arguments.server_required, arguments.server_result
            )
            require_component(
                "native Windows",
                arguments.windows_required,
                arguments.windows_result,
            )
        else:
            require_component(arguments.label, arguments.required, arguments.result)
    except CheckResultError as error:
        parser.exit(1, f"classic validation failed: {error}\n")
    print("validated every selected check")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
