# Private gameplay audit journal

The gameplay journal is a server-owned, operator-only recovery record for
selected item, currency, quest, and progression transactions. It is not a
player statistics feature, a replacement for cumulative metrics, or an
operational/security log.

## Schema and identity

Every line is one UTF-8 JSON object. New records use schema version 2; the
offline tool continues to validate retained version-1 files. Records contain:

- a globally unambiguous `event_id` composed from the stable server identity,
  random 128-bit run ID, and monotonic per-run sequence;
- a random 128-bit `transaction_id` shared by one intent and its terminal
  commit or abort, plus UTC time, server/run identity, phase, kind, and semantic
  reason code;
- stable canonical account and character identities and, for intents, typed
  map coordinates and `before`/`delta`/`after` values;
- a stable subject ID and kind-required item-lineage ID rather than a display
  name or serialized object description;
- for version-2 intents, bounded semantic `details`: item archetype/type and an
  immutable snapshot, quantity, source/destination, actor/counterparty,
  provenance before/after, total price, currency, and funding source;
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
| After commit sync | intent plus commit | operation completed in memory; compare/replay against the last saved state |
| After abort sync | intent plus abort | did not commit |

Host power loss, a filesystem or storage device that violates durable-flush
semantics, media loss, kernel corruption, and rollback of the entire server
state/journal volume are outside that boundary. Operators should place state
and journal on the same durability domain and include both in backups.

## Files, privacy, and retention

Records live under `DATAPATH/gameplay-journal`, normally mode `0700`, in files
named `journal-RUNID-NNNN.jsonl`, mode `0600`. An exclusive `journal.lock`
prevents two server processes from sharing a journal directory. Files target
8 MiB; a file with in-flight intents stays open until their terminal records
are synced, with a 64-intent cap and a validator-enforced 9 MiB hard bound.
Transactions therefore never straddle retained files. Before each new file the
server retains at most 15 older regular, private journal files, for an upper
bound of 16 files (approximately 144 MiB). A transaction that remains open at
the rotation target may extend the current file only to the 9 MiB hard limit;
the writer then fails closed before appending beyond that bound. Unsafe types, symlinks, insecure
retained-file permissions, or lock contention fail startup closed. Retention is
age-ordered and intentionally does not inspect or modify unrelated names.

When retention removes the beginning of a run, the first retained file is the
declared verification horizon: its first sequence and `prev_hash` identify the
pruned prefix, and subsequent retained records remain hash-chained from that
value. Integrity before the retained horizon can only be established from an
older protected copy or backup. Because transactions never cross files,
reconciliation within the retained horizon still has every intent and terminal
outcome.

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

Validation checks schema, filenames, duplicate JSON fields, permissions,
redaction, size bounds, event-ID uniqueness, per-run sequence, and every hash
chain across rotated files. Only an unterminated record in the highest retained
file for its run is classified as a torn tail and ignored; malformed interior
records, a torn rotated predecessor followed by more records, conflicting
duplicate intents, commit-without-intent, and commit/abort conflicts fail
validation.

Reconciliation is idempotent: identical repeated intents and terminal records
do not themselves apply a mutation. Both `attempted` and `committed`
transactions require the operator to compare their typed before/after values,
lineage, and bounded snapshot with authoritative saved state. A terminal commit
proves that the in-memory operation completed before the crash; it does not
claim that a later periodic player/map checkpoint was already durable. Replay a
committed transaction only when saved state still matches its `before` side;
when saved state matches `after`, do not apply it again. For an attempted
transaction, either side may be authoritative because the crash may have
occurred during the mutation. An `aborted` transaction is never replayed. The
tool reports the complete evidence needed for this comparison and deliberately
does not edit game state.
Account, character, subject, and lineage queries first select matching intents,
then return each selected transaction's complete ordered intent/terminal
timeline so an attempted action cannot be mistaken for a committed change.

## Native item and economy action matrix

Each row names the sole authoritative producer for one completed logical
operation. Helpers below that producer do not emit another transaction.

| Entry point or action | Producer | Reason code | Transaction boundary |
| --- | --- | --- | --- |
| Floor/external pickup | `pick_up_object()` | `item.acquire` | Intent after veto/capacity checks; commit after the actual split/merge survivor is inserted |
| Cross-player pickup | `pick_up_object()` | `item.player-transfer` | One item transaction with the other stable actor as counterparty |
| Ground drop | `drop_object()` | `item.drop` | Intent after drop vetoes; commit after map insertion |
| Same-player nested-container move | none | aggregate-only | Provenance and custody do not change |
| Player to external container/player | `put_object_in_sack()` | `item.external-transfer` / `item.player-transfer` | Intent before split; commit after destination insertion |
| Starting/treasure/quest grant | `treasure_insert()` / quest grant site | `item.starting-grant`, `item.treasure-grant`, `quest.item-grant`, `quest.objective-grant` | One reason-aware insertion transaction |
| Trusted Python item grant/transfer/removal/destruction | `object_insert_into_reason()` / `object_remove_reason()` | Caller-supplied bounded reason; documented `script.item-*` defaults | Intent before insertion, removal, or destruction; one terminal record |
| Trusted Python player-inventory to map transfer | `object_insert_map_reason()` / `object_enter_map_reason()` | Caller-supplied reason; `script.item-drop` / `script.item-teleport` defaults | Intent before removal; provenance before map merge; terminal result exposed as committed, failed, or ambiguous |
| Party item loot | `party_loot_random()` / `party_loot_split()` | `item.party-loot` | Reason-aware grant; source remains in the corpse if intent preparation fails |
| Party currency loot | `party_loot_random()` / `party_loot_split()` | `party.currency-loot` / `party.currency-split` | Random transfer journals before moving the source stack; split mode stages every recipient intent before source removal and commits after exact delivery |
| Shop checkout | `shop_pay_internal()` | `shop.purchase` | One item transaction containing quantity, total price, and carried/bank/mixed funding; no generic payment or bank duplicate |
| Shop sale | `shop_sell_item_begin()` / `shop_sell_item_commit()` | `shop.sale` | Intent before split; commit after coins, unpaid state, and provenance change |
| Bank deposit/withdrawal | `bank_deposit()` / `bank_withdraw()` | `bank.deposit` / `bank.withdraw` | Exact hidden balance before/delta/after around the mutation |
| Bank-funded checkout | `shop_pay_internal()` | `shop.purchase` | The correlated purchase transaction is also the sole hidden-bank write record |
| Generated currency | `shop_insert_coins_reason()` | `script.currency-grant` or caller reason | Intent before coin insertion; commit only after exact inventory/floor delivery |
| Alchemy currency replacement | `cast_transform_wealth()` | `spell.alchemy` | One before/delta/after transaction around source removal and replacement delivery |
| Trusted Python payment | `shop_pay_reason()` | Caller reason; `script.payment` by default | One currency transaction around carried/bank payment |
| Start-equipment destruction | `drop_object()` | `item.startequip-destroy` | Item captured before destruction; commit afterward |

