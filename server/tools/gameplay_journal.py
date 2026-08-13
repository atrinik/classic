#!/usr/bin/env python3
"""Validate and query Atrinik's private gameplay audit journal."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
from typing import Any, Iterable


SCHEMA_VERSION = 1
HASH_MARKER = b',"record_hash":"'
FORBIDDEN_FIELDS = {"password", "session_secret", "chat", "inscription", "text"}
TOKEN = re.compile(r"[A-Za-z0-9_.:/@+\-]{1,255}\Z")
IDENTITY = re.compile(r"[ -~]{1,255}\Z")
HASH = re.compile(r"[0-9a-f]{64}\Z")
KINDS = {"item", "currency", "quest", "progression"}
FILE_LIMIT = 9 * 1024 * 1024
JOURNAL_FILE = re.compile(r"journal-([0-9a-f]{32})-(\d{4,})\.jsonl\Z")


class JournalError(ValueError):
    """The journal does not satisfy its integrity or privacy contract."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise JournalError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _paths(inputs: Iterable[Path]) -> list[Path]:
    paths: list[Path] = []
    for value in inputs:
        metadata = value.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            raise JournalError(f"symbolic links are not journal inputs: {value}")
        if stat.S_ISDIR(metadata.st_mode):
            if os.name != "nt" and stat.S_IMODE(metadata.st_mode) != 0o700:
                raise JournalError(f"insecure journal directory permissions: {value}")
            paths.extend(sorted(value.glob("journal-*.jsonl")))
        elif stat.S_ISREG(metadata.st_mode):
            paths.append(value)
        else:
            raise JournalError(f"journal input is not a regular file or directory: {value}")
    if not paths:
        raise JournalError("no journal files found")
    return sorted(set(paths))


def _validate_privacy(value: Any, path: str = "record") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if key.lower() in FORBIDDEN_FIELDS:
                raise JournalError(f"forbidden private-content field at {path}.{key}")
            _validate_privacy(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _validate_privacy(child, f"{path}[{index}]")
    elif isinstance(value, str) and len(value.encode("utf-8")) > 255:
        raise JournalError(f"oversized string at {path}")


def _integer(value: Any, minimum: int, maximum: int) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and minimum <= value <= maximum


def _token(value: Any, *, empty: bool = False) -> bool:
    return isinstance(value, str) and (
        (empty and value == "") or TOKEN.fullmatch(value) is not None
    )


def _validate_schema(value: dict[str, Any], source: str) -> None:
    common = {
        "version", "event_id", "transaction_id", "sequence", "utc", "server_id",
        "run_id", "phase", "kind", "reason", "profile", "prev_hash", "record_hash",
    }
    phase = value["phase"]
    if not isinstance(phase, str) or phase not in {"boundary", "intent", "commit", "abort"}:
        raise JournalError(f"invalid record phase at {source}")
    if not isinstance(value["kind"], str):
        raise JournalError(f"invalid record kind at {source}")
    intent = {"account_id", "character_id", "context", "change"}
    expected = common | (intent if phase == "intent" else set())
    if set(value) != expected:
        raise JournalError(f"unexpected fields for {phase!r} record at {source}")
    if not all(_token(value[name]) for name in ("server_id", "run_id", "reason")):
        raise JournalError(f"invalid record identity or reason at {source}")
    if not _integer(value["sequence"], 1, (1 << 64) - 1):
        raise JournalError(f"invalid sequence at {source}")
    if value["event_id"] != f'{value["server_id"]}:{value["run_id"]}:{value["sequence"]}':
        raise JournalError(f"event ID does not match record identity at {source}")
    if not isinstance(value["utc"], str) or re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", value["utc"]
    ) is None:
        raise JournalError(f"invalid UTC timestamp at {source}")
    try:
        datetime.strptime(value["utc"], "%Y-%m-%dT%H:%M:%SZ")
    except (TypeError, ValueError):
        raise JournalError(f"invalid UTC timestamp at {source}")
    if not isinstance(value["prev_hash"], str) or (
        value["prev_hash"] != "" and HASH.fullmatch(value["prev_hash"]) is None
    ):
        raise JournalError(f"invalid previous hash at {source}")
    profile = value["profile"]
    if not isinstance(profile, dict) or set(profile) != {
        "id", "schema", "digest", "effective_axes"
    }:
        raise JournalError(f"invalid profile shape at {source}")
    profile_tokens = all(
        _token(profile[name]) for name in ("id", "digest", "effective_axes")
    )
    if not profile_tokens or not _integer(profile["schema"], 0, (1 << 32) - 1):
        raise JournalError(f"invalid profile identity at {source}")

    transaction = value["transaction_id"]
    if phase == "boundary":
        if transaction != "" or value["kind"] not in {"run", "profile"}:
            raise JournalError(f"invalid boundary record at {source}")
        return
    if phase in {"commit", "abort"}:
        if not _token(transaction) or value["kind"] != "transaction":
            raise JournalError(f"invalid terminal record at {source}")
        return
    if phase != "intent" or value["kind"] not in KINDS or not _token(transaction):
        raise JournalError(f"invalid transaction record at {source}")
    if not all(
        isinstance(value[name], str) and IDENTITY.fullmatch(value[name]) is not None
        for name in ("account_id", "character_id")
    ):
        raise JournalError(f"invalid player identity at {source}")
    context = value["context"]
    if not isinstance(context, dict) or set(context) != {"map_id", "x", "y"} or not _token(
        context["map_id"], empty=True
    ) or not _integer(context["x"], -(1 << 31), (1 << 31) - 1) or not _integer(
        context["y"], -(1 << 31), (1 << 31) - 1
    ):
        raise JournalError(f"invalid location context at {source}")
    change = value["change"]
    if not isinstance(change, dict) or set(change) != {
        "subject_id", "lineage_id", "before", "delta", "after"
    } or not _token(change["subject_id"]) or not _token(change["lineage_id"], empty=True):
        raise JournalError(f"invalid typed change at {source}")
    values_in_range = all(
        _integer(change[name], -(1 << 63), (1 << 63) - 1)
        for name in ("before", "delta", "after")
    )
    if not values_in_range:
        raise JournalError(f"typed change is outside int64 bounds at {source}")
    if change["before"] + change["delta"] != change["after"]:
        raise JournalError(f"typed change arithmetic mismatch at {source}")


