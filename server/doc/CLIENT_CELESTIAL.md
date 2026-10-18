# Client-computed celestial light (protocol 1085)

Status: follow-up to [CLIENT_LIGHTING.md](CLIENT_LIGHTING.md), which specifies
local light, the wire contract and the privacy principle this document builds
on. Celestial light (sun, moon, sky and starlight) moves to the client too. Like
local light it is always on and the only model: there is no server option, no
per-connection negotiation and no legacy path to fall back to.

## Implementation status

Implemented:

- the shared celestial model in libatrinik (`celestial_model.{c,h}`): solar
  and seasonal tables, brightness and color mixing and lunar evaluation, with
  the server's `celestial_lunar.c` and the model inputs of `celestial_light.c`
  delegating to it (step 1);
- the `CELESTIAL` command and its shared codec, and the client's validated
  slot table (`client_celestial.{c,h}`) (step 1);
- the shared solver in libatrinik (`celestial_solver.{c,h}`): the exact
  integer traversal, sky backtraces, halo resolution, spill coefficients,
  relaxation and publish, behind lattice operations. The server solves its
  gameplay field through operations on its maps, unchanged bit for bit (step
  2);
- `CELESTIAL_STRUCTURE`, the celestial closure and its codec and lattice
  (`celestial_closure.{c,h}`), sent with `CELESTIAL` as one publication; the
  client stages both, publishes them with the MAP2 update that follows, and
  solves its closure (step 2).

Until step 3 lands, base radiance keeps carrying the server's celestial
light: the client solves its closure but does not yet compose the field.
Step 3 removes celestial light from the base, composes the client's field and
deletes the server's display-only celestial code (Rollout below).

## Split of authority

The server keeps celestial authority: game time, region profiles and their
phase mappings, lunar overrides, structure, and the authoritative celestial
field that gameplay darkness gating reads. The client receives what it needs
to compute the same celestial field for its authorized tiles and evaluates it
itself, so presentation can move the sun smoothly within an hour and light
remembered tiles correctly.

## Region inputs

A new server command `CELESTIAL` (schema `game-commands.json`) carries
per-connection profile slots, numbered `0..254` in one byte. A slot is assigned
to each distinct effective region profile of the celestial-v1 maps the MAP2
update shows (the celestial closure, below, once structure lands) and carries only what the solver consumes, never the region's
name or digest. Slot 255 means "no profile": a closure needing more than 255
distinct profiles marks the fragments beyond with it, and the client gives
their cells zero celestial light, failing closed exactly as a missing upper
target does on the server. A slot keeps its number while its profile stays in
the closure.

- day and night linear colors and brightnesses, moon linear color and
  `moon_max`, starlight linear color and strength, and `lunar_period`;
- the effective phases of the current hour and of the next hour: solar hour
  `0..23`, season phase `0..8063` and lunar age `0..lunar_period - 1`, after
  the server has applied the cycle mappings and any active lunar override;
- the start of the current hour in game seconds. The client solves one field
  with the current hour's inputs and one with the next hour's, each with the
  shared solver, and blends their final per-cell scalar and RGB values exactly
  as it blends the base keyframe endpoints: for `p` game seconds since the
  start, clamped to `0..3600`, a value is `c + round((n - c) · p / 3600)` with
  rounding half away from zero, in signed arithmetic. Phases and directions are
  never interpolated, so every client presents the same mid-hour field.

The client never learns epochs, rates or override controls: the server resolves
time to phases, so time authority stays on the server.

Payload, in network byte order like every MAP2 field, encoded and decoded by
the shared `celestial_protocol_write()`/`celestial_protocol_read()`:

| Field | Type | Meaning |
| --- | --- | --- |
| generation | `uint32` | advances with every payload of a connection; pairs it with one `CELESTIAL_STRUCTURE` once structure lands |
| start | `uint64` | start of the current hour, game seconds |
| count | `uint8` | number of slot records, `0..255` |
| per slot: slot | `uint8` | `0..254`, strictly increasing |
| day, night | `3 × uint16` each | normalized linear Q0.16 colors |
| day, night brightness | `uint16` each | Q8.8 brightness `0..1024` |
| moon | `3 × uint16`, `uint8` | linear color, `moon_max` `0..20` |
| starlight | `3 × uint16`, `uint8` | linear color, strength `0..2` |
| lunar period | `uint16` | `168..8064`, a multiple of 24 and of 8 |
| current, next phases | `uint8`, `uint16`, `uint16` each | solar hour `0..23`, season `0..8063`, lunar age `< period` |

