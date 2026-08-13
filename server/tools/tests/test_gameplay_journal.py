from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "gameplay_journal.py"
SPEC = importlib.util.spec_from_file_location("gameplay_journal", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
gameplay_journal = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gameplay_journal
SPEC.loader.exec_module(gameplay_journal)


class JournalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.root.chmod(0o700)
        self.path = self.root / "journal-run-0000.jsonl"
        self.previous = ""
        self.sequence = 0

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def record(self, record_phase: str, transaction: str = "", **extra: object) -> bytes:
        self.sequence += 1
        value: dict[str, object] = {
            "version": 1,
            "event_id": f"server:run:{self.sequence}",
            "transaction_id": transaction,
            "sequence": self.sequence,
            "utc": f"2026-08-13T00:00:{self.sequence:02d}Z",
            "server_id": "server",
            "run_id": "run",
            "phase": record_phase,
            "kind": (
                "transaction"
                if record_phase in {"commit", "abort"}
                else "currency" if transaction else "run"
            ),
            "reason": "test.reason",
            "profile": {
                "id": "legacy-unknown",
                "schema": 0,
                "digest": "unknown",
                "effective_axes": "unknown",
            },
            "prev_hash": self.previous,
        }
        if record_phase == "intent":
            value.update(
                account_id="acct",
                character_id="hero",
                context={"map_id": "/world/start", "x": 1, "y": 2},
                change={
                    "subject_id": "currency:gold",
                    "lineage_id": "",
                    "before": 1,
                    "delta": 2,
                    "after": 3,
                },
            )
        value.update(extra)
        value["event_id"] = f'{value["server_id"]}:{value["run_id"]}:{value["sequence"]}'
        prefix = json.dumps(value, separators=(",", ":")).encode()[:-1]
        self.previous = hashlib.sha256(prefix).hexdigest()
        return prefix + f',"record_hash":"{self.previous}"}}\n'.encode()

    def write(self, *records: bytes) -> None:
        self.path.write_bytes(b"".join(records))
        self.path.chmod(0o600)

    def test_reconstructs_committed_timeline_and_queries(self) -> None:
        boundary = self.record("boundary")
        intent = self.record(
            "intent",
            "tx1",
            account_id="acct",
            character_id="hero",
            change={
                "subject_id": "gold",
                "lineage_id": "",
                "before": 1,
                "delta": 2,
                "after": 3,
            },
        )
        commit = self.record("commit", "tx1")
        self.write(boundary, intent, commit)
        journal = gameplay_journal.load([self.root])
        self.assertEqual(journal.transactions["tx1"]["status"], "committed")
        self.assertEqual(len(gameplay_journal.query(journal, account="acct")), 1)

    def test_intent_only_is_ambiguous_and_torn_tail_is_ignored(self) -> None:
        self.write(self.record("intent", "tx1"), b'{"torn"')
        journal = gameplay_journal.load([self.root])
        self.assertEqual(journal.transactions["tx1"]["status"], "attempted")
        self.assertEqual(len(journal.torn_tails), 1)

    def test_duplicate_commit_is_idempotent(self) -> None:
        self.write(
            self.record("intent", "tx1"),
            self.record("commit", "tx1"),
            self.record("commit", "tx1"),
        )
        journal = gameplay_journal.load([self.root])
        self.assertEqual(journal.transactions["tx1"]["status"], "committed")

    def test_duplicate_intent_across_restart_is_idempotent(self) -> None:
        self.write(self.record("intent", "tx1"))
        self.sequence = 0
        self.previous = ""
        other = self.root / "journal-run2-0000.jsonl"
        other.write_bytes(self.record("intent", "tx1", run_id="run2"))
        other.chmod(0o600)
        journal = gameplay_journal.load([self.root])
        self.assertEqual(journal.transactions["tx1"]["status"], "attempted")
        self.assertEqual(len(journal.transactions["tx1"]["events"]), 2)

    def test_conflicting_duplicate_intent_is_rejected(self) -> None:
        self.write(
            self.record("intent", "tx1"),
            self.record("intent", "tx1", reason="different.reason"),
        )
        with self.assertRaisesRegex(gameplay_journal.JournalError, "conflicting duplicate"):
            gameplay_journal.load([self.root])

    def test_rotated_files_continue_one_chain(self) -> None:
        first = self.record("intent", "tx1")
        second = self.record("abort", "tx1")
        self.write(first)
        other = self.root / "journal-run-0001.jsonl"
        other.write_bytes(second)
        other.chmod(0o600)
        journal = gameplay_journal.load([self.root])
        self.assertEqual(journal.transactions["tx1"]["status"], "aborted")

    def test_restart_resets_sequence_in_a_new_run(self) -> None:
        first = self.record("boundary")
        self.write(first)
        self.sequence = 0
        self.previous = ""
        other = self.root / "journal-run2-0000.jsonl"
        other.write_bytes(self.record("boundary", run_id="run2"))
        other.chmod(0o600)
        journal = gameplay_journal.load([self.root])
        self.assertEqual([record["sequence"] for record in journal.records], [1, 1])

    def test_crash_phase_reconciliation_is_conservative(self) -> None:
        boundary = self.record("boundary")
        intent = self.record("intent", "attempted")
        committed_intent = self.record("intent", "committed")
        commit = self.record("commit", "committed")
        self.write(boundary, intent, committed_intent, commit)
        journal = gameplay_journal.load([self.root])
        self.assertNotIn("before-intent", journal.transactions)
        self.assertEqual(journal.transactions["attempted"]["status"], "attempted")
        self.assertEqual(journal.transactions["committed"]["status"], "committed")

    def test_rejects_corruption_permissions_and_redaction(self) -> None:
        record = bytearray(self.record("boundary"))
        record[10] ^= 1
        self.write(bytes(record))
        with self.assertRaisesRegex(gameplay_journal.JournalError, "hash mismatch"):
            gameplay_journal.load([self.root])
        self.sequence = 0
        self.previous = ""
        self.write(self.record("boundary", password="secret"))
        with self.assertRaisesRegex(gameplay_journal.JournalError, "forbidden"):
            gameplay_journal.load([self.root])
        self.path.chmod(0o644)
        with self.assertRaisesRegex(gameplay_journal.JournalError, "permissions"):
            gameplay_journal.load([self.root])

    def test_rejects_malformed_phases_and_typed_changes(self) -> None:
        self.write(self.record("commit", "tx1"))
        with self.assertRaisesRegex(gameplay_journal.JournalError, "without intent"):
            gameplay_journal.load([self.root])

        self.sequence = 0
        self.previous = ""
        self.write(
            self.record(
                "intent",
                "tx1",
                change={
                    "subject_id": "currency:gold",
                    "lineage_id": "",
                    "before": 1,
                    "delta": 2,
                    "after": 4,
                },
            )
        )
        with self.assertRaisesRegex(gameplay_journal.JournalError, "arithmetic mismatch"):
            gameplay_journal.load([self.root])

        self.sequence = 0
        self.previous = ""
        self.write(self.record("boundary", phase=["intent"]))
        with self.assertRaisesRegex(gameplay_journal.JournalError, "invalid record phase"):
            gameplay_journal.load([self.root])

    def test_rejects_terminal_conflicts_and_symlink_inputs(self) -> None:
        self.write(
            self.record("intent", "tx1"),
            self.record("commit", "tx1"),
            self.record("abort", "tx1"),
        )
        with self.assertRaisesRegex(gameplay_journal.JournalError, "commit/abort conflict"):
            gameplay_journal.load([self.root])

        if os.name != "nt":
            link = self.root.parent / f"{self.root.name}-link.jsonl"
            link.symlink_to(self.path)
            self.addCleanup(link.unlink)
            with self.assertRaisesRegex(gameplay_journal.JournalError, "symbolic links"):
                gameplay_journal.load([link])


if __name__ == "__main__":
    unittest.main()
