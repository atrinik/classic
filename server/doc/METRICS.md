# Authoritative gameplay metrics

Atrinik records durable gameplay facts in a server-authoritative metrics
subsystem. Metrics are private operational state: they are not sent to the
client, ordinary players cannot inspect them, and recording a metric does not
make it an achievement or imply a reward.

The registry in `src/server/metrics.c` is the source of truth for stable save
names, scope, value kind, unit, operator text, visibility, aggregation, and
lifetime reset policy. Registry enum values are an in-memory implementation
detail and are never serialized. All gameplay metrics are lifetime values;
seasonal or monthly views must be derived by a future optional analytics
export without resetting or mutating authoritative values.

Metrics are checkpointed aggregates, not an ordered recovery log. Selected
item, currency, quest, progression, and privileged persistent transactions use
the separate [private gameplay audit journal](GAMEPLAY_JOURNAL.md), whose
intent/commit protocol and synced records distinguish attempted work from a
committed success. Startup, networking, configuration, security, and human
diagnostics remain in operational logs. Movement, attacks, per-hit effects,
chat, and other high-volume or player-authored data are never promoted to the
journal merely because an aggregate metric exists.

## Ownership and persistence

Character metrics belong to exactly one playable character and are stored in
the character directory's versioned `metrics.dat` sidecar. The sidecar is
written with mode 0600 through the same flushed temporary-file-and-rename
primitive used by account files. It is checkpointed with normal character
saves, clean logout, and orderly shutdown. Event hooks only update memory; a
movement, damage, spell, or item event never performs file or network I/O.

Account metrics are deliberately separate and are serialized in the owning
account's already atomic mode-0600 account file. Authentication, roster, and
account metric changes therefore replace one complete account snapshot.
Character-session account updates resolve the account exclusively from the
authenticated socket, never from command or display text.

Both stores use schema version 1 and record `metrics_epoch`, the UTC time at
which authoritative collection began. Stable unknown scalar, collection, and keyed-series
records are retained verbatim when an older server saves the store. Malformed
known records, oversized lines, invalid subject IDs, and collection overflow
reject the metrics load without replacing the previous file. Character
gameplay can continue after a malformed sidecar, but that session will not
overwrite it; malformed embedded account metrics prevent an account rewrite
and successful authentication completion until an operator repairs the file.

Backups must include the complete `data/players` and `data/accounts` trees.
Restoring only `player.dat`, only `metrics.dat`, or only a subset of an account
file can roll related facts back to different checkpoints. A future
achievement reward and the metric/unlock that grants it must be saved through
the same logical character checkpoint.

## Scopes

Character metrics include lifecycle/session timing, progression, combat,
survival and death attribution, movement, magic/ranged effectiveness,
effective healing, banking and shop flows, item units, books, consumables,
containers, traps, parties, scripted auctions/post, inscriptions, and quests. Character unique
sets cover stable quest and nested quest-part UIDs, canonical maps, acquired region maps,
visited and savebed regions, book artifacts/archetypes, food/potion/scroll archetypes, learned
spells and skills, explicit authored landmark/lore/boss tags, and UTC active dates. Bounded keyed
series retain per-ID quest outcomes, monster-archetype/family kills, skill levels/experience/uses,
construction archetypes, bounty factions, spell outcomes, and mana consumption.

Account metrics include successful authentication, successful character
session starts/completions, active time, longest character session, roster facts, and the
highest character level. Account collections deliberately union only selected
discoveries and roster facts: characters and playable archetypes used, quests and quest parts,
maps, regions, landmarks, lore, books, consumable archetypes, spells, skills, savebed regions,
bosses, and UTC active dates. Damage, kills, deaths, item actions,
and similar character counters are not automatically summed into account
progress.

Two characters with partial progress never satisfy a character threshold by
sharing an account. Future account achievements must explicitly consume an
account metric or collection.

## Stable identifiers and bounds

Serialized scalar, collection, and keyed-series names are registry-owned dotted IDs.
Unique subjects accept at most 255 ASCII alphanumeric or `_./:-+` characters.
Most collections and keyed series are bounded to 512 entries. Content-wide map, book, and
monster-archetype dimensions retain up to 8192 IDs, while active UTC dates retain up to 4096
days (more than eleven years). Inserting an existing entry is
idempotent and inserting beyond the bound fails without evicting an existing
fact. Display names, localized labels, arbitrary chat, IP addresses,
connection IDs, passwords, and failed credentials are never metric subjects.

Canonical map paths are recorded except generated `/random/` instance paths. Content-backed
subjects are domain-qualified exactly as at catalog persistence and interchange boundaries.
Examples include `map:/shattered_islands/world_0303`, `region:nawerhals`,
`archetype:giant_ant`, `artifact:book_orthrack_1`, `skill:literacy`, `spell:magic_bullet`,
`quest:lost_memories`, `quest-part:lost_memories::find_book`, and `faction:nawerhals_city`.
Books prefer their applied artifact ID and fall back to their archetype ID. Runtime skill/spell
indices are never serialized. Existing monster `race` groupings are stored under the explicit
`monster-family:` telemetry domain. Content-ID renames are data migrations; silently changing
an identifier starts a distinct fact.

The content catalog is a build-time identity and cross-reference validator, not a runtime tag
database. Its emitted graph currently contains source locations and tens of thousands of
references but no authored monster-family, boss, landmark, lore, or achievement schema. The
server therefore consumes the same stable identities directly from validated runtime objects
and the stable spell/skill registries. Shipping and parsing the complete build graph would add
startup cost and duplicate data without exposing useful tags. When content introduces an
authored tag domain, the catalog and runtime collector should publish a compact runtime index
for that domain; the existing bounded metric collections can consume those stable tags.

## Event semantics