A payload is complete state: it replaces every slot, and a slot it omits is no
longer assigned. Decoders reject, before replacing anything, truncation,
trailing bytes, unordered or out-of-range slots, out-of-range phases or
strengths, a day or night brightness above 1024, a lunar period outside its rules, and a nonzero color that is not
normalized (a peak channel other than 65535). The server encodes the effective
inputs of every slot on each MAP2 update and sends a new payload whenever the
encoding differs from the last one sent, so an hour change, a gained or lost
profile, and every lunar override change through `/celestial` reach the client
on the next update, in lockstep with gameplay gating. The payload is sent
immediately before the MAP2 update that shows its profiles. The generation
starts at 1 on every connection and advances with every payload; the client
resets its slots and generation with every connection and rejects a payload
whose generation does not advance.

## Structure inputs and the celestial closure

The server's solver (`celestial_light.c`) reads, per cell: the transmission
class (opaque, glass, grate, open) and aperture flag of the north, east, south,
west and downward faces (the existing 16-bit `celestial_geometry` signature),
the virtual sky cover above the cell, whether an upward exit reaches the sky,
and seam continuity between maps (schema, boundary, reciprocal link, profile
digest). Its rays travel at most 32 cells horizontally and 13 levels, and its
diffuse relaxation spills 4 cells.

The **celestial closure** of a viewer is the set of its authorized cells
inside the current MAP2 window, over all 13 depths, and nothing else. A cell
is authorized while MAP2 sends its content to the viewer, and while it is
remembered fog that keeps receiving light (CLIENT_LIGHTING.md, Privacy);
every other cell, an empty or camera-hidden one included, is outside it.
Structure of a cell the viewer has never been authorized for is never sent, so
there is no initial unauthorized record to choose; an upper level the viewer
has not seen is solved like any cell outside the closure. Remembered tiles
outside the current window keep the celestial field the client last computed
for them; they are not part of the closure. Every cell outside the closure is solved as open
air with an open sky exit. That constant discloses nothing; it makes sun, sky
and spill light near the closure edge approximate instead of exact, and errs
towards light, so outdoor views stay naturally lit at their edges.

The server sends a `CELESTIAL_STRUCTURE` command describing the closure as
fragments, one per map and depth, in network byte order:

| Field | Type | Meaning |
| --- | --- | --- |
| generation | `uint32` | the `CELESTIAL` payload whose slots it references |
| count | `uint16` | number of fragments |
| per fragment: depth | `int8` | `-6..6`, relative to the viewer |
| x, y, width, height | `uint8` each | the bounding rectangle of the map's closure cells in the MAP2 window, nonempty and inside it |
| slot | `uint8` | profile slot of the fragment's map, or 255 |
| runs | repeated | covering the rectangle in row-major order |
| per run: length | `uint8` | `1..255` cells |
| per run: value | `uint32` | the cell's structure value, zero for a cell outside the closure |

A run may continue past the end of a row into the next one but never past the
rectangle; runs must cover it exactly, and fragments of one depth must not
overlap. A cell outside every fragment, or with value zero, is outside the
closure. A fragment is one map: the client solves it as the server solves a
map, with the same halo, so spill across seams follows the server exactly.

Structure value bits (faces indexed 0 down, 1 north, 2 east, 3 south, 4
west):

| Bits | Meaning |
| --- | --- |
| 0–9 | transmission class of face i, two bits each: opaque, glass, grate, open |
| 10–14 | face i carries a door, gate or authored transmission |
| 15 | set on every closure cell |
| 16 | the virtual cover above the cell is open |
| 17 | no cell lies above, and an upward exit reaches the sky |
| 18–23 | continuity of the north, east, south, west, up and down faces |
| 24–25 | class of the static floor or roof of the cell above |
| 26 | that face carries a door, gate or authored transmission |
| 27–31 | reserved, zero |

