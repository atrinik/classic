# Client-computed lighting (protocol 1085 proposal)

Status: design for protocol 1085. Client-computed lighting is the only
lighting model: it is always on, with no server option, no per-connection
negotiation and no legacy full-radiance path. Until the rollout below lands,
[MAP_RENDERING.md](MAP_RENDERING.md) remains normative for the parts this
document does not replace.

## Problem

The server computes all lighting: local source masks traced through 3D
spatial rays, RGB accumulation, and celestial fields. It then samples
`map_get_darkness()` per visible cell, depth, and sub-layer for every player
every tick and sends Q5.11 radiance. That has three costs:

- **Server time.** Rebuilds run on the game thread and stall it (seconds while
  walking Brynknot on 5.99.0). The scalar, positive and color masks of a source
  trace identical rays, and MAP2 can trigger synchronous celestial rebuilds.
- **Presentation quality is server-bound.** The client interpolates one sample
  per cell vertex. It cannot shade buildings, roofs, or cutaways with more
  detail than the server grid. It also cannot animate light, such as flicker or
  the sun's movement within an hour, without new packets.
- **Disclosure is coarse but uncontrolled.** Radiance is aggregate-only, but at
  Q5.11 per cell with RGB, a client can triangulate a hidden source's tile and
  hue from the gradient around a wall.

## Goal and principle

Lighting should look as good as possible, so the client gets as much
information as it can have, while the server keeps authority and does not give
everything away. Concretely:

- It is fine to disclose light from just around a corner, as tile light levels.
- It is not fine to disclose creatures, or the current light of objects behind
  walls (source identity, position, count, radius or hue).
- The **privacy boundary** is the set of tiles already sent to this client: the
  existing LOS code plus the per-socket MAP2 cache, which is cleared on major
  map changes. A tile inside it is *authorized*, whether it is currently
  visible or remembered (fog of war) on the same map while that cache is valid.

The client computes the presentation lighting of everything it can already see,
on the GPU. The server sends:

1. **Signed base radiance** for every authorized tile: ambient, floor and world
   light plus the viewer's own `tli` override, in exact signed Q4.11 with RGB.
   It describes the environment, not a source the player could locate.
   Negative environment (darkness floors) is carried directly, so the client
   composes darkness everywhere.
2. **Emitters of sent objects**: glow radius, flicker opt-in and authored color,
   sent only with an object that MAP2 already sends. Its existence and tile are
   already disclosed by its face, so the emitter adds no location information.
3. **Coarse local light** for every authorized tile: per occupied sub-layer, a
   signed, quantized, neutral level of local light, with sources pooled per
   fixed 3×3 block before they propagate. Light and darkness from hidden
   sources bleed through walls without identifying the source tile, count or
   hue. A remembered tile keeps the levels it last had while visible.
4. **Occluders of sent cells**: geometry bits plus quantized transmission of
   each cardinal face and the downward face, derived only from objects MAP2
   sends, and face bits from static seam structure between authorized cells
   only.
5. **Celestial inputs** (see Celestial lighting): the region profiles and
   effective phases of the current and next hour, and the static celestial
   structure that the server's celestial solver reads for authorized tiles. The
   client evaluates sun, moon, diffuse sky and starlight itself.

Gameplay keeps using the full authoritative server light. That includes
darkness gating of live objects in MAP2 (`raw_light <= 0`), infravision,
stealth, `tli`, XRAY and the celestial field. Nothing in this design moves
gameplay authority to the client.

### What is exact and what is coarse

Everything that does not identify a hidden source is sent exactly: base
radiance, sent emitters, sent occluders, celestial inputs and structure.
Only light of local sources the viewer cannot see is coarse. Exact per-tile
local light on authorized tiles was considered and rejected: the per-cell
gradient around a wall locates the hidden source, which is exactly the
disclosure this design removes (see Problem). Remembered tiles keep the coarse
levels they had while visible and gain client-computed celestial light, which
needs no live information about the remembered area.

## Wire contract (protocol revision 1085)

`protocol/schema/game-commands.json` advances `protocol_version` to 1085. Peers
below 1085 are rejected at setup, as for every revision.