def _record(raw: bytes, source: str) -> dict[str, Any]:
    if not raw.endswith(b"\n"):
        raise JournalError(f"internal torn-record handling error at {source}")
    body = raw[:-1]
    try:
        prefix, suffix = body.rsplit(HASH_MARKER, 1)
    except ValueError as error:
        raise JournalError(f"missing record hash at {source}") from error
    if len(suffix) != 66 or not suffix.endswith(b'"}'):
        raise JournalError(f"malformed record hash at {source}")
    try:
        claimed = suffix[:-2].decode("ascii", "strict")
    except UnicodeDecodeError as error:
        raise JournalError(f"malformed record hash at {source}") from error
    actual = hashlib.sha256(prefix).hexdigest()
    if claimed != actual:
        raise JournalError(f"record hash mismatch at {source}")
    try:
        value = json.loads(body, object_pairs_hook=_unique_object)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        raise JournalError(f"malformed JSON at {source}: {error}") from error
    required = {
        "version",
        "event_id",
        "transaction_id",
        "sequence",
        "utc",
        "server_id",
        "run_id",
        "phase",
        "kind",
        "reason",
        "profile",
        "prev_hash",
        "record_hash",
    }
    if not isinstance(value, dict) or not required.issubset(value):
        raise JournalError(f"missing required fields at {source}")
    if value["version"] != SCHEMA_VERSION or value["record_hash"] != claimed:
        raise JournalError(f"unsupported schema or inconsistent hash at {source}")
    if HASH.fullmatch(claimed) is None:
        raise JournalError(f"invalid record hash at {source}")
    _validate_privacy(value)
    _validate_schema(value, source)
    value["_source"] = source
    return value


@dataclass
class Journal:
    records: list[dict[str, Any]]
    transactions: dict[str, dict[str, Any]]
    torn_tails: list[str]