Bits 24–26 are read only while the cell above is outside the closure. They
are what shades an indoor viewer: the ceiling above the room is cut away from
view, so its cell is not authorized, but it is the authorized room's own
roof. Doors and gates in it count as closed, so no unseen state is disclosed.
Decoders reject truncation, trailing bytes, a run of length 0, a run or
rectangle exceeding its bounds, overlap, a nonzero value without bit 15, a
sky exit that also continues upward, and reserved bits, before replacing the
closure. The fragment's slot selects the model that solves its cells, as the
server selects a model per map.

A run costs 5 bytes, and runs compress the open air that dominates upper
levels: a 32×32 window filled over all 13 depths encodes in 409 bytes. A
closure too large for one command loses its farthest depths, which the client
then solves as open air; no continuation framing is needed.

The two commands are one publication, and it belongs to the MAP2 update that
follows it. The server advances the generation whenever either payload changes
and sends both, with that generation, immediately before the MAP2 update whose
window and sent-object set they describe. The client stages each validated
payload and replaces its slots and closure together only when it holds both
with the same generation and the next MAP2 update has been applied
transactionally, so neither the closure nor the map cache is ever presented
with the other's window. A rejected payload, or a MAP2 update that fails to
apply, discards the staged pair and keeps the previous published pair until a
complete matching pair and its update arrive.
A fragment of a schema-0 map always carries slot 255: the server computes no
celestial field for legacy maps, so neither does the client.

The server recomputes the disclosed value of every closure cell on each MAP2
update and sends a new pair whenever the closure or any value differs from the
last one sent: when the closure moves with the player, and when a structure
revision, door or gate, or a change in which objects MAP2 sends to this viewer
(seeing invisible, direction filtering or plugin visibility) changes a
currently visible cell.

Disclosure rule: a cell's faces are built only from the objects MAP2 sends to
this viewer, with the same visibility, hidden-object and plugin filtering as
occluders, and a sent view blocker making its unauthored cardinal faces
opaque. Two static faces are added: the cell's own floor, which the camera
may leave undrawn on a boundary level, and the floor or roof of the cell
above (bits 24–26), each from every object the viewer may see, with doors and
gates closed or left out. The server's own cached `celestial_geometry`, which
aggregates every object of the cell and falls back to the space-wide
`P_BLOCKSVIEW`, is never serialized, so a hidden wall, gate or door contributes
nothing and its state never reaches the client. A structure record changes
only for currently visible cells.
A remembered cell keeps its entire record (signature, sky cover, sky exit,
continuity and door or gate coefficients) as it was when last visible, until
it is visible again, so neither an unseen door opening nor a wall or floor
built or removed out of sight reveals activity or timing. A structure revision
of a remembered cell sends nothing.
The record never names objects, creatures, items or light sources, and closure
cells carry no light of any kind. Seam continuity is static authored data; the
server autoloads the closure's maps with the same bounded autoload the MAP2
view uses, so a missing map reflects a load failure, not another player's
activity, and is reported as discontinuous on both the server and the client.

## Client solver and conformance

The celestial model and solver move to libatrinik (`celestial_model.{c,h}`),
shared by the server and the client: the solar elevation and azimuth tables,
season factors, brightness and color mixing, lunar evaluation, ray transport
with per-crossing rounding, aperture spill, diffuse relaxation and the final
color publish. The server's solver delegates to it, reading its structure from
maps; the client runs it on the received closure grid. Given the same inputs,
both produce bit-identical fields.

The test reference is the server solver run on the disclosed closure, with
cells outside it as open air and remembered doors at their remembered state;
the client must match it bit for bit. It runs on the server's real maps and
links: topology, continuity, sky exits and authored cover come from the maps,
the faces the viewer was shown from the disclosed records, and every cell
outside the closure, or no map resolves, is open air under an open sky.

