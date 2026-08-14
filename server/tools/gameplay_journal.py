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


SCHEMA_VERSIONS = {1, 2}
HASH_MARKER = b',"record_hash":"'
FORBIDDEN_FIELDS = {"password", "session_secret", "chat", "inscription", "text"}
TOKEN = re.compile(r"[A-Za-z0-9_.:/@+\-]{1,255}\Z")
MAP_ID = re.compile(r"[A-Za-z0-9_.:/@+$\-]{1,255}\Z")
IDENTITY = re.compile(r"[ -~]{1,255}\Z")
HASH = re.compile(r"[0-9a-f]{64}\Z")
KINDS = {"item", "currency", "quest", "progression"}
FILE_LIMIT = 9 * 1024 * 1024
JOURNAL_FILE = re.compile(r"journal-([0-9a-f]{32})-(\d{4,})\.jsonl\Z")
ITEM_SNAPSHOT = re.compile(
    r"arch=([^;]+);type=([0-9]+);nrof=([0-9]+);value=(-?[0-9]+);weight=([0-9]+)\Z"
)
ITEM_PROVENANCE = re.compile(r"first=([^;]{0,112});last=([^;]{0,112})\Z")


class JournalError(ValueError):
    """The journal does not satisfy its integrity or privacy contract."""