def load(inputs: Iterable[Path]) -> Journal:
    records: list[dict[str, Any]] = []
    torn_tails: list[str] = []
    chains: dict[str, tuple[str, int]] = {}
    event_ids: set[str] = set()
    paths = _paths(inputs)
    file_coordinates: dict[Path, tuple[str, int]] = {}
    final_indexes: dict[str, int] = {}
    for path in paths:
        match = JOURNAL_FILE.fullmatch(path.name)
        if match is None:
            raise JournalError(f"invalid journal filename: {path}")
        run_id, raw_index = match.groups()
        index = int(raw_index)
        file_coordinates[path] = (run_id, index)
        final_indexes[run_id] = max(final_indexes.get(run_id, -1), index)

    for path in sorted(paths, key=lambda value: file_coordinates[value]):
        filename_run, file_index = file_coordinates[path]
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            raise JournalError(f"journal path is not a direct regular file: {path}")
        if os.name != "nt" and stat.S_IMODE(metadata.st_mode) != 0o600:
            raise JournalError(f"insecure journal file permissions: {path}")
        if metadata.st_size > FILE_LIMIT:
            raise JournalError(f"journal file exceeds the rotation limit: {path}")
        data = path.read_bytes()
        lines = data.splitlines(keepends=True)
        for index, raw in enumerate(lines, 1):
            source = f"{path}:{index}"
            if not raw.endswith(b"\n"):
                if index == len(lines) and file_index == final_indexes[filename_run]:
                    torn_tails.append(source)
                    continue
                raise JournalError(f"torn non-final record at {source}")
            record = _record(raw, source)
            run_id = record["run_id"]
            if run_id != filename_run:
                raise JournalError(f"filename/run identity mismatch at {source}")
            if run_id not in chains and file_index != 0:
                previous_hash = record["prev_hash"]
                sequence = record["sequence"] - 1
            else:
                previous_hash, sequence = chains.get(run_id, ("", 0))
            if record["prev_hash"] != previous_hash or record["sequence"] != sequence + 1:
                raise JournalError(f"broken hash/sequence chain at {source}")
            if record["event_id"] in event_ids:
                raise JournalError(f"duplicate event ID at {source}")
            event_ids.add(record["event_id"])
            chains[run_id] = (record["record_hash"], record["sequence"])
            records.append(record)

    transactions: dict[str, dict[str, Any]] = {}
    for record in records:
        transaction_id = record["transaction_id"]
        if not transaction_id:
            continue
        state = transactions.setdefault(
            transaction_id,
            {"status": "missing-intent", "intent": None, "events": [], "terminal": None},
        )
        state["events"].append(record)
        phase = record["phase"]
        if phase == "intent":
            variable = {
                "_source", "event_id", "sequence", "utc", "server_id", "run_id",
                "prev_hash", "record_hash",
            }
            comparable = {
                key: value for key, value in record.items() if key not in variable
            }
            if state["intent"] is not None:
                existing = {
                    key: value
                    for key, value in state["intent"].items()
                    if key not in variable
                }
                if comparable != existing:
                    raise JournalError(f"conflicting duplicate intent for {transaction_id}")
            else:
                state["intent"] = record
        else:
            variable = {
                "_source", "event_id", "sequence", "utc", "server_id", "run_id",
                "prev_hash", "record_hash",
            }
            comparable = {
                key: value for key, value in record.items() if key not in variable
            }
            terminal = state["terminal"]
            if terminal is not None and terminal["phase"] != phase:
                raise JournalError(f"commit/abort conflict for {transaction_id}")
            if terminal is not None:
                existing = {
                    key: value for key, value in terminal.items() if key not in variable
                }
                if comparable != existing:
                    raise JournalError(f"conflicting duplicate terminal for {transaction_id}")
            else:
                state["terminal"] = record
    for transaction_id, state in transactions.items():
        if state["intent"] is None:
            raise JournalError(f"terminal record without intent for {transaction_id}")
        terminal = state.pop("terminal")
        if terminal is not None and terminal["phase"] == "commit":
            state["status"] = "committed"
        elif terminal is not None and terminal["phase"] == "abort":
            state["status"] = "aborted"
        else:
            state["status"] = "attempted"
    return Journal(records, transactions, torn_tails)


def query(journal: Journal, **filters: str | None) -> list[dict[str, Any]]:
    matching_transactions: set[str] = set()
    for record in journal.records:
        if record["phase"] != "intent":
            continue
        change = record.get("change", {})
        matches = (
            (not filters.get("account") or record.get("account_id") == filters["account"])
            and (
                not filters.get("character")
                or record.get("character_id") == filters["character"]
            )
            and (not filters.get("subject") or change.get("subject_id") == filters["subject"])
            and (not filters.get("lineage") or change.get("lineage_id") == filters["lineage"])
        )
        if matches:
            matching_transactions.add(record["transaction_id"])
    if filters.get("transaction"):
        matching_transactions &= {filters["transaction"]}
    selected = [
        {key: value for key, value in record.items() if key != "_source"}
        for record in journal.records
        if record["transaction_id"] in matching_transactions
    ]
    run_order = {
        coordinate: index
        for index, coordinate in enumerate(sorted(
            {(record["server_id"], record["run_id"]) for record in journal.records},
            key=lambda coordinate: (
                min(record["utc"] for record in journal.records
                    if (record["server_id"], record["run_id"]) == coordinate),
                coordinate,
            ),
        ))
    }
    transaction_order = {}
    for transaction_id in matching_transactions:
        intent = next((record for record in selected
                       if record["transaction_id"] == transaction_id
                       and record["phase"] == "intent"), None)
        if intent is not None:
            transaction_order[transaction_id] = (
                run_order[(intent["server_id"], intent["run_id"])], intent["sequence"]
            )
    phase_order = {"intent": 0, "commit": 1, "abort": 1}
    return sorted(selected, key=lambda value: (
        transaction_order[value["transaction_id"]], phase_order[value["phase"]],
        run_order[(value["server_id"], value["run_id"])], value["sequence"],
    ))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("validate")
    query_parser = sub.add_parser("query")
    for name in ("account", "character", "subject", "lineage", "transaction"):
        query_parser.add_argument(f"--{name}")
    sub.add_parser("reconcile")
    return parser


def main(argv: list[str] | None = None) -> int:
    options = _parser().parse_args(argv)
    try:
        journal = load(options.paths)
    except (JournalError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    if options.command == "validate":
        result = {
            "valid": True,
            "records": len(journal.records),
            "torn_tails": journal.torn_tails,
        }
        print(json.dumps(result, sort_keys=True))
    elif options.command == "query":
        names = ("account", "character", "subject", "lineage", "transaction")
        filters = {name: getattr(options, name) for name in names}
        print(json.dumps(query(journal, **filters), sort_keys=True))
    else:
        result = {
            key: {"status": value["status"], "events": len(value["events"])}
            for key, value in sorted(journal.transactions.items())
        }
        print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
