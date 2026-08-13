from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "gameplay_journal.py"
SPEC = importlib.util.spec_from_file_location("gameplay_journal", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
gameplay_journal = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gameplay_journal
SPEC.loader.exec_module(gameplay_journal)
RUN = "a" * 32
RUN2 = "b" * 32


class JournalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.root.chmod(0o700)
        self.path = self.root / f"journal-{RUN}-0000.jsonl"
        self.previous = ""
        self.sequence = 0

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_windows_reparse_metadata_is_rejected(self) -> None:
        metadata = SimpleNamespace(st_file_attributes=0x400)
        self.assertTrue(gameplay_journal._is_reparse(metadata))

    def record(self, record_phase: str, transaction: str = "", **extra: object) -> bytes:
        self.sequence += 1
        value: dict[str, object] = {
            "version": 1,
            "event_id": f"server:run:{self.sequence}",
            "transaction_id": transaction,
            "sequence": self.sequence,
            "utc": f"2026-08-13T00:00:{self.sequence:02d}Z",
            "server_id": "server",
            "run_id": RUN,
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
        self.assertEqual(len(gameplay_journal.query(journal, account="acct")), 2)

    def test_entity_queries_return_complete_status_timelines(self) -> None:
        records = [
            self.record(
                "intent",
                "committed",
                character_id="Hero One",
                change={
                    "subject_id": "item:ring",
                    "lineage_id": "lineage:1",
                    "before": 0,
                    "delta": 1,
                    "after": 1,
                },
            ),
            self.record("commit", "committed"),
            self.record(
                "intent",
                "aborted",
                character_id="Hero One",
                change={
                    "subject_id": "item:ring",
                    "lineage_id": "lineage:2",
                    "before": 1,
                    "delta": -1,
                    "after": 0,
                },
            ),
            self.record("abort", "aborted"),
            self.record(
                "intent",
                "attempted",
                character_id="Hero One",
                change={
                    "subject_id": "item:ring",
                    "lineage_id": "lineage:3",
                    "before": 1,
                    "delta": -1,
                    "after": 0,
                },
            ),
        ]
        self.write(*records)
        journal = gameplay_journal.load([self.root])
        for filters in (
            {"account": "acct"},
            {"character": "Hero One"},
            {"subject": "item:ring"},
        ):
            selected = gameplay_journal.query(journal, **filters)
            self.assertEqual([record["phase"] for record in selected], [
                "intent", "commit", "intent", "abort", "intent"
            ])
        lineage = gameplay_journal.query(journal, lineage="lineage:2")
        self.assertEqual([record["phase"] for record in lineage], ["intent", "abort"])

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

    def test_conflicting_duplicate_terminal_is_rejected(self) -> None:
        self.write(
            self.record("intent", "tx1"),
            self.record("abort", "tx1", reason="first.reason"),
            self.record("abort", "tx1", reason="different.reason"),
        )
        with self.assertRaisesRegex(gameplay_journal.JournalError, "conflicting duplicate terminal"):
            gameplay_journal.load([self.root])

    def test_duplicate_intent_across_restart_is_idempotent(self) -> None:
        self.write(self.record("intent", "tx1"))
        self.sequence = 0
        self.previous = ""
        other = self.root / f"journal-{RUN2}-0000.jsonl"
        other.write_bytes(self.record("intent", "tx1", run_id=RUN2))
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
        other = self.root / f"journal-{RUN}-0001.jsonl"
        other.write_bytes(second)
        other.chmod(0o600)
        journal = gameplay_journal.load([self.root])
        self.assertEqual(journal.transactions["tx1"]["status"], "aborted")

    def test_restart_resets_sequence_in_a_new_run(self) -> None:
        first = self.record("boundary")
        self.write(first)
        self.sequence = 0
        self.previous = ""
        other = self.root / f"journal-{RUN2}-0000.jsonl"
        other.write_bytes(self.record("boundary", run_id=RUN2))
        other.chmod(0o600)
        journal = gameplay_journal.load([self.root])
        self.assertEqual([record["sequence"] for record in journal.records], [1, 1])

    def test_retained_mid_chain_segment_is_a_valid_anchor(self) -> None:
        for _ in range(3):
            self.record("boundary")
        retained = self.record("intent", "tx1") + self.record("commit", "tx1")
        self.path = self.root / f"journal-{RUN}-0016.jsonl"
        self.write(retained)
        journal = gameplay_journal.load([self.root])
        self.assertEqual(journal.transactions["tx1"]["status"], "committed")

    def test_torn_rotated_predecessor_is_rejected(self) -> None:
        self.write(self.record("boundary"), b'{"torn"')
        successor = self.root / f"journal-{RUN}-0001.jsonl"
        successor.write_bytes(self.record("boundary"))
        successor.chmod(0o600)
        with self.assertRaisesRegex(gameplay_journal.JournalError, "torn non-final"):
            gameplay_journal.load([self.root])

    def test_transaction_query_preserves_numeric_sequence_order(self) -> None:
        records = [self.record("boundary") for _ in range(8)]
        records.extend([self.record("intent", "tx1"), self.record("commit", "tx1")])
        self.write(*records)
        selected = gameplay_journal.query(
            gameplay_journal.load([self.root]), transaction="tx1"
        )
        self.assertEqual([record["sequence"] for record in selected], [9, 10])

    def test_transaction_query_uses_sequence_when_utc_moves_backward(self) -> None:
        self.write(
            self.record("intent", "tx1", utc="2026-08-13T00:00:10Z"),
            self.record("commit", "tx1", utc="2026-08-13T00:00:09Z"),
        )
        selected = gameplay_journal.query(
            gameplay_journal.load([self.root]), transaction="tx1"
        )
        self.assertEqual([record["phase"] for record in selected], ["intent", "commit"])

    def test_entity_query_preserves_same_second_transaction_order(self) -> None:
        records = [
            self.record("intent", "z-transaction", utc="2026-08-13T00:00:01Z"),
            self.record("commit", "z-transaction", utc="2026-08-13T00:00:01Z"),
            self.record("intent", "a-transaction", utc="2026-08-13T00:00:01Z"),
            self.record("commit", "a-transaction", utc="2026-08-13T00:00:01Z"),
        ]
        self.write(*records)
        selected = gameplay_journal.query(gameplay_journal.load([self.root]), account="acct")
        self.assertEqual(
            [record["transaction_id"] for record in selected],
            ["z-transaction", "z-transaction", "a-transaction", "a-transaction"],
        )

    def test_large_entity_query_indexes_transactions_once(self) -> None:
        records = []
        for index in range(250):
            transaction = f"tx-{index:04d}"
            records.extend([
                self.record("intent", transaction, utc="2026-08-13T00:00:01Z"),
                self.record("commit", transaction, utc="2026-08-13T00:00:01Z"),
            ])
        self.write(*records)
        selected = gameplay_journal.query(gameplay_journal.load([self.root]), account="acct")
        self.assertEqual(len(selected), 500)
        self.assertEqual(selected[0]["transaction_id"], "tx-0000")
        self.assertEqual(selected[-1]["transaction_id"], "tx-0249")

    def test_cross_run_terminal_follows_replayed_intent(self) -> None:
        self.write(self.record("intent", "tx1", utc="2026-08-13T00:00:10Z"))
        self.sequence = 0
        self.previous = ""
        other = self.root / f"journal-{RUN2}-0000.jsonl"
        other.write_bytes(self.record(
            "commit", "tx1", run_id=RUN2, utc="2026-08-13T00:00:09Z"
        ))
        other.chmod(0o600)
        selected = gameplay_journal.query(
            gameplay_journal.load([self.root]), transaction="tx1"
        )
        self.assertEqual([record["phase"] for record in selected], ["intent", "commit"])

    def test_character_identity_accepts_configured_name_characters(self) -> None:
        self.write(self.record(
            "intent", "tx1", account_id='acct"quoted', character_id=r"Hero_One\\Two"
        ))
        journal = gameplay_journal.load([self.root])
        intent = journal.transactions["tx1"]["intent"]
        self.assertEqual(intent["account_id"], 'acct"quoted')
        self.assertEqual(intent["character_id"], r"Hero_One\\Two")

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
