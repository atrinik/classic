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
