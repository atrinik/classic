# Item custody provenance

Custody provenance is private server state. It is never included in normal item
packets or player-facing UI. Operators with the existing command permission can
inspect an item in their inventory with `/custody <item>`.

Each observed item lineage has a random persistent `item:` identifier, a first
acquirer, and its most recent relinquisher. A character has an account-scoped,
random persistent actor identifier; this survives character renames and makes a
later character with the same name distinct. The actor identifier is created
only by server code and is not an object key/value or content-script field.

## Semantic boundaries

The first acquirer is set once as part of a successful acquisition into player
custody. The relinquisher is set in the same logical transaction as a successful
ground drop, external transfer, shop return, or sale. Each semantic producer
syncs its journal intent after veto/capacity checks but before split, merge,
insertion, or destruction; provenance changes with the surviving object and the
terminal commit follows. Vetoed pickup/drop events do not reach this boundary.
The private gameplay journal therefore records one committed `item` transaction
for each observed acquisition, relinquishment, transfer, or sale, keyed by the
persistent lineage.

Movement between a player's inventory, open containers, or nested containers is
not a custody transfer. A container and all of its contents retain their own
records when it moves. A partial stack receives an exact quantity slice of its
lineage segments on split/copy. Equivalent gameplay objects may merge even when
their hidden custody fields differ: the server combines their bounded,
deterministic quantity-bearing segments instead of making audit metadata
fragment the inventory. The private `custody_provenance` save field uses
`lineage@quantity|lineage@quantity` and retains up to eight distinct segments;
an attempted merge that cannot fit the bound fails closed. Legacy objects with
only `custody_lineage` are treated as one segment covering their current
quantity. An otherwise untracked object merged with a tracked object is
represented as `legacy:unknown`, preserving the quantity without inventing a
historical lineage.

Currency outputs tagged with `currency:<transaction>` remain merge barriers
until the journal transaction retires the tag. This keeps ambiguous or
restart-reconciled outputs discoverable by their transaction lineage; once
settled, the normal segment merge rules apply.

Objects with no provenance are legacy/unknown. A new observed semantic transfer
assigns a lineage, but never invents a historical acquirer: `first` is set only
when the action actually acquires player custody. Direct trusted persisted
grants use `object_insert_into_reason()` (or the corresponding Python method)
with a bounded semantic reason; starting gear, treasure, quest objectives, and
script grants follow this path. Trusted removals and non-routine destruction use
`object_remove_reason()`. Unpaid shop pickup/return and purely internal
same-player reordering do not masquerade as custody transfer. Destroyed items
retain their final journal evidence but have no new owner.

The runtime object tag and combat/effect owner fields remain unrelated to this
feature. Map and player serialization persist custody fields verbatim; old
records simply omit them and remain unknown. Migration deliberately performs no
backfill because there is no trustworthy historical source. Journal lineage and
transaction IDs remain the stable operator correlation keys, while `/custody`
exposes the private segment string for contributor debugging.
