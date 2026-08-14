from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from types import SimpleNamespace
import unittest
from contextlib import redirect_stdout


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
                if record_phase in {"domain", "commit", "abort"}
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
        if record_phase == "intent" and value["version"] == 2:
            value.setdefault(
                "domains",
                [{"kind": "player", "id": f'{value["account_id"]}/{value["character_id"]}'}],
            )
        value["event_id"] = f'{value["server_id"]}:{value["run_id"]}:{value["sequence"]}'
        prefix = json.dumps(value, separators=(",", ":")).encode()[:-1]
        self.previous = hashlib.sha256(prefix).hexdigest()
        return prefix + f',"record_hash":"{self.previous}"}}\n'.encode()

    def write(self, *records: bytes) -> None:
        self.path.write_bytes(b"".join(records))
        self.path.chmod(0o600)

    @staticmethod
    def details(**updates: object) -> dict[str, object]:
        value: dict[str, object] = {
            "archetype": "", "object_type": 0, "snapshot": "", "quantity": 0,
            "source": "", "destination": "", "actor": "", "counterparty": "",
            "provenance_before": "", "provenance_after": "", "price": 0, "currency": "",
            "funding": "",
        }
        value.update(updates)
        return value

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

    def test_restart_continues_global_sequence_in_a_new_run(self) -> None:
        first = self.record("boundary")
        self.write(first)
        self.previous = ""
        other = self.root / f"journal-{RUN2}-0000.jsonl"
        other.write_bytes(self.record("boundary", run_id=RUN2))
        other.chmod(0o600)
        journal = gameplay_journal.load([self.root])
        self.assertEqual([record["sequence"] for record in journal.records], [1, 2])

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

    def test_version_two_transaction_families_reconcile_after_restart(self) -> None:
        records: list[bytes] = []
        families = (
            ("item.acquire", "item"),
            ("shop.purchase", "item"),
            ("bank.deposit", "currency"),
            ("script.item-grant", "item"),
        )
        for index, (reason, kind) in enumerate(families):
            attempted = f"attempted-{index}"
            committed = f"committed-{index}"
            details = self.details(
                archetype="sword" if kind == "item" else "",
                object_type=15 if kind == "item" else 0,
                snapshot="arch=sword;type=15;nrof=1;value=0;weight=0" if kind == "item" else "",
                quantity=1 if kind == "item" else 0,
                source="ground" if kind == "item" else "carried-cash",
                destination="player" if kind == "item" else "bank",
                actor="acct:actor",
                provenance_before="first=;last=" if kind == "item" else "",
                provenance_after="first=acct:actor;last=" if kind == "item" else "",
                price=75 if reason == "shop.purchase" else 0,
                currency="" if kind == "item" else "copper-equivalent",
                funding="" if kind == "item" else "carried-cash",
            )
            change = {
                "subject_id": f"item:lineage-{index}" if kind == "item" else "currency:gold",
                "lineage_id": f"item:lineage-{index}" if kind == "item" else "",
                "before": 0 if kind == "item" else 1,
                "delta": 1 if kind == "item" else 2,
                "after": 1 if kind == "item" else 3,
            }
            records.append(self.record(
                "intent", attempted, version=2, kind=kind, reason=reason, details=details,
                change=change,
            ))
            records.append(self.record(
                "intent", committed, version=2, kind=kind, reason=reason, details=details,
                change=change,
            ))
            records.append(self.record(
                "commit",
                committed,
                version=2,
                domains=[{"kind": "player", "id": "acct/hero"}],
            ))
        self.write(*records)
        journal = gameplay_journal.load([self.root])
        for index, _family in enumerate(families):
            self.assertEqual(journal.transactions[f"attempted-{index}"]["status"], "attempted")
            self.assertEqual(journal.transactions[f"committed-{index}"]["status"], "committed")

    def test_reconcile_compares_each_terminal_save_domain(self) -> None:
        details = self.details(
            actor="acct:actor",
            currency="copper-equivalent",
            funding="generated",
            source="service",
            destination="player-or-ground",
        )
        intent = self.record(
            "intent",
            "tx1",
            version=2,
            kind="currency",
            reason="script.currency-grant",
            details=details,
        )
        map_domain = self.record(
            "domain",
            "tx1",
            version=2,
            reason="domain.add",
            domain={"kind": "map-unique", "id": "/world/start"},
        )
        commit = self.record(
            "commit",
            "tx1",
            version=2,
            domains=[
                {"kind": "player", "id": "acct/hero"},
                {"kind": "map-unique", "id": "/world/start"},
            ],
        )
        self.write(intent, map_domain, commit)
        journal = gameplay_journal.load([self.root])
        player_save = self.root / "player.save"
        player_save.write_text(
            f"journal_run {RUN}\njournal_sequence 3\nendplst\nmsg\n"
            "journal_sequence 999\ncustody_actor forged\nendmsg\n"
        )
        map_save = self.root / "map.save"
        map_save.write_text(
            f"arch map\njournal_run {RUN}\njournal_sequence 2\nmsg\n"
            "journal_sequence 999\nendmsg\nend\narch sign\nmsg\n"
            "# gameplay-journal "
            f"{RUN} 999\njournal_sequence 999\nendmsg\nend\n"
        )
        unique_save = self.root / "map.v00"
        unique_save.write_text(
            "# gameplay-journal aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1\n"
        )
        plan = gameplay_journal.reconcile(
            journal,
            gameplay_journal._saved_domain_arguments(SimpleNamespace(
                domain=[],
                player_save=[f"acct/hero={player_save}"],
                map_save=[
                    f"/world/start={map_save}",
                    f"/world/start={unique_save}",
                ],
            )),
        )
        actions = {
            (domain["kind"], domain["id"]): domain["action"]
            for domain in plan["tx1"]["domains"]
        }
        self.assertEqual(actions[("player", "acct/hero")], "checkpointed")
        self.assertEqual(actions[("map-unique", "/world/start")], "replay-required")

        legacy_save = self.root / "legacy-player.save"
        legacy_save.write_text("endplst\narch human_male\nend\n")
        self.assertEqual(gameplay_journal._checkpoint_sequence(legacy_save), (0, "player"))
        with self.assertRaisesRegex(gameplay_journal.JournalError, "not a map save"):
            gameplay_journal._saved_domain_arguments(SimpleNamespace(
                domain=[], player_save=[], map_save=[f"/world/start={legacy_save}"],
            ))
        with self.assertRaisesRegex(gameplay_journal.JournalError, "not a player save"):
            gameplay_journal._saved_domain_arguments(SimpleNamespace(
                domain=[], player_save=[f"acct/hero={map_save}"], map_save=[],
            ))
        legacy_unique = self.root / "legacy.v00"
        legacy_unique.write_text(
            "arch coppercoin\nmsg\njournal_sequence 999\nendmsg\nend\n"
        )
        self.assertEqual(
            gameplay_journal._checkpoint_sequence(legacy_unique),
            (0, "map-unique"),
        )
        self.assertEqual(
            gameplay_journal._saved_domain_arguments(SimpleNamespace(
                domain=[], player_save=[],
                map_save=[f"/world/start={legacy_unique}"],
            )),
            ["map-unique:/world/start=0"],
        )
        invalid_save = self.root / "invalid-player.save"
        invalid_save.write_text("journal_sequence 3\nendplst\n")
        with self.assertRaisesRegex(gameplay_journal.JournalError, "no run identity"):
            gameplay_journal._checkpoint_sequence(invalid_save)

    def test_reconcile_groups_party_source_and_grants_as_one_batch(self) -> None:
        source_details = self.details(
            actor="acct:actor", currency="copper-equivalent", funding="party-corpse",
            source="corpse", destination="party-pool",
        )
        child_details = self.details(
            actor="acct:actor", currency="copper-equivalent", funding="source-tx",
            source="corpse", destination="player-or-ground",
        )
        self.write(
            self.record(
                "intent", "source-tx", version=2, kind="currency",
                reason="party.currency-source", details=source_details,
                change={
                    "subject_id": "currency:party-corpse", "lineage_id": "",
                    "before": 5, "delta": -5, "after": 0,
                },
            ),
            self.record(
                "domain", "source-tx", version=2, reason="domain.add",
                domain={"kind": "map-runtime", "id": "/world/start"},
            ),
            self.record(
                "intent", "grant-tx", version=2, kind="currency",
                reason="party.currency-split", details=child_details,
                change={
                    "subject_id": "currency:party-loot", "lineage_id": "",
                    "before": 10, "delta": 5, "after": 15,
                },
            ),
            self.record(
                "commit", "source-tx", version=2,
                domains=[
                    {"kind": "player", "id": "acct/hero"},
                    {"kind": "map-runtime", "id": "/world/start"},
                ],
            ),
            self.record(
                "commit", "grant-tx", version=2,
                domains=[{"kind": "player", "id": "acct/hero"}],
            ),
        )
        plan = gameplay_journal.reconcile(
            gameplay_journal.load([self.root]),
            ["player:acct/hero=0", "map-runtime:/world/start=0"],
        )
        self.assertEqual(plan["source-tx"]["action"], "replay-party-batch")
        self.assertEqual(
            plan["source-tx"]["batch_transactions"], ["source-tx", "grant-tx"],
        )
        self.assertEqual(plan["grant-tx"]["action"], "covered-by-party-batch")
        self.assertEqual(
            plan["grant-tx"]["domains"][0]["action"], "covered-by-party-batch",
        )
        incomplete_plan = gameplay_journal.reconcile(
            gameplay_journal.load([self.root]),
            ["player:acct/hero=0"],
        )
        self.assertEqual(
            incomplete_plan["source-tx"]["action"], "inspect-party-batch",
        )
        malformed = gameplay_journal.load([self.root])
        malformed.transactions["grant-tx"]["intent"]["change"]["delta"] = 4
        with self.assertRaisesRegex(
            gameplay_journal.JournalError, "do not conserve source value",
        ):
            gameplay_journal.reconcile(
                malformed,
                ["player:acct/hero=0", "map-runtime:/world/start=0"],
            )

        self.sequence = 0
        self.previous = ""
        self.write(self.record(
            "intent", "source-tx", version=2, kind="currency",
            reason="party.currency-source", details=source_details,
            change={
                "subject_id": "currency:party-corpse", "lineage_id": "",
                "before": 5, "delta": -5, "after": 0,
            },
        ))
        attempted_plan = gameplay_journal.reconcile(
            gameplay_journal.load([self.root]),
            ["player:acct/hero=0"],
        )
        self.assertEqual(attempted_plan["source-tx"]["action"], "inspect-party-batch")

        self.sequence = 0
        self.previous = ""
        child_details["funding"] = "source-tx"
        self.write(
            self.record(
                "intent", "source-tx", version=2, kind="currency",
                reason="party.currency-source", details=source_details,
                change={
                    "subject_id": "currency:party-corpse", "lineage_id": "",
                    "before": 5, "delta": -5, "after": 0,
                },
            ),
            self.record(
                "intent", "grant-tx", version=2, kind="currency",
                reason="party.currency-split", details=child_details,
                change={
                    "subject_id": "currency:party-loot", "lineage_id": "",
                    "before": 10, "delta": 2, "after": 12,
                },
            ),
            self.record(
                "abort", "grant-tx", version=2,
                domains=[{"kind": "player", "id": "acct/hero"}],
            ),
            self.record(
                "abort", "source-tx", version=2,
                domains=[{"kind": "player", "id": "acct/hero"}],
            ),
        )
        aborted_plan = gameplay_journal.reconcile(
            gameplay_journal.load([self.root]),
            ["player:acct/hero=0"],
        )
        self.assertEqual(aborted_plan["source-tx"]["action"], "none")
        self.assertEqual(aborted_plan["grant-tx"]["action"], "covered-by-party-batch")

    def test_reconcile_cli_preserves_intent_sequence_order(self) -> None:
        details = self.details(
            actor="acct:actor", currency="copper-equivalent", source="corpse",
            destination="player", funding="party-pool",
        )
        records = []
        for transaction in ("z-source", "a-grant"):
            records.extend((
                self.record("intent", transaction, version=2, details=details),
                self.record(
                    "commit", transaction, version=2,
                    domains=[{"kind": "player", "id": "acct/hero"}],
                ),
            ))
        self.write(*records)
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(gameplay_journal.main([
                str(self.root), "reconcile", "--domain", "player:acct/hero=999",
            ]), 0)
        plan = json.loads(output.getvalue())
        self.assertEqual(
            [entry["transaction_id"] for entry in plan],
            ["z-source", "a-grant"],
        )

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
                "intent", "tx1", version=2,
                details=self.details(
                    actor="acct:actor", currency="copper-equivalent",
                    source="service", destination="player", funding="generated",
                ),
            ),
            self.record(
                "commit", "tx1", version=2,
                domains=[{"kind": "player", "id": "acct/hero"}],
            ),
            self.record(
                "domain", "tx1", version=2, reason="domain.add",
                domain={"kind": "map-runtime", "id": "/world/start"},
            ),
        )
        with self.assertRaisesRegex(gameplay_journal.JournalError, "after terminal"):
            gameplay_journal.load([self.root])

        self.sequence = 0
        self.previous = ""
        self.write(self.record(
            "intent", "tx1", version=2,
            domains=[{"kind": "player", "id": "other/hero"}],
            details=self.details(
                actor="acct:actor", currency="copper-equivalent",
                source="service", destination="player", funding="generated",
            ),
        ))
        with self.assertRaisesRegex(gameplay_journal.JournalError, "intent save domains"):
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
        self.write(self.record(
            "intent",
            "tx1",
            version=2,
            kind="item",
            change={
                "subject_id": "item:lineage",
                "lineage_id": "item:lineage",
                "before": 0,
                "delta": 1,
                "after": 1,
            },
            details=self.details(),
        ))
        with self.assertRaisesRegex(gameplay_journal.JournalError, "item semantic details"):
            gameplay_journal.load([self.root])

        item_change = {
            "subject_id": "item:lineage",
            "lineage_id": "item:lineage",
            "before": 0,
            "delta": 1,
            "after": 1,
        }
        valid_details = self.details(
            archetype="sword",
            object_type=15,
            snapshot="arch=sword;type=15;nrof=1;value=0;weight=0",
            quantity=1,
            source="service",
            destination="player",
            actor="acct:actor",
            provenance_before="first=;last=",
            provenance_after="first=acct:actor;last=",
        )
        self.sequence = 0
        self.previous = ""
        noncanonical_snapshot = dict(valid_details)
        noncanonical_snapshot["snapshot"] = "arch=sword;type=015;nrof=1;value=0;weight=0"
        self.write(self.record(
            "intent", "tx1", version=2, kind="item", change=item_change,
            details=noncanonical_snapshot,
        ))
        with self.assertRaisesRegex(gameplay_journal.JournalError, "item snapshot"):
            gameplay_journal.load([self.root])

        self.sequence = 0
        self.previous = ""
        noncanonical_provenance = dict(valid_details)
        noncanonical_provenance["provenance_before"] = "password=secret"
        self.write(self.record(
            "intent", "tx1", version=2, kind="item", change=item_change,
            details=noncanonical_provenance,
        ))
        with self.assertRaisesRegex(gameplay_journal.JournalError, "item provenance"):
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

    def test_native_action_matrix_reasons_remain_wired_to_semantic_producers(self) -> None:
        server = Path(__file__).resolve().parents[2]
        def function_body(source: str, name: str) -> tuple[str, str]:
            masked = list(source)
            comments_masked = list(source)
            index = 0
            while index < len(source):
                if source.startswith("/*", index):
                    end = source.find("*/", index + 2)
                    self.assertNotEqual(end, -1, "unterminated C comment")
                    for offset in range(index, end + 2):
                        if masked[offset] != "\n":
                            masked[offset] = " "
                            comments_masked[offset] = " "
                    index = end + 2
                elif source.startswith("//", index):
                    end = source.find("\n", index + 2)
                    end = len(source) if end == -1 else end
                    for offset in range(index, end):
                        masked[offset] = " "
                        comments_masked[offset] = " "
                    index = end
                elif source[index] in {'"', "'"}:
                    quote = source[index]
                    index += 1
                    while index < len(source) and source[index] != quote:
                        if source[index] == "\\":
                            masked[index] = " "
                            index += 1
                        if index < len(source) and masked[index] != "\n":
                            masked[index] = " "
                        index += 1
                    index += 1
                else:
                    index += 1
            structural = "".join(masked)
            match = re.search(
                rf"(?m)^[ \t]*(?:[A-Za-z_][A-Za-z0-9_]*[ \t\n*]+)+"
                rf"{re.escape(name)}[ \t]*\([^;{{}}]*?\)[ \t\n]*\{{",
                structural,
                re.S,
            )
            self.assertIsNotNone(match, name)
            start = match.end() - 1
            depth = 0
            for index in range(start, len(structural)):
                if structural[index] == "{":
                    depth += 1
                elif structural[index] == "}":
                    depth -= 1
                    if depth == 0:
                        return (
                            "".join(comments_masked[start:index + 1]),
                            structural[start:index + 1],
                        )
            self.fail(f"unterminated function body: {name}")

        expected = {
            "src/types/player.c": {
                "item.acquire", "item.drop", "item.external-transfer",
                "item.player-transfer", "item.startequip-destroy",
            },
            "src/server/treasure.c": {"item.starting-grant", "item.treasure-grant"},
            "src/server/quest.c": {"quest.item-grant", "quest.objective-grant"},
            "src/server/party.c": {
                "item.party-loot", "party.currency-loot", "party.currency-split",
            },
            "src/server/shop.c": {
                "shop.purchase", "shop.sale", "script.currency-grant", "script.payment",
            },
            "src/server/bank.c": {"bank.deposit", "bank.withdraw"},
            "src/server/spell_effect.c": {"spell.alchemy"},
        }
        for relative, reasons in expected.items():
            source = (server / relative).read_text(encoding="utf-8")
            for reason in reasons:
                with self.subTest(source=relative, reason=reason):
                    self.assertIn(f'"{reason}"', source)

        semantic_calls = {
            "src/types/player.c": ("object_custody_begin", "object_custody_finish"),
            "src/server/treasure.c": ("object_insert_into_reason",),
            "src/server/quest.c": ("object_custody_begin", "object_insert_into_reason"),
            "src/server/party.c": (
                "gameplay_journal_currency_begin", "object_insert_into_reason",
            ),
            "src/server/shop.c": (
                "object_custody_begin_economy", "gameplay_journal_currency_begin",
            ),
            "src/server/bank.c": ("gameplay_journal_currency_begin",),
            "src/server/spell_effect.c": ("gameplay_journal_currency_begin",),
        }
        for relative, calls in semantic_calls.items():
            source = (server / relative).read_text(encoding="utf-8")
            for call in calls:
                with self.subTest(source=relative, call=call):
                    self.assertIn(f"{call}(", source)

        exact_sites = {
            ("src/types/player.c", "pick_up_object"): (
                {"item.acquire", "item.player-transfer"}, {"object_custody_begin_parties"},
            ),
            ("src/types/player.c", "put_object_in_sack"): (
                {"item.acquire", "item.external-transfer", "item.player-transfer"},
                {"object_custody_begin_parties"},
            ),
            ("src/types/player.c", "drop_object"): (
                {"item.drop", "item.startequip-destroy"}, {"object_custody_begin"},
            ),
            ("src/server/treasure.c", "treasure_insert"): (
                {"item.starting-grant", "item.treasure-grant"}, {"object_insert_into_reason"},
            ),
            ("src/server/quest.c", "quest_check_item_drop"): (
                {"quest.item-grant"}, {"object_custody_begin", "object_insert_into_reason"},
            ),
            ("src/server/quest.c", "quest_check_item"): (
                {"quest.objective-grant"}, {"object_insert_into_reason"},
            ),
            ("src/server/party.c", "party_loot_random"): (
                {"item.party-loot", "party.currency-loot"},
                {"party_currency_begin_at_corpse", "object_insert_into_reason"},
            ),
            ("src/server/party.c", "party_currency_source_begin"): (
                {"party.currency-source"}, {"party_currency_begin_at_corpse"},
            ),
            ("src/server/party.c", "party_loot_split"): (
                {"item.party-loot", "party.currency-split"},
                {"party_currency_source_begin", "gameplay_journal_currency_begin"},
            ),
            ("src/server/shop.c", "shop_pay"): ({"script.payment"}, {"shop_pay_reason"}),
            ("src/server/shop.c", "shop_pay_items_rec"): (
                {"shop.purchase"}, {"shop_pay_internal"},
            ),
            ("src/server/shop.c", "shop_sell_item_begin"): (
                {"shop.sale"}, {"object_custody_begin_economy"},
            ),
            ("src/server/shop.c", "shop_insert_coins"): (
                {"script.currency-grant"}, {"shop_insert_coins_reason"},
            ),
            ("src/server/bank.c", "bank_deposit"): (
                {"bank.deposit"}, {"gameplay_journal_currency_begin"},
            ),
            ("src/server/bank.c", "bank_withdraw"): (
                {"bank.withdraw"}, {"gameplay_journal_currency_begin"},
            ),
            ("src/server/spell_effect.c", "cast_transform_wealth"): (
                {"spell.alchemy"}, {"gameplay_journal_currency_begin"},
            ),
            ("src/plugins/plugin_python/atrinik_object.c", "Atrinik_Object_Decrease"): (
                {"script.item-decrease"}, {"object_decrease_reason"},
            ),
            ("src/plugins/plugin_python/atrinik_object.c", "Atrinik_Object_CreateObject"): (
                {"script.currency-grant"}, {"object_insert_into_reason"},
            ),
            ("src/plugins/plugin_python/atrinik_object.c", "Object_SetAttribute"): (
                {"script.item-adjust", "script.item-value-adjust",
                 "script.currency-adjust", "script.bank-adjust"},
                {"object_set_nrof_reason", "object_set_value_reason",
                 "shop_set_coin_nrof_reason", "bank_set_balance_reason"},
            ),
        }
        for (relative, function), (reasons, calls) in exact_sites.items():
            body, structural_body = function_body(
                (server / relative).read_text(encoding="utf-8"), function,
            )
            with self.subTest(source=relative, function=function):
                for reason in reasons:
                    self.assertIn(f'"{reason}"', body)
                for call in calls:
                    self.assertIn(f"{call}(", structural_body)


if __name__ == "__main__":
    unittest.main()
