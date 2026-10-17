# Client-computed celestial light (protocol 1085)

Status: follow-up to [CLIENT_LIGHTING.md](CLIENT_LIGHTING.md), which specifies
local light, the wire contract and the privacy principle this document builds
on. Celestial light (sun, moon, sky and starlight) moves to the client too. Like
local light it is always on and the only model: there is no server option, no
per-connection negotiation and no legacy path to fall back to.

## Implementation status

This pull request implements:

- the shared celestial model in libatrinik (`celestial_model.{c,h}`): solar
  and seasonal tables, brightness and color mixing and lunar evaluation, with
  the server's `celestial_lunar.c` and the model inputs of `celestial_light.c`
  delegating to it;
- the `CELESTIAL` command and its shared codec, sent to every connection
  before each MAP2 update whose shown profiles' encoding changed, and the
  client's validated slot table (`client_celestial.{c,h}`).

Until the remaining steps land, base radiance keeps carrying the server's
celestial light, so presentation stays complete and the client does not yet
compose its own celestial field. The remaining steps of this design are
`CELESTIAL_STRUCTURE` and the celestial closure, the shared ray and relaxation
solver with the client solving the closure, base radiance excluding celestial
light, and the deletion of the server's display-only celestial code
(Rollout below).

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
| day, night brightness | `uint16` each | Q8.8 brightness |
| moon | `3 × uint16`, `uint8` | linear color, `moon_max` `0..20` |
| starlight | `3 × uint16`, `uint8` | linear color, strength `0..2` |
| lunar period | `uint16` | `168..8064`, a multiple of 24 and of 8 |
| current, next phases | `uint8`, `uint16`, `uint16` each | solar hour `0..23`, season `0..8063`, lunar age `< period` |

A payload is complete state: it replaces every slot, and a slot it omits is no
longer assigned. Decoders reject, before replacing anything, truncation,
trailing bytes, unordered or out-of-range slots, out-of-range phases or
strengths, a lunar period outside its rules, and a nonzero color that is not
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
inside the current MAP2 window, over all 13 depths, and nothing else.
Structure of a cell the viewer has never been authorized for is never sent, so
there is no initial unauthorized record to choose; an upper level the viewer
has not seen is solved like any cell outside the closure. Remembered tiles
outside the current window keep the celestial field the client last computed
for them; they are not part of the closure. Every cell outside the closure is solved as open
air with an open sky exit. That constant discloses nothing; it makes sun, sky
and spill light near the closure edge approximate instead of exact, and errs
towards light, so outdoor views stay naturally lit at their edges.

The server sends a `CELESTIAL_STRUCTURE` command describing the closure as
fragments, in network byte order:

| Field | Type | Meaning |
| --- | --- | --- |
| generation | `uint32` | the `CELESTIAL` payload whose slots it references |
| count | `uint16` | number of fragments |
| per fragment: depth | `int8` | `-6..6`, relative to the viewer |
| x, y, width, height | `uint8` each | rectangle within the MAP2 window, nonempty and inside it |
| slot | `uint8` | profile slot of the fragment's map, or 255 |
| runs | repeated | covering the rectangle in row-major order |
| per run: length | `uint8` | `1..255` cells |
| per run: value | `3` bytes | the cell's 24-bit structure value |

A run may continue past the end of a row into the next one but never past the
rectangle; runs must cover it exactly, and fragments of one depth must not
overlap. A cell outside every fragment is outside the closure. The structure
value is the 16-bit signature, bit 16 sky cover open, bit 17 sky exit, bits
18–21 continuity of the north, east, south and west faces to the neighboring
cell (bits 22–23 the up and down faces), with reserved bits zero. Decoders
reject truncation, trailing bytes, a run of length 0, a run or rectangle
exceeding its bounds, overlap and reserved bits, before replacing the closure.
The fragment's slot selects the model that solves its cells, as the server
selects a model per map. The worst case is the 17×17 window over 13 depths,
3,757 cells. A cell costs at most 4 bytes as a run and, in the worst case of
one-cell fragments, 6 more bytes of fragment header, so a complete closure is
at most about 38 KB and always fits in one command; no continuation framing is
needed. The rollout's qualification measures this bound together with the
client's memory.

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

Disclosure rule: a cell's structure value is built only from the objects MAP2
sends to this viewer, with the same visibility, hidden-object and plugin
filtering as occluders. The server's own cached `celestial_geometry`, which
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
the client must match it bit for bit. Against the gameplay field, the client
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
dominates upper levels. The client keeps 3 bytes per closure cell; the worst
case is 3,757 cells, about 11 KB stored and 15 KB on the wire, and typical scenes are far smaller. The rollout qualifies this against MAP_RENDERING.md budgets before
the celestial step merges.

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
   to it, and the `CELESTIAL` command with the client's slot table (this pull
   request).
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
- the libatrinik model matches the server's model for all solar
  hours, season months and lunar ages; the client solver on the encoded closure
  matches the server solver bit for bit on outdoor, roofed, glass, grate, door,
  aperture-spill and multi-level fixtures; remembered cells keep their whole structure record when another player builds,
  removes or opens there, checked with two clients; cells outside the closure
  solve as open air; mixed-profile closures select each fragment's slot, and
  slot 255 gives zero celestial light; region slots carry no digest or epoch. (steps 2 and 3)

## Decisions recorded

- Celestial light moves to the client, with server authority over time,
  profiles and gameplay.

- The celestial closure is the authorized cells in the current window; cells
  outside it are open air, structure values come only from sent objects, and
  remembered cells keep their whole remembered record.

- Profile slots are 8-bit with slot 255 failing closed; one atomic
  `CELESTIAL_STRUCTURE` holds the whole closure.
- Celestial light is always on with no option, negotiation or legacy path.
