# Private gameplay audit journal

The gameplay journal is a server-owned, operator-only recovery record for
selected item, currency, quest, and progression transactions. It is not a
player statistics feature, a replacement for cumulative metrics, or an
operational/security log.

## Schema and identity

Every line is one UTF-8 JSON object using schema version 1. Records contain:

- a globally unambiguous `event_id` composed from the stable server identity,
  random 128-bit run ID, and monotonic per-run sequence;
- a random 128-bit `transaction_id` shared by one intent and its terminal
  commit or abort, plus UTC time, server/run identity, phase, kind, and semantic
  reason code;
- stable canonical account and character identities and, for intents, typed
  map coordinates and `before`/`delta`/`after` values;
- a stable subject ID and optional item-lineage ID rather than a display name or
  serialized object description;
- immutable world-profile ID, schema, digest, and effective-axis identity; and
- `prev_hash` and `record_hash`, where `record_hash` is SHA-256 over the exact
  bytes preceding the final `,"record_hash":"..."}` field.

Classic does not yet have the frozen profile identity owned by #207. Until it
does, run-start records use the explicit fail-closed profile
`legacy-unknown`/schema `0`/digest `unknown`/axes `unknown`. This cannot be
mistaken for canonical standard or accelerated progress. The profile boundary
API lets #212 record the frozen identity at an explicit fork or migration
without changing this journal format.

All identifiers and reason codes are bounded tokens. The API cannot accept raw
JSON or free-form descriptions. Passwords, failed credentials, session or
rendezvous secrets, chat, arbitrary inscription text, and unrelated
player-authored content are forbidden.

## Transaction and durability boundary

A producer follows exactly this order:

1. Construct and validate the typed transaction.
2. Append the intent and flush it with `fsync` (`_commit` on Windows).
3. Only after that succeeds, perform the gameplay mutation.
4. If the mutation is vetoed or rolled back, append and sync an abort. If it
   succeeds, append and sync a commit.

An intent failure means the mutation must not begin. A commit/abort failure
marks the journal unavailable; no new journal-backed mutation can start and the
main loop enters orderly fail-stop shutdown. The operator must treat the final
intent as ambiguous instead of assuming recovery coverage.

The supported crash model is abrupt process termination after a successful
sync. A fully synced commit survives that model. The exact phase outcomes are:

| Crash point | Durable evidence | Recovery meaning |
| --- | --- | --- |
| Before intent sync | none | action did not begin through this API |
| After intent, before mutation | intent only | attempted; reconcile with authoritative state |
| During/after mutation, before commit | intent only | ambiguous; never claim committed success |
| After commit sync | intent plus commit | committed success |
| After abort sync | intent plus abort | did not commit |

Host power loss, a filesystem or storage device that violates durable-flush
semantics, media loss, kernel corruption, and rollback of the entire server
state/journal volume are outside that boundary. Operators should place state
and journal on the same durability domain and include both in backups.

## Files, privacy, and retention

Records live under `DATAPATH/gameplay-journal`, normally mode `0700`, in files
named `journal-RUNID-NNNN.jsonl`, mode `0600`. A file rotates before exceeding
8 MiB. At process start the server retains at most 15 older regular, private
journal files before opening the new file, for an upper bound of 16 files
(approximately 128 MiB). Unsafe types, symlinks, or insecure retained-file
permissions fail startup closed. Retention is age-ordered and intentionally
does not inspect or modify unrelated names.

## Validation, query, and recovery

Stop the server or work on a protected copy when conducting recovery. From the
Classic checkout:

```sh
python3 server/tools/gameplay_journal.py \
  DATAPATH/gameplay-journal validate
python3 server/tools/gameplay_journal.py \
  DATAPATH/gameplay-journal reconcile
python3 server/tools/gameplay_journal.py \
  DATAPATH/gameplay-journal query --transaction TRANSACTION_ID
python3 server/tools/gameplay_journal.py \
  DATAPATH/gameplay-journal query --account ACCOUNT_ID
python3 server/tools/gameplay_journal.py \
  DATAPATH/gameplay-journal query --character CHARACTER_ID
python3 server/tools/gameplay_journal.py \
  DATAPATH/gameplay-journal query --subject SUBJECT_ID
python3 server/tools/gameplay_journal.py \
  DATAPATH/gameplay-journal query --lineage LINEAGE_ID
```

Validation checks schema, duplicate JSON fields, permissions, redaction,
event-ID uniqueness, per-run sequence, and every hash chain across rotated
files. Only an unterminated final record is classified as a torn tail and
ignored; malformed interior records, a torn rotated predecessor followed by
more records, conflicting duplicate intents, commit-without-intent, and
commit/abort conflicts fail validation.

Reconciliation is idempotent: identical repeated intents and terminal records
do not apply a mutation. A transaction ending at `attempted` requires the
operator to compare its typed before/after values with authoritative saved
state. `committed` is applied at most once by transaction ID; `aborted` is
never applied. The tool reports evidence and does not edit game state.

## Trusted producer APIs

Native code uses `gameplay_journal_begin()`, performs the mutation only after
success, and then calls `gameplay_journal_commit()` or
`gameplay_journal_abort()`. The API validates kind, identifiers, bounds, and
the exact arithmetic invariant `before + delta == after`.

Trusted content uses the typed player methods rather than formatting records:

```python
transaction = player.JournalIntent(
    "currency", "quest.reward", "currency:gold", before, delta, after
)
try:
    perform_mutation()
except Exception:
    player.JournalAbort(transaction, "quest.reward_failed")
    raise
else:
    player.JournalCommit(transaction)
```

Item intents additionally pass a stable lineage ID. Quest and progression
intents use stable authored subject IDs. Producers must not catch and ignore a
journal exception or emit a commit for a vetoed/failed mutation.

## Choosing the right record

| Need | Facility | Rationale |
| --- | --- | --- |
| Lifetime counts, maxima/minima, bounded sets, and aggregate coverage | Gameplay metrics | Bounded checkpointed statistics; no ordered recovery claim |
| Selected item custody/economy, quest lifecycle, rare progression/survival, or privileged persistent mutation | Gameplay journal | Ordered typed intent/terminal evidence with a durable boundary |
| Startup, networking, plugin, configuration, security, or human diagnostics | Operational/security logging | Human/operator diagnostics; not replay evidence |
| Movement, attacks, damage/healing ticks, routine skill use, ordinary kills, chat, or emotes | Not journaled | Excess volume, privacy, and no recovery value; aggregates where useful |

Internal stack merges/reordering, low-level object housekeeping, regeneration,
routine XP ticks, ordinary map traversal, and metric reads remain aggregate-only
or unrecorded. Future producers must document their semantic success boundary
and recovery value before adding a journal event.