Protocol 1085 means client-computed lighting; there is nothing to negotiate.
Every 1085 connection receives signed base radiance and the client-lighting
fields, and every 1085 client composes them. There is no SETUP subtype,
capability byte or server option for it, and no way for a 1085 peer to receive
the former full-radiance meaning. Reconnect clears all client lighting state
with the map cache.

Per MAP2 cell record (one record per cell and depth). The fields follow the
existing extended-tile data, in this order, after RGB, keyframe and animation
data. They are announced by `ext_flags` bits, because the `MAP2_MASK_*` cell
word already uses bits 6–15 for coordinates:

| Field | Bytes | When | Meaning |
| --- | --- | --- | --- |
| `MAP2_FLAG_EXT_LOCAL_LIGHT` (8) | 1 + n | changed | `uint8` count `n` of `1..7`, then `n` level bytes: level `i` applies to sub-layer `i`, and sub-layers from `n` on use level `n - 1`. Each level byte: bits 0–4 signed level `-15..15` (two's complement; `-16` invalid); bits 5–6 reserved, zero; bit 7: apply the viewer's vision floor after composition |
| `MAP2_FLAG_EXT_OCCLUDER` (16) | 5 | changed | `uint16`: bit 0 blocks view, bit 1 floor, bit 2 roof surface, bit 3 closed door; bits 4–7 reserved, zero; bits 8–11: local light does not cross the north, east, south or west face; bits 12–13: nor the upward or downward face; bits 14–15 reserved, zero. Then `uint16`: attenuation `a` of the north, east, south and west faces, one nibble each from bit 0. Then `uint8`: bits 0–3 vertical attenuation `v` of the downward face; bits 4–7 reserved, zero |

Attenuation `a` and `v` share one scale: 0 is unattenuated, 1–14 is the
coefficient `256 - 16n`, 15 is opaque. A view blocker with authored
transmission keeps its attenuation. `a` applies to horizontal crossings of the
cell; `v` applies to crossings of its downward face (its floor or roof slab),
in either direction, so a glass floor transmits vertically while an ordinary
floor stays horizontally open.

The `ext_flags` value 32 (formerly the aggregate bitmap) is unused in 1085 and
rejected.

Per MAP2 layer (existing `flags2`):

| Field | Bytes | Meaning |
| --- | --- | --- |
| `MAP2_FLAG2_EMITTER` (2048) | 1 + 1 + 3 | signed `glow_radius` as stored (`int8`, 0 invalid); emitter flags: bit 0 flicker opt-in, bits 1–7 reserved, zero; then authored sRGB `RRGGBB`: nonzero for a positive radius, `000000` for a negative one |

The emitter payload is the last optional layer field: it follows the ALPHA,
ROTATE, ZOOM, TARGET, PROBE and GLOW payloads in that order, and precedes the
next layer record. Radii are not clamped on the wire, because the server sums a
tile's radii before mapping the sum through `light_mask`, which saturates at
±13; clamping each emitter first would change that sum. Flicker is allowed
only on a positive radius; darkness never flickers.

### Signed base radiance

Every per-sub-layer scalar and RGB lighting word is a **signed** two's
complement `int16`, signed Q4.11 on the existing scale (2048 is raw 1280,
daylight). It carries environment only: no local source, no XRAY floor and no
celestial light, which the client computes itself (see Celestial lighting). Encoding from the server's raw value `r`:

- `q = sign(r) · floor((|r| · 8 + 2) / 5)`, the existing Q5.11 rounding applied to the
  magnitude, so `Q(-r) = -Q(r)`;
- saturating symmetrically to `-32767..32767` (raw ±20479, 16× daylight).
  The server never encodes `-32768`; decoders read every word as a plain
  `int16` and accept it.

Conformance vectors: raw `0 → 0`, `1 → 2`, `-1 → -2`, `160 → 256`,
`1280 → 2048`, `-1280 → -2048`, `20479 → 32766`, `20480 → 32767`,
`-20480 → -32767`. Current world, map and celestial anchors stay within raw
±1280, far inside the range.

Because the encoding is odd-symmetric, environment and composed light of equal
magnitude cancel to exactly zero, as the server's single raw sum does; other
sums differ from the server's single rounding by at most one Q4.11 unit per
channel. The scalar and each channel are encoded independently, and a negative
channel is meaningful until composition finishes.

### Budgets

MAP_RENDERING.md caps per-sub-layer *lighting samples* at 8 bytes and the
client and per-socket lighting storage at 56 bytes per cell. Once celestial
light is computed on the client (rollout step 3), the base is environment only,
which is neutral (map ambient, `tli` and darkness), and its next-hour endpoint
has no celestial change left to interpolate: the base RGB words and the
timed-light next endpoints retire, and the base keeps one signed scalar per
sub-layer. A fully populated cell then stores 14 bytes of base, 7 bytes of
coarse levels and 5 bytes of occluder, 26 bytes of the 56. The intermediate
stack steps are never released on their own (see Rollout), so no shipped build
exceeds the cap. Emitters are object attributes, not lighting samples: their 5 bytes
ride on the layer record of the object they describe, within the unchanged
complete-MAP2 and continuation limits. The client stores emitters in one packed
per-cell array of 6-byte entries (layer, radius, flags, RGB), allocated only
for emitting cells and bounded by the cell's sent layer records, so it scales
with object storage rather than the lighting budget. The per-socket cache adds
25 bytes per cell (see Server). Celestial structure has its own budget (see
Celestial lighting). The rollout's final step records these figures in
MAP_RENDERING.md.

### Validation

Decoders reject, before any state is published, exactly as for other
malformed MAP2 fields: an emitter radius of 0; a positive radius with color
`000000` (a zero linear peak, which MAP_RENDERING.md rejects for positive
sources); a negative radius with a nonzero color (darkness is achromatic) or
with the flicker bit; a local level of `-16`; `ext_flags` value 32; and any
nonzero reserved bit. The base words have one meaning on every 1085
connection.

## Server

- **Gameplay light only.** The server keeps authoritative raw light for
  darkness gating, infravision, stealth, XRAY and `tli`: the scalar local mask
  and the celestial solver's scalar. Display-only code (positive and color
  masks, display RGB, timed-light display endpoints) has no consumer once the
  client computes celestial light, and is deleted in that step.
