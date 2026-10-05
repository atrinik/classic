# Classic book writing

The writing pen opens an editor for a marked, unsigned inventory book or letter.
The player needs Literacy and Inscription and must be able to see. The pen stays
in the main inventory; it need not be equipped. `/use_skill inscription` directs
players to this editor instead of accepting message text.

## Transactions and inventory

`src/server/book_edit.c` owns the authoritative editor transaction. One volatile
session per connection snapshots the pen and eligible books. Every selection,
save, copy and signature rechecks object tags, inventory ancestry and custody
generations, title, contents, finalization and current prerequisites. Transfer,
removal, changes by scripts and moving an ancestor invalidate a stale submission.
Applying another pen or disconnecting clears the session. Errors preserve the
client draft and change neither the book nor the ink; Cancel changes no item.

Inventory lookup, including marked-object resolution and ink refilling, is
bounded to 32 object levels and 4096 visited objects. Hidden or removed objects
and their subtrees are excluded using the player's `IS_INVISIBLE` policy. Unpaid
books are excluded, and unpaid pens, books and copy sources fail commit checks.
Refill checks both the marked pen and carried bottle before splitting or consuming
anything. Split a stacked pen or destination book into one item first. A stacked
source can be copied because the source is never mutated.

An editor lists at most 64 books. Titles contain 1–127 UTF-8 bytes and contents
at most 2037 bytes, excluding NUL terminators. Titles reject edge whitespace and
line breaks. Contents allow newline/tab but reject loader-unsafe control bytes
and the case-insensitive `endmsg` sentinel. Invalid UTF-8 is rejected.

## Ink

The pen uses `race writing_ink`, `stats.maxhp` as capacity and `stats.food` as
remaining ink. A `LIGHT_REFILL` bottle with the same race supplies its
`stats.food` amount. Mark the pen, then apply a carried bottle. A partial refill
splits one bottle from a stack before changing its remaining ink; a complete
refill consumes one bottle. Other light-refill behavior is unchanged.

Each successful save/copy charges one ink unit per new or replaced UTF-8 byte:
`new_length - longest_common_subsequence(old_contents, new_contents)`. The
comparison is byte-based and bounded by the content limit. Identical text and
deletions cost zero. Renaming and signing cost zero. Insufficient ink rejects
the complete transaction, including its title change, so the player can refill
and retry the retained draft.

## Persistence and authored behavior

Edits change only `name` and `msg`, clearing an existing `custom_name` alias when
it would hide a renamed/copied title. They preserve object identity, archetype,
level, value, quest attributes and inventory script/event objects. The editor
does not apply the book or grant arbitrary object-property access; reading still
uses the normal apply path and its authored hooks.

Signing requires the persisted title and contents, preventing unsaved edits from
being silently finalized. It stores four object key/value fields through normal
object serialization:

| Field | Meaning |
| --- | --- |
| `book_finalized` | Presence permanently locks title and contents, even for an unexpected value. |
| `book_signer` | Authenticated current character name. |
| `book_signed_date` | Server-generated visible in-game date. |
| `book_signed_utc` | Separate real-world UTC timestamp in seconds. |

These fields survive ordinary saves, reloads and transfers. Signed books remain
readable and usable as copy sources. Copying changes only the unsigned
destination's title and contents; it preserves destination attributes and never
inherits source signature metadata. A destination can then be signed separately.
The ordinary `/rename` alias path also rejects finalized books.

## Wire and validation

The coordinated Classic protocol revision is 1081. `BOOK_EDIT` action/result
layouts and bounds live in `protocol/include/atrinik/protocol/book_edit.h`.
Parsers reject truncated fields, unknown actions, illegal source fields and
trailing bytes before any mutation. Existing `BOOK` readers receive one
NUL-terminated formatted title/body string, with two additional NUL-terminated
signer/date strings only for signed books. Both empty and populated quest lists
also emit a terminated `BOOK` string.

The `book_edit` server suite covers edits, copies, signing and serialization,
quest/event attribute preservation, ink costs/refills, stacked items, hidden and
unpaid items, bounded marks, stale custody/content and malformed/replayed packets.
Run through the resolved Classic wrapper profile and the pinned CPU worker.

For an isolated integrated check, the offline `writing-books` scenario preset
provides normal Literacy/Inscription skills, three drafts, a populated source,
full/dry pens and an ink bottle. From the workspace wrapper root, replace `NAME`
and `PROFILE` with owned, task-specific coordinates:

```sh
./atrinik scenario create NAME --profile PROFILE --preset writing-books
./atrinik scenario show NAME --json
./atrinik topology show PROFILE --state scenario-NAME --json
./atrinik up --name NAME --profile PROFILE --state scenario-NAME
./atrinik ps NAME --json
./atrinik logs NAME server --tail 200
./atrinik logs NAME client --tail 200
```

Use the scenario's returned state coordinate if it differs. Observe editor open
and Cancel without ink loss, rename/edit, copy with replacement confirmation,
free signing, rejection of later changes, and dry-pen failure followed by refill
and retry. Save, disconnect and restart the owned topology to verify persisted
title/text and signature metadata; test transfer through the normal inventory
path. Unit serialization and process health alone are not these live observations.
Inspect bounded logs, then stop only the owned topology:

```sh
./atrinik down NAME
./atrinik ps NAME --json
```

Keep scenario credentials and mutable state private. Content help, ink acquisition
and the existing ink-bottle graphic remain owned by `atrinik/content@main` and
its Classic-target artifact.
