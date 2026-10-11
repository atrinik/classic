# Classic physical 3D space

This contract owns server geometry, gameplay perception, targeting and moving
combat effects. [MAP_RENDERING.md](MAP_RENDERING.md) owns aggregate radiance,
MAP2 publication, structural camera disclosure and client composition. The
world remains tiled maps with per-cell objects; it is not a continuous mesh or
a persisted voxel database. Protocol revision 1084 coordinates target identity
with the existing linked-depth MAP2 records.

## Geometry and policies

`src/include/spatial.h` and `src/server/spatial.c` own loaded-only resolution
and integer supercover traversal. One tile and one linked story have equal
geometric size. Coordinates resolve through resident reciprocal tiled links
and their integer transforms. The physical stack admits thirteen levels;
MAP2 advertises relative depths -6 through +6. Viewer clipping is not a bound
on physical lighting or stable cover analysis. Missing, ambiguous, cyclic,
over-bound or unavailable topology fails closed. Resolution never loads a map.

The API runs on the server thread with borrowed map/object pointers and bounded
temporary lookup storage. A retained cursor requires retained map lifetime.
Finite segment deltas admit horizontal components through 4096 and vertical
components strictly within thirteen; directional component ratios admit
magnitudes through 65536 and at most 12288 boundary events. Consumers impose
smaller operation-specific limits where required.

Starting at a cell center, compare boundary times as integer ratios, with
checked wide products. Equal boundary times form one event. Visit every
nonempty subset of tied axes (up to seven cells) and every crossed or grazing
face (up to twelve). A diagonal cannot tunnel through a touching wall, slab or
three-axis corner. Distinct events retain their order; no floating-point
sampling or independent horizontal/vertical ray approximation is permitted.

Sight and combat share events but retain distinct predicates. Sight tests
`P_BLOCKSVIEW`; combat tests closed doors, player-only, no-pass and pass-through
blocking policy. Every vertical crossing checks the upper cell's floor or
hidden wall-layer roof slab. A terminal-cell exception may permit seeing an
opaque target surface, but does not excuse a crossed slab or intermediate
corner blocker. Light tests oriented face coefficients and combines
simultaneous faces by minimum; sequential crossings attenuate sequentially.
Camera hiding/cutaway never changes a physical predicate.

## Gameplay authorization and camera disclosure

`los.c` maintains a separate mask for every advertised relative depth. Refresh
physical sight before use; darkness, hidden/system flags, invisibility and
existing xray/operator exceptions remain separate authorization checks.
`perception.c` checks a multipart actor's live parts and head identity. Player
selection must authorize the exact physical part, not merely a visible head
or a remembered sprite. Retained counts and pointers are revalidated before
use and after script/plugin visibility callbacks.

MAP2 performs the structural-column prepass across the complete resolved
thirteen-level stack, then applies the player's advertised -6..+6 range.
Camera-visible exterior walls and roof silhouettes may be serialized without
actors, items, effects or interior floor faces. Covered wall-only disclosure
suppresses `MAP2_FLAG2_EXIT` even if the underlying wall-layer object is an exit.
Roof-only upper cells count as cover; they do not require `FLAG_IS_FLOOR` or
`P_BLOCKSVIEW`. Remembered geometry grants no target or combat authority.

The player viewport update owns an operation-local coordinate cache: at most
thirteen retained cells per projected column and nineteen checked relative
queries (-6 through +12). A valid result beyond the storage bound uses exact
resolution without retaining it. Equal-sized validated local vertical chains
share transforms inside the origin map; seam coordinates retain full resolution.
The cache is freed before returning and grants no later action authority.

After camera classification, the same synchronous read-only operation may build
per-depth prefix sums for opaque/unresolved cells and floor/roof slabs. Every
integer supercover cell lies inside the closed box between the two endpoints.
A ray is proven clear only when every plane in that box has zero opaque and
unresolved cells, and every upper plane of a crossed vertical boundary has zero
slabs. Same-story rays cross no slab; upward rays do not cross their origin's
floor. Any nonzero sum selects the unchanged exact traversal, including its
corner and terminal-slab rules. Prefix storage is bounded by two `uint16_t`
fields per prefix coordinate on each advertised depth (56,628 bytes at the
32-by-32 compile-time limit). No mutation, plugin call or yield may separate
the snapshot from its use. Fresh physical action checks use current default
traversal and fresh camera checks construct their own scope.