- **Base radiance** is the environment term of each sub-layer: raw light minus
  the local source value and minus the celestial value, encoded as signed
  Q4.11. The existing `tli` override is
  folded in per viewer, as today. The XRAY floor is not applied (see Vision
  floor). There is no aggregate fallback: negative environment is sent as
  negative base.
- **Coarse local light** is derived each time MAP2 sends a visible cell, from
  the already maintained per-space source counters. No new per-map state is
  needed. Each sub-layer takes the level of the space its light was sampled
  at, which can differ between sub-layers; the server sends the shortest level
  count describing them:
  - the block is the fixed 3×3 square containing the cell, aligned to map-local
    coordinates (`x - x % 3`, `y - y % 3`). A remainder of one or two cells at a
    map edge joins the adjacent block instead of forming a smaller one, so no
    block is narrower than three cells unless the map itself is;
  - pool the scalar source counters of each block and resolve the sum through
    the same mask table, before any propagation. The pooled source of a block
    reaches another block by the gap between the two blocks in cells (0 for
    the own block, 1 for an edge neighbor, `√2` for a corner one), which
    depends only on block geometry. The own block and its eight neighbors are
    within the largest radius. Neighbors across a seam light crosses (a
    resident reciprocal link, and continuity as `light_face_continuous()`
    requires) count too, on maps that match along the seam so blocks align, and
    so do blocks at the same position on such linked levels above and below,
    adding the vertical distance as the server's masks do. No map is ever
    loaded for it. A source anywhere inside a block therefore
    gives every block the same level, so neighboring blocks cannot
    triangulate its tile;
  - the strongest positive and the strongest negative pooled contribution
    compete; the larger magnitude wins;
  - quantize the magnitude down to 1/8-daylight levels (raw 160), clamp to 15,
    and keep the sign.