Crash/restart coverage groups those entry points by their authoritative state
shape: item grant/acquisition, item removal/destruction, generated currency,
bank deposit/withdrawal, and shop purchase/sale. Starting, treasure, quest,
party, alchemy, and trusted-script producers delegate to the corresponding
tested reason-aware item or currency boundary. The action-matrix source audit
verifies each concrete reason and business-state call site.

The item transaction's top-level arithmetic is quantity for ordinary custody.
For purchases it represents the balance actually mutated (the hidden bank
slice for bank or mixed funding), while `details.price` always holds the
complete copper-equivalent price. Shop sales, generated/party currency, and
alchemy use a recovery aggregate spanning player-held/bank currency plus
canonical currency on the player's current delivery tile. Their before/after
values therefore remain comparable when output is carried, spilled, or split
across the two, including a partially checkpointed delivery. Bank records
remain exact hidden-balance arithmetic. These conventions keep recovery
arithmetic authoritative without double-counting correlated writes.

Journal-backed currency output is materialized in non-merging stacks carrying
`currency:<transaction_id>` as durable lineage. During recovery, the hidden
bank balance (where applicable) and those tagged survivors distinguish an
unapplied intent, an interrupted delivery, and a fully applied operation. The
tag applies equally to inventory and floor output. After the terminal commit is
durable, the producer retires it and permits ordinary merging; intent-only
output keeps the tag for reconciliation without permanent fragmentation.

Existing aggregate metrics remain at the same semantic boundaries. Covered
positive-value bank/shop action counts and pickup/drop unit counts advance only
after a journal commit. Zero-price checkout remains journaled as an item
transfer but deliberately does not change the historical positive-value shop
metric.

## Trusted producer APIs

Native semantic code uses the reason-aware custody/currency adapters, which
write an intent before mutation and a terminal commit or abort afterward. New
producers may use `gameplay_journal_begin()` directly only when they preserve
that order. The API validates kind, identifiers, bounds, non-negative price,
and the exact arithmetic invariant `before + delta == after`.

Trusted content uses the typed player methods rather than formatting records:

```python
transaction = player.JournalIntent(
    "quest", "quest.advance", "quest:example", before, delta, after
)
try:
    perform_mutation()
except Exception:
    player.JournalAbort(transaction, "quest.reward_failed")
    raise
else:
    player.JournalCommit(transaction)
```

Item and currency mutations use `InsertInto`, `Remove`, `Destroy`, `Map.Insert`,
`TeleportTo`, `InsertCoins`, and `PayAmount` so schema-v2 semantic details
cannot be omitted. Quest and progression intents use stable authored subject IDs.
Producers must not catch and ignore a journal exception or emit a commit for a
vetoed/failed mutation.

`JournalIntent` retains its released C/Python signature, but schema-v2
hardening intentionally rejects its legacy `item` and `currency` kinds because
that signature cannot supply mandatory custody/economy details. Existing
content must migrate those calls to the reason-aware methods above; `quest` and
`progression` remain source compatible. Retained schema-v1 files remain
readable, but the server never writes new incomplete schema-v1 intents.

## Choosing the right record

| Need | Facility | Rationale |
| --- | --- | --- |
| Lifetime counts, maxima/minima, bounded sets, and aggregate coverage | Gameplay metrics | Bounded checkpointed statistics; no ordered recovery claim |
| Selected item custody/economy, quest lifecycle, rare progression/survival, or privileged persistent mutation | Gameplay journal | Ordered typed intent/terminal evidence with a durable boundary |
| Startup, networking, plugin, configuration, security, or human diagnostics | Operational/security logging | Human/operator diagnostics; not replay evidence |
| Movement, attacks, damage/healing ticks, routine skill use, ordinary kills, chat, or emotes | Not journaled | Excess volume, privacy, and no recovery value; aggregates where useful |

Internal stack splits/merges and same-player reordering are implementation
details of the single surrounding semantic action. Generic `object_destroy()`,
map teardown, temporary effect/force creation, regeneration, routine
consumption and ammunition, ordinary loot consumption, routine XP ticks, map
traversal, and metric reads remain aggregate-only or unrecorded. Their volume
is high, their actor/reason context is often absent, and replaying individual
helper calls would add lifecycle noise rather than recoverable transactions.
Non-routine persisted grants/removals instead enter through the reason-aware
APIs in the matrix. Future producers must document their semantic success
boundary and recovery value before adding a journal event.