Completed camera classifications and sight masks may be retained across idle
socket posts. Their key includes a process-wide nonwrapping LOS geometry epoch,
origin map incarnation, player position, viewport dimensions and center offsets,
true-line-of-sight mode, blindness and xray state; the existing dirty flag also
forces recomputation. No actor identity, light sample or action authorization is
retained by this cache. Geometry changes in every map schema invalidate it;
ordinary nonblocking actor movement does not. Cell signatures distinguish sight
opacity, any floor, hidden wall-layer roofs, and any wall-layer object. Map
lifecycle and topology changes invalidate absent and conflicting dependencies as
well as existing links. A build whose epoch changes cannot become cache-valid.

MAP2 buffers every level and continuation until serialization finishes. Script
visibility callbacks revalidate observer and actor lifetimes, location and the
view generation before another borrowed value is used. A separate nonwrapping
publication mutation epoch covers mapped-object mutations, including ordinary
actor insertion/removal and Python field/flag assignments. It cannot invalidate
idle geometry masks, but detects a callback changing an already serialized actor.
Native plugins must publish mapped-object mutations through the normal object
mutation/update APIs; raw unsignaled writes are unsupported. Changed geometry or
publication state aborts
the whole pending MAP2 publication, clears its delta cache and target publication
references, and requests a complete map on the next socket post. Exploration
grants are deferred until the same publication fence passes. The final fence and
queue append run synchronously without callbacks or yields.
Visibility callbacks may synchronously log out the observer or another player.
Packet-local buffers are released independently; connection cleanup first checks
live-list membership, controller/socket identity and observer incarnation. The
socket post pass snapshots connection identities and skips deleted or replaced
entries after callbacks, including target-stat authorization before map drawing.
New connections wait until the next post pass. A linked connection remains live
while its avatar is removed or unmapped; spatial activity checks belong to map
and action callers, so connection keepalives and queued output continue.
The incoming target command applies the same post-callback identity fence.
Command dispatch, queued-frame removal and the player action loop revalidate the
connection before resuming after target authorization; destroyed observers leave
no borrowed queue or controller access. Active-tick retained-target refreshes
also fence their callbacks before updating selection or continuing combat.

Test-only counters distinguish sight candidates, camera-pruned cells, coordinate
resolution calls, successful clear-volume proofs and actual ray traversals, plus
cumulative viewport builds and idle cache hits.
The uncached reference bypasses coordinate caching and clear-volume proofs;
regressions compare complete camera and sight masks across seams, negative
depths, slabs, corner blockers, conflicting links and unavailable dependencies.

Monster acquisition scans loaded cells in three dimensions with a horizontal
radius capped at 35 cells and relative vertical range capped at six. This is
an explicit acquisition work limit, not unlimited world search. Existing
sleep/blindness, stealth, friendship and aggro rules still apply. Retained
hidden enemies may drive pursuit; aimed attacks and retaliation must pass
fresh physical perception. There is no promise of vertical pathfinding merely
because perception and ranged combat can cross stories.

## Target protocol 1084

Authored `protocol/include/atrinik/protocol/target.h` owns the layout. Command
and subtype identities remain unchanged. The version handshake rejects older
peers; update native and Python consumers together.

| Direction/action | Payload in field order |
| --- | --- |
| C2S SELECT | subtype `u8`, viewport x `u8`, y `u8`, relative depth `i8`, actor count `u32` (exactly 8 bytes) |
| C2S CLEAR | subtype `u8` (exactly 1 byte) |
| S2C result | type `u8`, color cstring, name cstring, level `u8`, actor count `u32`, relative depth `i8`, combat `u8`, combat force `u8` |

Integers use existing network byte order. SELECT is PLAYING-only and binds a
physical MAP2 part to its head's nonzero actor count; zero is never a wildcard.
Malformed packets preserve state. A valid unauthorized request returns SELF
without disclosing the rejected actor. SELF carries count/depth zero. Parsers
validate the complete payload and reject trailing bytes before mutation.
Server action authority also requires a previously completely queued MAP2
publication, plus fresh perception. Each living sub-layer caches its own head
actor count; a count from another sub-layer cannot authorize an overlapping
part. The socket publication binds the origin map and map count, center x/y,
viewport dimensions and complete depth set. Reject pending publications,
unloaded/recycled origins, unavailable depths, fog/clear cells, missing living
faces, mismatched counts and physical coordinates that no longer resolve to
the cached exact part. Queue the full header and all continuations before
publishing this authority; queuing is not a client delivery acknowledgement.
Withdraw borrowed origin references and clear their caches before map unload,
delete or reuse, including playing, admission and pre-login sockets.