- **Remembered tiles are frozen.** When base LOS loss turns an authorized tile
  into fog, the server keeps its per-socket cache entry marked as remembered
  and keeps its coarse levels at the values last sent while it was visible,
  so a client cannot follow a hidden source through fog. Only the viewer's
  own vision-floor bit is refreshed, in a layerless record. A tile never seen
  has level 0. The client keeps the tile's last base radiance and computes its
  celestial light itself. This continues while the per-socket cache is valid;
  a map change, hard clear or reconnect ends it. Remembered tiles never
  receive objects, emitters or occluder changes: those describe objects, and
  the objects on a remembered tile are not currently sent.
- **Vision floor.** XRAY raises presentation light to raw 100 after all light
  is combined. The server sends base radiance without that floor and sets bit 7
  of the local-light byte for viewers with XRAY; the client applies the same
  floor after composition. When bit 7 is set and the composed scalar `S` is
  below raw 100, the client adds `100 - S` neutrally to the scalar and to every
  channel, exactly as the server used to raise raw light before resolving color.
  Conformance vector: scalar/RGB `(80, 80, 0, 0)` becomes `(100, 100, 20, 20)`.
  This is a property of the viewer only, so it discloses nothing about sources.
- **Emitters** are copied from `glow_radius`, `light_color` and the
  `light_flicker` object attribute (archetype default, boolean) of the object
  MAP2 is already sending (the object on that tile, including the viewer's own
  derived emitter). Another player's emitter is never sent: it is derived from
  the strongest eligible item in that player's private inventory, so its exact
  radius and color would disclose inventory state that item packets show only to
  the owner. Other players' light reaches the viewer only through the coarse
  level. A negative radius is sent with color `000000` and no flicker. The
  per-socket cache keeps, per cell, a bitmap of layers that carry an emitter, a
  bitmap of stale emitter layers and a digest of the cell's emitters (20 bytes).
  Gaining or losing an emitter forces a layer delta; a radius-, flag- or
  color-only change on a cached layer changes the digest and resends that layer
  on the next update. Emitters of objects not sent, whether hidden, out of
  sight, dark-gated, plugin-hidden, remembered or below the top object of a
  layer, are never sent.
