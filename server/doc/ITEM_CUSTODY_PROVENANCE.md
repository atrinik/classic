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

The first acquirer is set once after a successful pickup into player custody.
The relinquisher is set after a successful ground drop, shop return, or sale.
Vetoed pickup/drop events do not reach these boundaries and therefore do not
mutate provenance. The private gameplay journal records one committed `item`
transaction for each observed acquisition, relinquishment, or sale, keyed by
the persistent lineage.

Movement between a player's inventory, open containers, or nested containers is
not a custody transfer. A container and all of its contents retain their own
records when it moves. A partial stack inherits its lineage on split/copy; a
merge is allowed only when every custody field matches, so mixed history is
never discarded.

Objects with no provenance are legacy/unknown. Loading, moving, or dropping a
legacy object never invents historical first custody. Direct trusted semantic
grant code must call `object_custody_acquire()` after its insertion succeeds;
this includes starting gear, treasure, quest/auction/post delivery, scripts,
and generated currency. Unpaid items, completed shop returns/sales, death-drop
paths, and destruction rules must use the same successful-transfer boundary;
destroyed items retain their last journal evidence but have no new owner.

The runtime object tag and combat/effect owner fields remain unrelated to this
feature. Map and player serialization persist custody fields verbatim; old
records simply omit them and remain unknown. Migration deliberately performs no
backfill because there is no trustworthy historical source.