Clicks, retained targets, retaliation, target HP/level metadata, player
projectile aiming and targeted spells use this same publication authority.
After a visibility callback, revalidate observer, target, multipart head and
part counts, identity relationships, location and flags; then check exact-part
publication again. A callback can remove, hide, move or recycle any borrowed
actor, so a pre-callback authorization result cannot be retained as authority.

Reconnect clears target identity. Client clicks and cycling use current live
MAP2 records and resolved sprite ownership; remembered structural records do
not become candidates. Combat revalidates retained identity and authorization.

## Projectiles

An embedded numeric value record owns flight state rather than borrowed map
pointers: version 1, signed three-component Q16 heading bounded by 65536,
three fractions in 0..65536 (cell center 32768), nonnegative remaining range,
and signed launch-relative depth within -6..+6. Heading is normalized by its
largest absolute component, so one movement event spans at most one dominant
tile/story. Remaining range tracks `last_sp` at processing time. Boundary
comparison uses wide integer products; deterministic movement truncates
noncrossed components and rounds the time budget upward.

Inspect every shared tied-cell/face event before actor hits. Wall/slab
reflection flips only contacted axes and consumes the event; movement resumes
outward on the next tick. Actor reflection reverses all three components.
Homing uses fresh projectile sight to re-aim; otherwise it retains heading.
Missing topology cannot be traversed. Stop, pickup and refire reset flight
state. Recoverable arrows persist one `projectile3d` numeric record containing
version, heading, fractions, range and depth; copying preserves values. A
legacy missing record initializes the existing planar direction; a malformed
record fails loading. No runtime pointer or map-lifetime token is saved.

## Propagating effects

Propagation state is cast-owned and transient. Save/unload of any participating
map expires the complete cast, not a subset that could resume from stale
borrowed topology. No cast frontier/visited state is persisted and no save
schema migration is required. Admission reserves the complete bounded volume before effects or costs: at
most 65536 entries per cast and 1048576 entries globally. A fair round-robin
scheduler shares one aggregate 4096-expansion budget per simulation tick and
retains pending work for later ticks. Failed admission creates no partial cast
or charged cast costs; it cannot create an unbounded synchronous flood.

Spell admission reserves capacity before `MEVENT_SPELL_CAST` callbacks or cast
costs. A stack-scoped escrow reserves the complete bounding volume and swarm
children; callback-triggered nested casts have independent admission scopes
and cannot consume the outer reservation. Activate the escrow only after
callbacks return and source/caster/target counts and targeted authorization
are checked again. Cast creation transfers credits without double reservation;
every return path releases unused credits. A failed capacity check produces
no partial cast and cannot run the spell callback before admission. Swarm
credits transfer to child explosions, share the aggregate bound and whole-group
map expiry, and release unused child capacity when the parent ends.

Wave/explosion expansion uses 26 neighbours with shared combat boundary tests.
It may spread around cover through a reachable path; it cannot pass through a
blocked face/corner. Cone expansion also requires the fixed 45-degree angular
constraint on each neighbour step. Expansion remains within launch-relative
depths -6..+6. Cast identity prevents duplicate initial hits without suppressing
existing periodic part damage. A camera-hidden effect follows the same
physical propagation as a visible effect.

## Validation boundaries

Required focused regressions cover loaded/ambiguous seams, vertical slab and
hole sight, two-/three-axis corners, light attenuation order, roof-only cover,
fresh cross-depth perception, exact-part authorization, stale/depth/count
selection rejection and transactional target parsing. Projectile tests cover
numeric copy/save round trips, floor/hole reflection, homing and simultaneous
axis reflection. Effects require around-cover paths, cast deduplication,
periodic damage, aggregate work/memory exhaustion, and whole-cast save/unload
expiry. Lighting requires the algorithm-2 cases in MAP_RENDERING.md.

Run generated protocol checks and affected native CTest suites on the
integrated revision through the pinned wrapper build. Offline unit evidence
alone does not prove playable movement, live presentation, audible behavior or
release hardware qualification. Record those acceptance results separately.