- **Occluders** are derived only from the objects MAP2 sends to this viewer:
  the sent object of each layer, after plugin visibility and hidden-object
  filtering. A sent view blocker, floor, roof surface or closed door sets its
  bit. Authored transmission gives the coefficients: doors and exact-type gates
  use `celestial_transmission_closed` or `celestial_transmission_open` by their
  current state (`FLAG_DOOR_CLOSED` for doors, `FLAG_NO_PASS` for gates), other
  objects `celestial_transmission`. Each cardinal face has its own `a`, set by
  the objects authoring that face, and objects with a downward face set `v`;
  each is the maximum attenuation over the sent objects (the minimum
  coefficient, as the server's face traversal takes), quantized up to the next
  step. A face a sent view blocker does not author is opaque, and other
  unauthored faces stay open, as in `spatial_face_light()`. A client step
  leaves its cell through the exit face of its direction. A tied diagonal step
  crosses every face the shared supercover event emits: the source cell's two
  exit faces and both grazing side cells' faces towards the destination, and
  the smallest of their coefficients applies (a three-axis tie likewise takes
  all twelve), so the client never transmits more light than the server. Space-wide flags such as `P_BLOCKSVIEW`
  are never used, because they aggregate hidden and unsent objects too. Unsent
  geometry therefore contributes nothing, and the client may light past a
  blocker it cannot see; that errs towards disclosing less.
- **Face bits** encode only static authored seam continuity, and only for faces
  between two cells that are both currently in the viewer's view this update
  (the face's neighbor at the same depth, or at the adjacent depth for the
  upward and downward faces). A face is open when both cells are in view and
  either lie on the same map, or the seam is continuous as
  `light_face_continuous()` requires: two schema-0 maps are continuous across
  any reciprocal link, as MAP_RENDERING.md freezes for legacy maps, and two
  celestial-v1 maps are continuous when both boundaries are authored
  continuous, the link is reciprocal and the effective profile digests match. The
  upward face is also closed under a sealed virtual sky cover. Every face
  leading to a remembered, unauthorized or out-of-view cell is always blocked.
  Both cells in view are resident by construction, so the bits never depend on,
  and never encode, whether some other map is loaded.
- **No per-player light recompute.** MAP2 reads cached base radiance and
  per-cell scalars. It does not call the celestial ensure or keyframe paths at
  all (see Celestial lighting).

## Client

The GPU light pass gains an emitter stage before the existing tone/LUT stages:

1. Group the visible emitters of a tile exactly as the server groups a tile's
   counters. The scalar and positive terms sum the radii of the tile's sent
   emitters and map each sum through `light_mask`, which reproduces the
   same-cell cap. Color keeps every distinct constituent (color and profile)
   with its exact server weight, resolved against the grouped positive term
   exactly as the server resolves its color accumulators, so hue can vary with
   distance. Constituents that resolve to one profile share its value at every
   distance and its rays, so they fold exactly into one walk with their summed
   color: a tile costs at most one walk per profile, however many emitters it
   carries. When some of a tile's emitters are not sent,
   the client underestimates and the coarse level supplies the rest. The
   occluders are, per depth, an R16 grid of geometry and face bits (bits 8–13
   must reach the ray walk), a second R16 grid of the four horizontal face
   attenuation nibbles, and an R8 vertical-attenuation grid. The CPU reference
   packs the two words of a cell into one 32-bit value.
2. A compute (or full-screen fragment) pass evaluates every light vertex of the
   existing compact light rows. It sums emitter profiles using the same radial
   tables as the server (`light_profiles`, radius ≤ 4, vertical ≤ 4) with a 3D
   DDA through the occluder grid, so it follows the same "stop after the exposed
   face" and floor-crossing rules as `light_path_value()`. Light attenuates as
   it leaves each cell, the source cell included. Horizontal steps apply the
   `a` of the face they leave by, vertical steps apply `v` of the upper cell of
   the crossing, and a step across a face marked closed stops the ray: bits
   8–11 for horizontal steps and bits 12–13 for steps to the linked level above
   or below. A vertical step that terminates on a visible floor or roof surface
   skips that receiving slab's `v`, under the same terminal-surface predicate as
   `light_path_value()`, so the exposed surface is lit; source-side virtual
   cover still applies.
3. Combine the client emitter sum `E` with the coarse block level `C` (raw
   step 160), which is a bleed hint for the whole block:
   - `C > 0`: `L = max(E, 160C)`; `C < 0`: `L = min(E, 160C)`; `C = 0`: `L = E`;
   - then limit `L` to the block's level: at most `160(C + 1) - 1` when `C >= 0`
     and `C < 15`, and at least `160(C - 1) + 1` when `C <= 0` and `C > -15`.

   `C` is the block's extremum, not a per-cell bound. Cells of a block that are
   authoritatively darker than its brightest cell, such as the unlit side of a
   wall, are presented at the block's level. That is the intended bleed of
   hidden light through walls. It is presentation only: gameplay darkness
   gating uses each cell's own server light, so such a cell can look lit while
   its dark-gated live objects remain unsent.

   The emitter sum carries hue as signed per-channel deltas relative to its
   scalar, so overlapping light and darkness keep the server's color shift; the
   coarse term is neutral. `E`, `C` and `L` are raw light. `L`, with each
   channel's hue delta, converts to radiance with the server's rounding applied
   to its magnitude, `sign(r) · floor((|r| · 8 + 2) / 5)`, without saturating.
   The composed radiance is, per scalar and channel, signed base + celestial
   radiance + converted `L`, all in Q5.11 units in unbounded signed arithmetic,
   so the coarse and emitter terms keep their full weight. Negative channels then clamp to zero; if the largest channel then
   exceeds the finite range, all three channels scale together so the strongest
   is 65535, as the server's common-vector overflow rule requires, keeping hue.
   The scalar saturates at 65535. The vision floor (if bit 7 is set) follows.
4. **Player field.** After composition, and after the vision floor, the
   existing independent local-player presentation field (radius 8, center
   strength 80, neutral) is added exactly as MAP_RENDERING.md specifies today:
   primary depth only, only on currently authorized visible tiles (never on
   fog), with its existing clipping and before the window fade. It does not
   depend on the viewer's emitter, so an unlit player keeps it. Then the
   unchanged unsigned Q16 tone/LUT stage runs; negative intermediate values
   never reach it.
5. Recompute when emitters, occluders, coarse levels, celestial inputs or the
   camera change. A presentation-only time animates emitters whose flicker bit
   is set, without server traffic. The function is deterministic: for the
   client presentation clock `t` in milliseconds, keys fall every 125 ms; key
   `k` has dip `d(k) = h(k) mod 52`, where `h` is SplitMix64's finalizer of
   `k + 0x9e3779b97f4a7c15` truncated to 32 bits; with `f = (t mod 125) · 256 /
   125` (integer division) the interpolated dip is
   `(d(k)·(256 - f) + d(k+1)·f + 128) / 256`, and the Q0.8 gain is
   `g = 256 - dip`, always in `205..256`. The flicker
   emitters' own field `F` (scalar and channels) becomes `F · (g - 256) / 256`,
   truncated towards zero, so `E' - E` is that delta. Conformance compares
   flickered output at a fixed `t`. Flicker modulates the emitter
   field after composition (`L + E' - E` for flickered sum `E'`), so the coarse
   floor of a block that the emitter itself lights cannot suppress it. The delta
   is added to the signed, unbounded composed scalar and channels, and the
   negative clamp, common-vector scaling and scalar saturation of step 3 then
   run once, after it, so no clipped magnitude or hue is lost.

Using `max()`/`min()` instead of a sum avoids double counting, because the
coarse term already includes visible sources. The cost is underestimating
overlaps of a visible and a hidden source, which is an accepted presentation
trade-off.

Remembered (fog) tiles compose their signed base, client celestial light and
their frozen coarse levels, then receive the remembered-world lift of
MAP_RENDERING.md: when the composed scalar `S` is below raw 40, `40 - S` is
added neutrally before window attenuation, so dark remembered geometry keeps
rendering at `(40, 40, 40)` rather than black. They never compose the
remembered emitters of the
objects shown in fog, which may since have changed, and their faces are closed
by the server's face bits, so emitter rays from visible cells stop at their
boundary; the coarse level carries hidden and visible local light there.

## Celestial lighting

### Split of authority

The server keeps celestial authority: game time, region profiles and their
phase mappings, lunar overrides, structure, and the authoritative celestial
field that gameplay darkness gating reads. The client receives what it needs
to compute the same celestial field for its authorized tiles and evaluates it
itself, so presentation can move the sun smoothly within an hour and light
remembered tiles correctly.

### Region inputs

A new server command `CELESTIAL` (schema `game-commands.json`) carries
per-connection profile slots, numbered `0..254` in one byte. A slot is assigned
to each distinct effective region profile of the maps in the client's celestial
closure (below) and carries only what the solver consumes, never the region's
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
- the start of the current hour in game seconds, so the client interpolates
  from the current to the next hour's phases over that hour in lockstep with the
  base endpoints.

The client never learns epochs, rates or override controls: the server resolves
time to phases, so time authority stays on the server.

Payload, in network byte order like every MAP2 field, encoded and decoded by
the shared `celestial_protocol_write()`/`celestial_protocol_read()`:

| Field | Type | Meaning |
| --- | --- | --- |
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
on the next update, in lockstep with gameplay gating.

### Structure inputs and the celestial closure

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
3,757 cells, at most 4 bytes each even when no run merges, about 15 KB with
fragment headers, so a complete closure always fits in one command: each `CELESTIAL_STRUCTURE` replaces the client's closure atomically,
and no continuation framing is needed. It is resent when the closure moves
with the player, or when a structure revision, door or gate changes a
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

### Client solver and conformance

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

The client adds its celestial field to the signed base before composition
(step 3 above), for visible and remembered tiles alike. The base no longer
contains celestial light, so the server never samples the celestial field in
MAP2 for display: it stops calling `celestial_light_keyframe_request()` and
reading celestial display values. Gameplay darkness gating is unchanged and
stays fail-closed: MAP2's per-cell gate keeps calling `map_get_darkness()`,
which still obtains the current celestial generation synchronously through
`celestial_light_ensure()` before any live object is serialized, so a field
invalidated by a door, structure revision, profile or lunar override is never
read stale.

### Celestial budget

Structure is static between changes, and row runs compress the open air that
dominates upper levels. The client keeps 3 bytes per closure cell; the worst
case is 3,757 cells, about 11 KB stored and 15 KB on the wire, and typical scenes are far smaller. The rollout qualifies this against MAP_RENDERING.md budgets before
the celestial step merges.

## Contract change: visible emitters and structure

MAP_RENDERING.md currently states that no visual change may disclose radiance
source identity. This design deliberately narrows that invariant to **objects
the viewer cannot see**. For a sent object, MAP2 now discloses whether it emits
light, with its radius, flicker opt-in and color. Two visually identical sent
objects with different light properties are therefore distinguishable. That is
the price of computing presentation lighting on the client, and the rollout's
final step updates MAP_RENDERING.md to say so.

Celestial lighting also discloses static celestial structure (walls,
roofs, floors and their transmission classes) of cells near authorized tiles
that are not themselves authorized. That is map geometry, not creatures or
light, and it is what the client needs to reproduce sun and sky light exactly.

A per-object opt-out (`light_private`, keeping a visible object's light
server-side) is not part of this proposal. It can be added later without a
protocol change, because such an object would simply send no emitter.

## Privacy argument

What a modified client can learn, beyond today's MAP2:

- **Sent emitters:** radius, flicker opt-in and color of objects it already
  sees. This is new, but it concerns only already-authorized objects.
- **Hidden light:** for each visible authorized tile and sub-layer, the signed
  1/8-daylight level of local light, with sources pooled per fixed 3×3 block
  before propagation. A source anywhere inside a block gives every block the
  same level, so neither the peak nor the levels of neighboring blocks locate
  it more precisely than its block. Sources on one tile group before the
  nonlinear profile, so separating co-located sources can change a level (for
  example `+3` and `-1` group to `+2`); that reveals that they are no longer
  co-located, not where they are. Hue, count and profile are never sent.
  Remembered tiles keep the levels they last had while visible, so a moving
  hidden source cannot be followed through fog.
- **Environment:** exact signed base radiance, including negative environment,
  on authorized tiles. It contains no local source and no gameplay state of
  others; the former aggregate fallback, which sent complete per-cell radiance
  of hidden sources around darkness floors, no longer exists.
- **Occluders and faces:** only properties of objects MAP2 sends to the
  viewer, and static seam structure between two cells both in view. Face bits
  never reflect residency or anything beyond the authorized set.
- **Celestial:** region light constants and effective phases (no epochs or
  identities), and the structure of authorized cells in the current window,
  built only from objects sent to the viewer, with door and gate state current
  only for visible cells and frozen on remembered ones.
- **Never sent:** creatures, and the source identity, count, exact tile, color,
  profile or height of any local light source not itself sent in MAP2; no
  property of hidden or unsent objects; no dynamic state of cells outside the
  authorized set.

This is less disclosure than the current contract for hidden sources, and an
intentional increase for visible ones and for static structure.

## Rollout

Protocol 1085 ships the new model; the legacy model is removed. Nothing is
gated by an option or negotiated, so the steps below land as one stack, and a
release containing protocol 1085 contains all of them.

1. Protocol 1085, server producer and client decoder: signed base radiance,
   emitters, the coarse local level and occluders on every connection. The
   legacy full-radiance encoding, the SETUP subtype and the server option are
   gone.
2. Client emitter composition, the only rendering path. `client_lighting.c` is
   the CPU reference: it groups emitters per tile, evaluates the shared
   `light_profile` tables with a line walk through the occluder grid, composes
   by sign and clamps to the coarse level. The map sampler adds the result to
   received base radiance per cell sample, before the existing GPU light rows.
   The client's Smooth lighting setting is removed; an existing settings file
   that names it still loads, because unknown settings are skipped. The line
   walk approximates the server's 3D supercover rays and evaluates one depth.
3. Celestial: the shared libatrinik model and solver with the server
   delegating to it, the `CELESTIAL` and `CELESTIAL_STRUCTURE` commands and the
   client solver, qualified against the server solver. Base radiance then
   excludes celestial light, and the server's display-only lighting code is
   deleted.
4. GPU emitter stage replacing the CPU reference, with linked depths, plus
   qualification: GPU frame budgets as in MAP_RENDERING.md, and offline
   player-view proofs comparing indoor, outdoor and cutaway Brynknot fixtures.
   The offline-only discrete projection, its conformance fixtures and goldens
   are retired with golden re-approval.
5. Update MAP_RENDERING.md to make this document normative.

## Test plan

- Protocol: encode/decode round trips; reserved bits in the local byte,
  occluder and emitter flags; `ext_flags` value 32; radius 0 and
  the full `int8` range; positive radius with color `000000`, negative radius
  with a color or with flicker; level `-16`; the signed base conformance
  vectors; truncated emitter/occluder fields; reconnect reset; and
  a failed parse leaving map state unchanged.
- Server: coarse level equals the block's signed quantized peak for fixtures
  with a hidden source behind a wall; a source moved through every position of
  its block leaves every block's level unchanged; remembered tiles keep their
  last levels while hidden light changes and never receive objects or
  emitters; per-sub-layer level counts 1..7 and their reuse rule; per-face
  attenuation, including a cell whose only authored face is east; no emitter or occluder for an
  unsent object or cell; hidden and plugin-hidden blockers set no occluder bits;
  glass floors set `v` only; an emitter-only change (radius, flicker or color)
  produces a layer delta; `tli` stays in base radiance; negative environment
  is sent as negative base; gameplay darkness gating identical with the flag on
  and off; the vision-floor bit follows XRAY; face bits closed towards
  remembered and out-of-view neighbors and unchanged when an unrelated map
  loads or unloads.
- Client: emitter field matches a CPU reference of `light_path_value()` for
  open, walled, roofed, transmissive (horizontal and vertical) and multi-depth
  fixtures; per-tile grouping with exact constituent colors beyond eight
  colors; composition and block limits for positive, negative and zero levels;
  signed base cancellation; vision floor after composition; player field after
  composition, absent on fog; fog tiles compose no remembered emitters; flicker
  only for flagged emitters; GPU frame-time budgets.
- Celestial: the libatrinik model matches the server's model for all solar
  hours, season months and lunar ages; the client solver on the encoded closure
  matches the server solver bit for bit on outdoor, roofed, glass, grate, door,
  aperture-spill and multi-level fixtures; remembered cells keep their whole structure record when another player builds,
  removes or opens there, checked with two clients; cells outside the closure
  solve as open air; mixed-profile closures select each fragment's slot, and
  slot 255 gives zero celestial light; region slots carry no digest or epoch.
- Privacy: another player's inventory-derived emitter is never sent, while the
  viewer's own is; all cells of a block report the same level; edge remainders
  join the adjacent block; levels never exceed 15 in magnitude; moving a single
  uncancelled source within its block leaves its own block's level unchanged.

## Decisions recorded

- Seam faces encode static continuity between cells both in view; never
  residency (maintainer decision, option b).
- Coarse light: fixed 3×3 blocks, 16 levels per sign.
- Negative environment: signed base radiance; the aggregate fallback and its
  `ext_flags` bit are removed.
- Flicker: per-emitter flag bit. `light_private`: deferred, not proposed.
- Celestial light moves to the client, with server authority over time,
  profiles and gameplay.
- Client-computed lighting is the only model: always on, no option, no
  negotiation and no legacy full-radiance path (maintainer decision).
- Coarse light pools sources per block before propagation, is sent per
  sub-layer, and is frozen on remembered tiles.
- Horizontal transmission is per cardinal face.
- The celestial closure is the authorized cells in the current window; cells
  outside it are open air, structure values come only from sent objects, and
  remembered cells keep their whole remembered record.
- Profile slots are 8-bit with slot 255 failing closed; one atomic
  `CELESTIAL_STRUCTURE` holds the whole closure.

## Open questions

- Whether the flicker opt-in should also cover authored flicker profiles
  (rate and depth) in the reserved emitter flag bits.

## Follow-up work outside this protocol change

- Replace the synchronous MAP2 fallback rebuild with serving the previous field
  while a budgeted job rebuilds it. This needs a decision on MAP_RENDERING.md's
  fail-closed contract.