def _is_reparse(metadata: os.stat_result | Any) -> bool:
    return bool(
        getattr(metadata, "st_file_attributes", 0)
        & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    )


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
        if stat.S_ISLNK(metadata.st_mode) or _is_reparse(metadata):
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
    if not isinstance(phase, str) or phase not in {
        "boundary", "intent", "domain", "commit", "abort"
    }:
        raise JournalError(f"invalid record phase at {source}")
    if not isinstance(value["kind"], str):
        raise JournalError(f"invalid record kind at {source}")
    intent = {"account_id", "character_id", "context", "change"}
    terminal = {"domains"} if phase in {"commit", "abort"} and value["version"] == 2 else set()
    domain = {"domain"} if phase == "domain" else set()
    expected = common | (intent if phase == "intent" else terminal | domain)
    if phase == "intent" and value["version"] == 2:
        expected.update({"details", "domains"})
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
    if phase == "domain":
        domain = value["domain"]
        if (
            value["version"] != 2
            or not _token(transaction)
            or value["kind"] != "transaction"
            or not isinstance(domain, dict)
            or set(domain) != {"kind", "id"}
            or domain["kind"] not in {"player", "map-runtime", "map-unique"}
            or not isinstance(domain["id"], str)
            or IDENTITY.fullmatch(domain["id"]) is None
        ):
            raise JournalError(f"invalid save-domain record at {source}")
        return
    if phase in {"commit", "abort"}:
        if not _token(transaction) or value["kind"] != "transaction":
            raise JournalError(f"invalid terminal record at {source}")
        if value["version"] == 2:
            domains = value["domains"]
            if not isinstance(domains, list):
                raise JournalError(f"invalid terminal domains at {source}")
            seen: set[tuple[str, str]] = set()
            for domain in domains:
                if (
                    not isinstance(domain, dict)
                    or set(domain) != {"kind", "id"}
                    or domain["kind"] not in {"player", "map-runtime", "map-unique"}
                    or not isinstance(domain["id"], str)
                    or IDENTITY.fullmatch(domain["id"]) is None
                    or (domain["kind"], domain["id"]) in seen
                ):
                    raise JournalError(f"invalid terminal domains at {source}")
                seen.add((domain["kind"], domain["id"]))
            if phase == "commit" and not domains:
                raise JournalError(f"committed transaction has no save domains at {source}")
        return
    if phase != "intent" or value["kind"] not in KINDS or not _token(transaction):
        raise JournalError(f"invalid transaction record at {source}")
    if not all(
        isinstance(value[name], str) and IDENTITY.fullmatch(value[name]) is not None
        for name in ("account_id", "character_id")
    ):
        raise JournalError(f"invalid player identity at {source}")
    context = value["context"]
    if not isinstance(context, dict) or set(context) != {"map_id", "x", "y"} or not (
        isinstance(context["map_id"], str)
        and (context["map_id"] == "" or MAP_ID.fullmatch(context["map_id"]) is not None)
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
    if value["version"] == 2:
        domains = value["domains"]
        if (
            not isinstance(domains, list)
            or len(domains) != 1
            or not isinstance(domains[0], dict)
            or set(domains[0]) != {"kind", "id"}
            or domains[0]["kind"] != "player"
            or not isinstance(domains[0]["id"], str)
            or IDENTITY.fullmatch(domains[0]["id"]) is None
        ):
            raise JournalError(f"invalid intent save domains at {source}")
        details = value.get("details")
        expected_details = {
            "archetype", "object_type", "snapshot", "quantity", "source",
            "destination", "actor", "counterparty", "provenance_before",
            "provenance_after", "price", "currency", "funding",
        }
        if not isinstance(details, dict) or set(details) != expected_details:
            raise JournalError(f"invalid semantic details at {source}")
        token_fields = ("archetype", "source", "destination", "currency", "funding")
        identity_fields = (
            "snapshot", "actor", "counterparty", "provenance_before",
            "provenance_after",
        )
        if not all(_token(details[name], empty=True) for name in token_fields) or not all(
            isinstance(details[name], str)
            and (details[name] == "" or IDENTITY.fullmatch(details[name]) is not None)
            for name in identity_fields
        ) or not _integer(details["object_type"], -(1 << 31), (1 << 31) - 1) or not _integer(
            details["quantity"], 0, (1 << 32) - 1
        ) or not _integer(details["price"], 0, (1 << 63) - 1
        ):
            raise JournalError(f"invalid semantic detail value at {source}")
        if value["kind"] == "item" and (
            not change["lineage_id"]
            or not details["archetype"]
            or not details["snapshot"]
            or details["quantity"] == 0
            or not details["source"]
            or not details["destination"]
            or not details["actor"]
            or not details["provenance_before"]
            or not details["provenance_after"]
        ):
            raise JournalError(f"item semantic details are incomplete at {source}")
        if value["kind"] == "item":
            snapshot = ITEM_SNAPSHOT.fullmatch(details["snapshot"])
            provenance = (
                ITEM_PROVENANCE.fullmatch(details["provenance_before"]),
                ITEM_PROVENANCE.fullmatch(details["provenance_after"]),
            )
            if (
                snapshot is None
                or snapshot.group(1) != details["archetype"]
                or details["object_type"] < 0
                or not all(
                    _integer(int(snapshot.group(index)), 0, (1 << 32) - 1)
                    for index in (2, 3, 5)
                )
                or not _integer(int(snapshot.group(4)), -(1 << 63), (1 << 63) - 1)
                or int(snapshot.group(2)) != details["object_type"]
                or int(snapshot.group(3)) == 0
                or details["snapshot"]
                != (
                    f"arch={snapshot.group(1)};type={int(snapshot.group(2))};"
                    f"nrof={int(snapshot.group(3))};value={int(snapshot.group(4))};"
                    f"weight={int(snapshot.group(5))}"
                )
            ):
                raise JournalError(f"invalid item snapshot at {source}")
            if any(
                item is None
                or any(
                    identity != "" and IDENTITY.fullmatch(identity) is None
                    for identity in item.groups()
                )
                for item in provenance
            ):
                raise JournalError(f"invalid item provenance at {source}")
        if value["kind"] == "currency" and (
            not details["source"]
            or not details["destination"]
            or not details["actor"]
            or not details["currency"]
            or not details["funding"]
        ):
            raise JournalError(f"currency semantic details are incomplete at {source}")


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
    if value["version"] not in SCHEMA_VERSIONS or value["record_hash"] != claimed:
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
        if stat.S_ISLNK(metadata.st_mode) or _is_reparse(metadata) or not stat.S_ISREG(metadata.st_mode):
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
            if run_id not in chains:
                previous_hash = record["prev_hash"] if file_index != 0 else ""
                sequence = record["sequence"] - 1
            else:
                previous_hash, sequence = chains[run_id]
            if record["prev_hash"] != previous_hash or record["sequence"] != sequence + 1:
                raise JournalError(f"broken hash/sequence chain at {source}")
            if record["event_id"] in event_ids:
                raise JournalError(f"duplicate event ID at {source}")
            event_ids.add(record["event_id"])
            chains[run_id] = (record["record_hash"], record["sequence"])
            records.append(record)

    records.sort(key=lambda record: record["sequence"])
    version_two_sequences: set[int] = set()
    for record in records:
        if record["version"] == 2:
            if record["sequence"] in version_two_sequences:
                raise JournalError("duplicate global sequence in version-2 records")
            version_two_sequences.add(record["sequence"])
    transactions: dict[str, dict[str, Any]] = {}
    for record in records:
        transaction_id = record["transaction_id"]
        if not transaction_id:
            continue
        state = transactions.setdefault(
            transaction_id,
            {
                "status": "missing-intent",
                "intent": None,
                "events": [],
                "terminal": None,
                "domains": [],
            },
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
                state["domains"] = list(record.get("domains", []))
        elif phase == "domain":
            if state["intent"] is None:
                raise JournalError(f"save domain without intent for {transaction_id}")
            domain = record["domain"]
            if domain in state["domains"]:
                raise JournalError(f"duplicate save domain for {transaction_id}")
            state["domains"].append(domain)
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
        if terminal is not None and {
            (domain["kind"], domain["id"]) for domain in terminal.get("domains", [])
        } != {(domain["kind"], domain["id"]) for domain in state["domains"]}:
            raise JournalError(f"terminal save domains differ from intent plan for {transaction_id}")
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
    phase_order = {"intent": 0, "domain": 1, "commit": 2, "abort": 2}
    return sorted(selected, key=lambda value: (
        value["sequence"], phase_order[value["phase"]],
    ))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("validate")
    query_parser = sub.add_parser("query")
    for name in ("account", "character", "subject", "lineage", "transaction"):
        query_parser.add_argument(f"--{name}")
    reconcile_parser = sub.add_parser("reconcile")
    reconcile_parser.add_argument(
        "--domain",
        action="append",
        default=[],
        metavar="KIND:ID=SEQUENCE",
        help="saved player/map domain watermark; repeat for every affected domain",
    )
    reconcile_parser.add_argument(
        "--player-save",
        action="append",
        default=[],
        metavar="ACCOUNT/CHARACTER=PATH",
        help="saved player file and its journal domain identity",
    )
    reconcile_parser.add_argument(
        "--map-save",
        action="append",
        default=[],
        metavar="MAP_ID=PATH",
        help="saved map/header or unique-object component; repeated components use the oldest sequence",
    )
    return parser


def _checkpoint_sequence(path: Path) -> tuple[int, bool]:
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode) or _is_reparse(metadata) or not stat.S_ISREG(metadata.st_mode):
        raise JournalError(f"save domain is not a direct regular file: {path}")
    sequence: int | None = None
    run_id: bytes | None = None
    unique_component = False
    lines = path.read_bytes().splitlines()
    if lines and lines[0].startswith(b"# gameplay-journal "):
        fields = lines[0].split()
        if len(fields) != 4 or re.fullmatch(b"[0-9a-f]{32}", fields[2]) is None:
            raise JournalError(f"invalid unique save-domain marker: {path}")
        try:
            sequence = int(fields[3])
        except ValueError as error:
            raise JournalError(f"invalid save-domain sequence: {path}") from error
        unique_component = True
        run_id = fields[2]
    else:
        for raw_line in lines:
            if raw_line in {b"end", b"endplst"}:
                break
            if raw_line.startswith(b"journal_run "):
                if run_id is not None:
                    raise JournalError(f"duplicate save-domain run identity: {path}")
                run_id = raw_line.removeprefix(b"journal_run ")
                if re.fullmatch(b"[0-9a-f]{32}", run_id) is None:
                    raise JournalError(f"invalid save-domain run identity: {path}")
                continue
            if not raw_line.startswith(b"journal_sequence "):
                continue
            if sequence is not None:
                raise JournalError(f"duplicate save-domain sequence: {path}")
            try:
                sequence = int(raw_line.removeprefix(b"journal_sequence "))
            except ValueError as error:
                raise JournalError(f"invalid save-domain sequence: {path}") from error
    if sequence is None:
        sequence = 0
    elif run_id is None:
        raise JournalError(f"save domain sequence has no run identity: {path}")
    if not _integer(sequence, 0, (1 << 64) - 1):
        raise JournalError(f"save domain has no valid journal sequence: {path}")
    return sequence, unique_component


def _saved_domain_arguments(options: argparse.Namespace) -> list[str]:
    domains = list(options.domain)
    for value in options.player_save:
        identity, separator, raw_path = value.partition("=")
        if not separator or not identity or not raw_path or IDENTITY.fullmatch(identity) is None:
            raise JournalError(f"invalid player save argument: {value}")
        sequence, _unique_component = _checkpoint_sequence(Path(raw_path))
        domains.append(f"player:{identity}={sequence}")
    for value in options.map_save:
        identity, separator, raw_path = value.partition("=")
        if not separator or not identity or not raw_path:
            raise JournalError(f"invalid map save argument: {value}")
        sequence, unique_component = _checkpoint_sequence(Path(raw_path))
        kind = "map-unique" if unique_component else "map-runtime"
        domains.append(f"{kind}:{identity}={sequence}")
    return domains


def reconcile(journal: Journal, raw_domains: list[str]) -> dict[str, Any]:
    watermarks: dict[tuple[str, str], int] = {}
    for raw in raw_domains:
        coordinate, separator, raw_sequence = raw.rpartition("=")
        kind, colon, identity = coordinate.partition(":")
        if (
            not separator
            or not colon
            or kind not in {"player", "map-runtime", "map-unique"}
            or not identity
        ):
            raise JournalError(f"invalid reconciliation domain: {raw}")
        try:
            sequence = int(raw_sequence)
        except ValueError as error:
            raise JournalError(f"invalid reconciliation domain: {raw}") from error
        if not _integer(sequence, 0, (1 << 64) - 1):
            raise JournalError(f"invalid reconciliation domain: {raw}")
        coordinate_key = (kind, identity)
        watermarks[coordinate_key] = min(watermarks.get(coordinate_key, sequence), sequence)

    result: dict[str, Any] = {}
    ordered = sorted(
        journal.transactions.items(),
        key=lambda item: item[1]["intent"]["sequence"],
    )
    for transaction_id, transaction in ordered:
        status = transaction["status"]
        terminal = next(
            (
                event
                for event in reversed(transaction["events"])
                if event["phase"] in {"commit", "abort"}
            ),
            None,
        )
        entry: dict[str, Any] = {
            "status": status,
            "intent_sequence": transaction["intent"]["sequence"],
            "terminal_sequence": terminal["sequence"] if terminal is not None else None,
            "domains": [],
        }
        if transaction["intent"] is not None:
            entry["change"] = transaction["intent"]["change"]
            if "details" in transaction["intent"]:
                entry["details"] = transaction["intent"]["details"]
        if status == "committed" and terminal is not None:
            terminal_sequence = terminal["sequence"]
            for domain in terminal.get("domains", []):
                coordinate = (domain["kind"], domain["id"])
                saved = watermarks.get(coordinate)
                action = (
                    "unknown"
                    if saved is None
                    else "checkpointed"
                    if saved >= terminal_sequence
                    else "replay-required"
                )
                entry["domains"].append(
                    {
                        **domain,
                        "saved_sequence": saved,
                        "terminal_sequence": terminal_sequence,
                        "action": action,
                    }
                )
        elif status == "attempted":
            entry["domains"] = [
                {**domain, "saved_sequence": watermarks.get((domain["kind"], domain["id"]))}
                for domain in transaction["domains"]
            ]
            entry["action"] = "inspect-typed-state"
        else:
            entry["action"] = "none"
        result[transaction_id] = entry
    return result


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
        try:
            result = reconcile(journal, _saved_domain_arguments(options))
        except JournalError as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
        print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