- A successful account authentication is a verified password followed by a
  successful atomic account save. Registration alone is not counted as a
  later login.
- A character session starts once, after the character has loaded, entered a
  map, and reached the playing state. It is separate from account
  authentication.
- Session, active, and AFK durations use the monotonic server clock. UTC wall
  time is used only for persisted timestamps and the metrics epoch. Autosaves
  checkpoint elapsed duration; wall-clock jumps cannot change it.
- A completed session is an orderly logout. Sessions lasting at least sixty
  seconds participate in the shortest-nontrivial value. A session has
  progression when it awards positive experience or completes a quest.
- Active-day collections use the UTC date at successful session start. Account
  values are set unions, so multiple characters and sessions on one day count once.
- Entering AFK is an explicit active-to-AFK transition. Reapplying an AFK
  auto-reply while already AFK does not create another entry.
- A movement step is one successful voluntary `move_ob` action, including a
  diagonal action as one step. Blocked moves, forced movement, pushes,
  knockback, teleports, map transitions, and administrative relocation do not
  count as steps. Successful cross-map insertion is tracked separately.
- Damage is effective hit-point loss after protection and capped for
  overkill. Attempted damage and immune/blocked damage do not count. Summoned
  damage and final-hit kills follow the existing `OWNER()`/experience-credit
  owner. PvP values are operational facts and require a separate anti-farming
  policy before any reward use.
- Melee attempts require a nonzero-damage direct player attack after event
  vetoes. Melee/projectile hits require positive effective damage. The engine
  has no critical-hit event, so no synthetic critical metric is recorded.
- Death causes are exclusive: player ownership is PvP, monster ownership is
  PvE, and starvation/traps/other non-living sources are environmental.
  Save-life prevention is counted separately and is not a death or respawn.
- Healing excludes overhealing and records effective healing to self, to
  another friendly target, and received from another friendly source
  separately.
- Pickups and drops count individual units after the operation succeeds, not
  stack actions. NPC-shop values are true sinks/sources. Bank values are
  labeled transfers and never interpreted as currency creation/destruction.
  Total currency spent covers every successful positive direct payment,
  including native shops and scripted services using `PayAmount`.
  Scripted merchant purchases, housing purchases/fees, Auction House actions,
  and post-office sends/receipts are recorded only after their trusted Python
  transaction completes.
- Normal player spell results are counted at the ranged-fire boundary, where
  success, failure, and actual mana consumption are known. Potions, rods,
  wands, NPC abilities, and scripted casts do not inflate these values. Successful learning,
  authoritative forgetting, current/highest known-spell counts, and lifetime learned spell IDs
  are tracked separately.
- Party active time is non-AFK monotonic time. Party kills and quest
  completions are credited to the character whose normal kill/quest hook fires;
  they are not duplicated onto every member.
- Trusted guild and jail APIs record submitted membership applications,
  voluntary departures, jail placements, and finite sentenced time. Offline
  administrative approvals/removals are intentionally not attributed to a
  character metric.
- A quest completion is recorded only from an explicit successful completion
  or from a completed quest object found during backfill. `FAILED` is never
  interpreted as completion. Existing completed saves are backfilled; missing
  historical attempts, kills, steps, damage, consumables, and play time are
  never fabricated.
- `QuestManager` reports only top-level lifecycle transitions to quest totals. Successful nested
  part completion records both a lifetime set and a repeat-aware per-ID count using the stable
  qualified part path; repeated calls on an already-final part are rejected. Failed quests remain
  failed rather than being stored as completed. Authored repeatable quests also increment an
  explicit repeatable-completion counter.
- DM/test characters currently update their own metrics. External analytics
  may filter them, but gameplay code must not reinterpret or reset their
  authoritative values.

## Operator inspection

`/metrics <player> [character|account|all] [category]` requires the explicit
`metrics` command permission (operators have all permissions). The target must
be online. The default `all` view prints nonzero character and authenticated
owning-account values privately to the invoking operator. Optional categories
include `lifecycle`, `sessions`, `progression`, `combat`, `survival`,
`movement`, `magic`, `ranged`, `pvp`, `support`, `economy`, `items`, `skills`, `quests`,
`social`, `authentication`, `roster`, `exploration`, and `discoveries`.
Durations use `hours:minutes:seconds`; timestamps are labeled UTC; collections and keyed series
show bounded ID counts. Every use is logged at the administrative system
level. The command never accepts an account name from its arguments and never
shows authentication or network security data.

## Trusted Python content API

The embedded Python `Player` wrapper exposes `QuestStatus(uid, status)`,
`MetricAdd(save_name, amount=1)`, `MetricKeyedAdd(save_name, subject, amount=1)`, and
`MetricMarkUnique(save_name, subject)`.
These are trusted authored-content APIs, not client APIs. They can mutate only
the current character store. `MetricAdd` accepts registered additive counters
and durations; it rejects account, current, timestamp, maximum, and minimum
metrics. `MetricKeyedAdd` accepts only registered character counter series; it cannot set current
or maximum snapshots. `MetricMarkUnique` applies the same identifier validation and bounds as C
callers. Content must invoke them only after the authoritative scripted operation succeeds and
must never use display text as a subject ID. Content-backed subjects passed from Python use the
same domain-qualified IDs described above. A valid additive/unique telemetry request is
best-effort when its bounded series is already full, so instrumentation cannot fail a completed
gameplay transaction; unknown metric names and malformed subjects still raise `ValueError`.

## Analytics boundary

The former UDP datagram and Python Shelve collector have been removed. There
is one authoritative update path. A future analytics exporter must use
authenticated, retryable snapshot or idempotent-delta semantics with a durable
watermark. Analytics outages or retries must never change gameplay totals,
grant rewards, or double-count authoritative state.