The client solves a publication a few milliseconds a frame, one fragment and
both hours at a time; a newer publication supersedes the rest. Each cell's
field lives in its map cache header, which scrolls with the map and survives
fog, so a cell keeps its last field until it is solved again. An optimized
build solves one hourly field of a full 32×32 window over 13 depths in about
29 ms, a 17×17 one in about 11 ms; authored Brynknot views of 300–900 closure
cells take a few milliseconds. Against the gameplay field, the client
differs only where sun, sky or spill light reaches an authorized tile through
cells outside the closure or through a remembered door that has since changed.

The client adds its celestial field to the signed base before it combines the
base with local light (client step 3 of CLIENT_LIGHTING.md), for visible and
remembered tiles alike. The base no longer contains celestial light, so the server never samples the celestial field in
MAP2 for display: it stops calling `celestial_light_keyframe_request()` and
reading celestial display values. Gameplay darkness gating is unchanged and
stays fail-closed: MAP2's per-cell gate keeps calling `map_get_darkness()`,
which still obtains the current celestial generation synchronously through
`celestial_light_ensure()` before any live object is serialized, so a field
invalidated by a door, structure revision, profile or lunar override is never
read stale.

## Remote samples

The one exception is a sub-layer whose light is sampled at another space than
its own cell, such as a magic mirror showing a remote place, because the
client's celestial field covers only its own closure. Such a sub-layer is sent
as a complete remote sample: its scalar and its explicit RGB words (the
existing per-sub-layer RGB extension, which only remote samples keep once the
base RGB words retire) carry that space's celestial radiance too, and the
presence of the explicit RGB words marks it. The client composes no closure
celestial field onto a remote sample; it still composes local light at the
cell.

## Celestial budget

Structure is static between changes, and row runs compress the open air that
dominates upper levels. The client keeps the published closure (4 bytes per
cell of its fragments' rectangles) and 16 bytes of solved field in every map
cache header: the scalar and RGB words of the current and next hour.

## Privacy

The privacy principle of CLIENT_LIGHTING.md applies unchanged: the client
receives only what the viewer is entitled to see.

- **Celestial:** region light constants and effective phases (no epochs or
  identities), and the structure of authorized cells in the current window,
  built only from objects sent to the viewer, with door and gate state current
  only for visible cells and frozen on remembered ones.

Celestial lighting also discloses the static celestial structure (walls,
roofs, floors and their transmission classes) of authorized cells in the
current window, built only from objects sent to the viewer. Cells the viewer
has never been authorized for are never described and are solved as open air,
so sun, sky and spill light near the closure edge are approximate rather than
exact.

## Rollout

1. Shared model and lunar evaluation in libatrinik with the server delegating
   to it, and the `CELESTIAL` command with the client's slot table.
2. `CELESTIAL_STRUCTURE`, the celestial closure and the shared solver, with
   the client solving the closure, qualified bit for bit against the server
   solver.
3. Base radiance excludes celestial light; the client composes its field, and
   the server's display-only celestial code is deleted.

## Test plan

- `CELESTIAL` round-trips every slot id; encoders and decoders reject the same
  malformed payloads; a payload replaces the whole slot table; stale
  generations and malformed payloads keep the previous state; a new connection
  resets slots and generation; the server sends a payload only when its
  encoding changes (step 1).
- the shared solver reproduces the server's gameplay field bit for bit
  (golden digests of every Brynknot stack at six times of day); the client's
  solve of the received closure matches the server solver on the disclosed
  closure bit for bit on synthetic outdoor, glass, grate, door, gate,
  aperture and roofed multi-level fixtures, where it also equals the gameplay
  field, and on Brynknot outdoor, indoor, upper-level, door and gate views; a
  visible door's change sends the next pair, an unchanged view sends none, an
  invisible object contributes nothing, and a remembered door that changes out
  of sight keeps its record and sends nothing; the client publishes only a
  matching pair after its MAP2 update, discards it when the update fails, and
  gives slot 255 zero celestial light (step 2).

## Decisions recorded

- Celestial light moves to the client, with server authority over time,
  profiles and gameplay.

- The celestial closure is the authorized cells in the current window; cells
  outside it are open air, structure values come only from sent objects, and
  remembered cells keep their whole remembered record.

- Profile slots are 8-bit with slot 255 failing closed; one atomic
  `CELESTIAL_STRUCTURE` holds the whole closure.
- Celestial light is always on with no option, negotiation or legacy path.
