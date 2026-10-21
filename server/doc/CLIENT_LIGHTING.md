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
   light, in exact signed Q4.11 with RGB. The viewer's own `tli` override is
   not part of it; it travels as bit 5 of the local-light byte.
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
   hue. A remembered tile keeps receiving its levels while the per-socket
   cache is valid, like all its light levels. The levels sent to a viewer
   leave out the viewer's own light (its player object's glow, such as a
   carried torch): the client always receives that as an emitter and traces
   it exactly, while pooled into the block it would hold the whole block at
   its peak and move only when the viewer crosses a block edge, trailing a
   moving viewer by up to two tiles. Every other source stays pooled,
   including other players' light, and the viewer learns nothing that
   unequipping its own light would not show. Each block keeps, per sign, its
   strongest contribution with that contribution's source and the strongest of
   any other source, so the viewer's level is read from the shared block entry
   without another walk. The level never depends on where in a block the
   viewer stands, so moving a light through a block cannot locate another
   source in it.
4. **Occluders of sent cells**: geometry bits plus quantized transmission of
   each cardinal face and the downward face, derived only from objects MAP2
   sends, and face bits from static seam structure between authorized cells
   only.
5. **Celestial light** moves to the client too, in a follow-up (see Celestial
   light); until then base radiance carries it.

Gameplay keeps using the full authoritative server light. That includes
darkness gating of live objects in MAP2 (`raw_light <= 0`), infravision,
stealth, `tli`, XRAY and the celestial field. Nothing in this design moves
gameplay authority to the client.

### What is exact and what is coarse

Everything that does not identify a hidden source is sent exactly: base
radiance, sent emitters and sent occluders.
Only light of local sources the viewer cannot see is coarse. Exact per-tile
local light on authorized tiles was considered and rejected: the per-cell
gradient around a wall locates the hidden source, which is exactly the
disclosure this design removes (see Problem). Remembered tiles keep receiving
their coarse levels, like all their light levels, while the per-socket cache
is valid (the maintainer's disclosure boundary for previously sent tiles).

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
| `MAP2_FLAG_EXT_LOCAL_LIGHT` (8) | 1 + n | changed | `uint8` count `n` of `1..7`, then `n` level bytes: level `i` applies to sub-layer `i`, and sub-layers from `n` on use level `n - 1`. Each level byte: bits 0–4 signed level `-15..15` (two's complement; `-16` invalid); bit 5: the viewer's `tli` override, which adds raw 1280 (daylight) neutrally during composition; bit 6 reserved, zero; bit 7: apply the viewer's vision floor during composition |
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
| `MAP2_FLAG2_EMITTER` (2048) | 1 + 1 + 3 | signed `glow_radius` as stored (`int8`, 0 invalid); emitter flags: bit 0 flicker opt-in, bits 1–7 reserved, zero; then authored sRGB `RRGGBB`: any color for a positive radius (`000000` is black light, whose zero linear vector dilutes hue exactly as the server's resolver weighs it), `000000` for a negative one |

The emitter payload is the last optional layer field: it follows the ALPHA,
ROTATE, ZOOM, TARGET, PROBE and GLOW payloads in that order, and precedes the
next layer record. Radii are not clamped on the wire, because the server sums a
tile's radii before mapping the sum through `light_mask`, which saturates at
±13; clamping each emitter first would change that sum. Flicker is allowed
only on a positive radius; darkness never flickers.

### Signed base radiance

Every per-sub-layer scalar and RGB lighting word is a **signed** two's
complement `int16`, signed Q4.11 on the existing scale (2048 is raw 1280,
daylight). It carries environment only: no local source, no celestial light
and no XRAY floor. The client composes celestial light itself (Celestial
light), so the environment is achromatic: every channel equals the scalar.
Encoding from the server's raw value `r`:

- `q = sign(r) · floor((|r| · 8 + 2) / 5)`, the existing Q5.11 rounding applied to the
  magnitude, so `Q(-r) = -Q(r)`;
- saturating symmetrically to `-32767..32767` (raw ±20479, 16× daylight).
  The server never encodes `-32768`; decoders read every word as a plain
  `int16` and accept it.

The base no longer changes within an hour, so MAP2 sends no timed-light
descriptor or next-hour endpoints; the client blends its own celestial field
between the current and next hour instead. Decoders still read both as
signed values, in arithmetic wide enough for a cross-zero transition (`-2048`
to `2048` passes through 0).
Conformance vectors: raw `0 → 0`, `1 → 2`, `-1 → -2`, `160 → 256`,
`1280 → 2048`, `-1280 → -2048`, `20479 → 32766`, `20480 → 32767`,
`-20480 → -32767`. Current world, map and celestial anchors stay within raw
±1280, far inside the range.

The base is the only bounded term: the environment is saturated to raw
±20479 (16× daylight) by its encoding, and the client converts its composed
local term without saturation (Client, step 3). Because the encoding is
odd-symmetric, environment and composed light of equal magnitude cancel to
exactly zero whenever the environment lies in that range, as the server's
single raw sum does, and every other sum, residuals included, differs from
the server's single rounding by at most one Q4.11 unit per channel. An
environment beyond the bound (only an extreme authored floor value can reach
it) is presented at the bound. The scalar and each channel are encoded independently, and a negative
channel is meaningful until composition finishes.

### Budgets

MAP_RENDERING.md caps per-sub-layer *lighting samples* at 8 bytes and the
client and per-socket lighting storage at 56 bytes per cell. Signed base keeps
the 8 bytes per sub-layer, 56 per fully populated cell, and the cell gains 13
bytes of header: 7 coarse levels, 5 bytes of occluder and a known-state byte.
This design therefore raises the client's per-cell lighting storage cap to 72
bytes. The server's per-socket cell keeps the 56 bytes of signed base and adds
the 45 bytes of protocol-1085 state below, so its cap is 101 bytes per fully
populated cell. The rollout qualifies client memory at 72 and server memory
at 101 bytes, and MAP_RENDERING.md records both (with the client's 20-byte
celestial field, 92 bytes per client cell). With the base neutral, the
server no longer sends base RGB words or next-hour endpoints and its
per-socket cell drops the next-hour endpoint cache; a sent cell carries 27
bytes of lighting state: 14 of scalar base, 7 coarse levels, 5 of occluder
and the known-state byte. Emitters are object attributes, not lighting samples: their 5 bytes
ride on the layer record of the object they describe, within the unchanged
complete-MAP2 and continuation limits. The client stores emitters in one packed
per-cell array of 6-byte entries (layer, radius, flags, RGB), allocated only
for emitting cells and bounded by the cell's sent layer records, so it scales
with object storage rather than the lighting budget. One emitter-field
evaluation composes at most 2,048 emitters (`CLIENT_LIGHTING_EMITTERS_MAX`),
far above any real scene: a stack sending more composes no emitter field and
presents its coarse levels, so no publication can stall the render thread. The per-socket cache adds
45 bytes per cell as stored: the 24 bytes of emitter state (see Server), the
occluder word (8 bytes, 5 of them meaningful), the 7 per-sub-layer coarse
levels with their count byte and the known-state byte, which the delta
encoding compares against for visible and remembered cells alike, and the 8
bytes of the map cell the coordinate last authorized (map count and
coordinates), which binds remembered fog to it. MAP_RENDERING.md records
these figures.

### Validation

Decoders reject, before any state is published, exactly as for other
malformed MAP2 fields: an emitter radius of 0; a negative radius with a nonzero
color (darkness is achromatic) or with the flicker bit; a local-light count
outside `1..7`, before reading or indexing any level byte; a local level of
`-16`; `ext_flags` value 32; and any nonzero reserved bit. A positive radius
with color `000000` is valid: the object loader and plugins accept it, and the
server renders it as positive scalar light whose zero linear vector (the shared
`light_profile_color_component()` resolves a zero peak to zero) dilutes hue, so
the client resolves it the same way rather than rejecting the update. The base words have one meaning on every 1085
connection.

## Server

- **Gameplay light only.** The server keeps authoritative raw light for
  darkness gating, infravision, stealth, XRAY and `tli`: the scalar local mask
  and the celestial solver's field (CLIENT_CELESTIAL.md, What stays on the
  server). Display-only lighting code without a consumer is deleted: MAP2's
  per-cell keyframe sampling, the timed-light descriptor and next-hour
  endpoints, the per-socket light generation and the celestial-colored base.
- **Base radiance** is the environment term of each sub-layer: raw light minus
  the local source value and the celestial value, encoded as signed Q4.11. It
  is achromatic, so every channel equals the scalar: neutral `-80` beside a red
  `+80` emitter stays `(-80, -80, -80)`. The viewer's `tli`
  override is not folded in: it is a viewer property carried as bit 5 of the
  local-light byte, refreshed on remembered tiles like the vision-floor bit,
  so a tile that entered fog follows a later `/tli` toggle instead of keeping
  the offset frozen in its base. The XRAY floor is not applied (see Vision
  floor). There is no aggregate fallback: negative environment is sent as
  negative base.
- **Coarse local light** is derived each time MAP2 sends a visible or
  remembered cell, from
  the already maintained per-space source counters. No new per-map state is
  needed. All light MAP2 sends for a cell, base and coarse level, is the
  record cell's own: the authorized tile. Only gameplay darkness
  gating reads the space of the object it gates (for a multi-part object,
  the part's own space), and when that candidate is rejected the cell's light
  gates the sub-layer's later objects instead. No light of any other space is
  sent. The server sends the shortest level count describing the sub-layers;
  with one cell sample per record they are equal:
  - blocks partition each map axis of size `W` independently: with
    `k = floor(W / 3)`, block `i < k - 1` covers `[3i, 3i + 3)` and the last
    block covers `[3(k - 1), W)`, so a remainder of one or two cells joins it
    and it is three to five cells wide. A cell's block is the one containing
    it, so no block is narrower than three cells. A map with an axis shorter
    than three cells would make blocks that locate a hidden source within
    fewer than nine cells, so it fails closed: it reports level 0 and its
    sources contribute to no block;
  - pool each block's sources as its strongest positive and strongest
    negative single-source mask, before any propagation. Every emitting
    object counts with its own glow radius, and light a space's counter holds
    without an object counts as one more source; no two sources are summed,
    not even on one tile. A viewer who controls a known light, on the hidden
    source's tile or another, therefore cannot combine it with the hidden
    source through the nonlinear mask and recover that source's radius from
    where the level saturates: raising the known radius reveals only
    `max(known, hidden)`, which the level already discloses with no known
    light at all. The pooled source of a block reaches another block by the
    gap between the two block rectangles in cells (0 for the own block, 1 for
    an edge neighbor, `√2` for a corner one), which depends only on block
    geometry. Every block within the largest radius counts: on this map and on
    every map reached through seams light crosses (a resident reciprocal link,
    and continuity as `light_face_continuous()` requires): cardinal and
    diagonal horizontal seams and links to the levels above and below, taken
    in any order, so corner maps, maps linked only diagonally and a level
    linked only from a neighboring map count too. The linked levels
    of a stack form one vertical bucket spanning the whole linked component:
    a block on any of them reaches the target at the horizontal distance
    `sqrt(gx² + gy²)` of their gaps, whatever the number of levels between
    them, so the levels of a stack agree, however tall it is, and viewers with cells on several floors cannot tell which floor a
    hidden source is on, or follow it between floors. Light through floors is
    therefore presented at its horizontal strength; that bleed is presentation
    only, like the bleed through walls below. Each map is divided into its own blocks and measured in the
    target map's coordinates, so the blocks of neighboring maps need not align.
    No map is ever loaded for it, and no reachable map is dropped. The work is
    bounded: a component that places more than 4096 maps (real topologies stay
    far below; a 13-level stack of one-cell maps within reach places 1053),
    such as an unbounded legacy link cycle or stack, fails closed to level 0
    for every target in it, so its levels still agree and no rebuild is
    unbounded. A source anywhere inside a block therefore
    gives every block the same level, so neighboring blocks cannot
    triangulate its tile;
  - quantize the strongest positive and the strongest negative contribution
    reaching the block separately, each magnitude down to 1/8-daylight levels
    (raw 160) and clamped to 15;
  - the two levels then compete: the larger wins with its sign, and equal
    levels go to light. Quantizing first means a viewer's known light of the
    opposite sign learns only the hidden extremum's level, never where it
    lies inside its bin (`light_local_level_from_extrema()`).
- **Remembered tiles keep receiving light levels.** When base LOS loss turns
  an authorized tile into fog, the server keeps its per-socket cache entry
  marked as remembered and keeps sending that tile's light-level information,
  its signed base radiance and coarse local levels with the
  viewer's vision-floor and `tli` bits, as they change, for as long as the
  per-socket cache is valid; a map change, hard clear or reconnect ends it.
  This is the maintainer's disclosure boundary: light levels of previously
  sent tiles are disclosed. A tile never sent receives nothing. Remembered
  tiles never receive objects, emitters, occluder changes or any other object
  information: those describe objects, and the objects on a remembered tile
  are not currently sent.
- **Vision floor.** XRAY raises presentation light to raw 100 after all light
  is combined. The server sends base radiance without that floor and sets bit 7
  of the local-light byte for viewers with XRAY; the client applies the same
  floor to the composed signed values, before clamping (Client, step 3). When
  bit 7 is set and the composed scalar `S` is below the floor, the client adds
  the difference neutrally to the scalar and to every channel, exactly as the
  server used to raise raw light before resolving color. Composition works in
  Q5.11, so the floor is raw 100 encoded, Q5.11 160
  (`light_profile_raw_to_radiance(100)`), never the raw number itself.
  Conformance vector, in raw light: scalar/RGB `(80, 80, 0, 0)` becomes
  `(100, 100, 20, 20)`; in Q5.11, `(128, 128, 0, 0)` becomes
  `(160, 160, 32, 32)`.
  This is a property of the viewer only, so it discloses nothing about sources.
- **Emitters** are copied from `glow_radius`, `light_color` and the
  `light_flicker` object attribute (archetype default, boolean) of the object
  MAP2 is already sending (the object on that tile, including the viewer's own
  derived emitter). For a multi-part object this is the part on that tile,
  with that part's own `glow_radius`, exactly as the server's light sources
  are the parts at their own positions: the record's tile is the emitter's
  origin, whatever `quick_pos` says about which part draws the face, so a
  part that carries no light sends no emitter and the head's light stays at
  the head's tile. Another player's emitter is never sent: it is derived from
  the strongest eligible item in that player's private inventory, so its exact
  radius and color would disclose inventory state that item packets show only to
  the owner. Other players' light reaches the viewer only through the coarse
  level. A negative radius is sent with color `000000` and no flicker. The
  per-socket cache keeps, per cell, a bitmap of layers that carry an emitter, a
  bitmap of stale emitter layers and a 64-bit digest of the cell's emitters (24
bytes).
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
  the objects authoring that face, and objects with a downward face set `v`
  (as in `spatial_cell_light_face()`, every hidden wall-layer surface, such
  as a roof or camera surface, owns one even without `sky_boundary`);
  each is the maximum attenuation over the sent objects (the minimum
  coefficient, as the server's face traversal takes), quantized up to the next
  step. A cardinal face that no sent object of the cell authors is opaque when
  any sent object blocks view, and other unauthored faces stay open, as in
  `spatial_cell_light_face()`: its implicit fallback applies only when no
  object authors the face, so a transmissive pane authoring a face keeps its
  transmission beside a view blocker that does not, and the fallback never
  applies to the downward face: an
  ordinary wall without a downward face leaves `v` open, so light still
  crosses between floors at walls. A client step
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
  upward face is also closed under a sealed virtual sky cover, and so is the
  matching downward face of the cell above, because `spatial_face_light()`
  applies the lower cell's virtual cover in either travel direction. A vertical
  step therefore tests the face it leaves by and the destination's opposite
  face, and is stopped if either is closed. Every face
  leading to a remembered, unauthorized or out-of-view cell is always blocked.
  Both cells in view are resident by construction, so the bits never depend on,
  and never encode, whether some other map is loaded.
- **No per-player light recompute.** MAP2 reads cached base radiance and
  per-cell scalars; coarse levels are cached per block, invalidated only by source changes on the maps their walk reaches. MAP2
  makes no keyframe calls; the only celestial work left in it is gameplay
  gating's current field.

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
   coarse level does not group: it pools each source alone (Server), so where
   co-located sources of opposite sign cancel, such as `+3` and `-1` grouping
   to `+2`, the coarse floor can present the block at its strongest single
   source's level rather than the grouped one. The same holds where the
   viewer's own light shares a tile with a source of the opposite sign: the
   viewer's level leaves its own light out, so a `+5` viewer over `-4`
   darkness, grouped by the client to `+1`, is floored at the darkness's
   level. Like the bleed below, that is presentation only and the price of
   resisting co-located probing. The
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
   or below. An upward step that terminates on a cell whose sent occluder
   blocks view (bit 0) or is a roof surface (bit 2) skips that receiving
   slab's `v`, as `light_terminal_surface()` decides for `light_path_value()`
   under its upward-crossing guard (`face->normal[2] > 0`), so the exposed
   surface is lit from below; an ordinary floor (bit 1 alone) applies its `v`,
   so an opaque floor receives no light from below. A downward step never
   skips a slab: it always applies the `v` of the upper cell it leaves, so a
   lower surface is never lit through an opaque floor. Source-side virtual
   cover still applies.
3. Combine the client emitter sum `E` with the coarse block level `C` (raw
   step 160), which is a bleed hint for the whole block. `C` excludes the
   viewer's own light, which `E` always carries exactly, so the viewer's light
   pool follows it tile by tile:
   - `C > 0`: `L = max(E, 160C)`; `C < 0`: `L = min(E, 160C)`; `C = 0`: `L = E`;
   - except that an emitter field of the opposite sign at least as strong as
     the floor keeps its value (`C < 0` and `E ≥ -160C`, or `C > 0` and
     `E ≤ -160C` give `L = E`): `C`'s sign comes from single sources, so sent
     emitters that group, such as four `+4` and one `-5` on a tile giving
     `E = 640` against `C = -2`, are never reversed by it.

   The coarse term is only a floor (or, for darkness, a ceiling) under the
   emitter field; nothing caps `E` at the block's level. `C` measures the
   strongest single source, while `E` groups a tile's sent emitters as the
   server does, so three sent `+4` emitters on one tile keep their grouped
   value even though `C` is the bin of one of them. `E` is built only from sent
   objects, so leaving it unbounded discloses nothing.

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
   to its magnitude, `sign(r) · floor((|r| · 8 + 2) / 5)`, without saturating,
   so dense local light keeps its full weight and its residual against the
   base: raw environment 20000 with local darkness -25000 composes to raw
   -5000. The composed radiance is, per scalar and channel, signed base +
   converted `L`, both in Q5.11 units, in unbounded signed arithmetic. The vision floor
   (if bit 7 is set) applies next, to the signed scalar and channels, as the
   server raises the raw scalar before resolving color: pre-floor scalar and
   channel raw `(80, -40)` (Q5.11 `(128, -64)`) become raw `(100, -20)`
   (Q5.11 `(160, -32)`), and then `(100, 0)`. A set `tli` bit adds raw 1280,
   converted the same way, neutrally after `L` is selected and before the
   floor; whether the emitter field supplied `L` (for flicker, step 5) is
   decided before this addition, so `tli` never disables flicker. Negative
   channels then clamp to zero; if the largest channel then
   exceeds the finite range, all three channels scale together so the strongest
   is 65535, as the server's common-vector overflow rule requires, keeping hue.
   The scalar clamps to `0..65535`: negative composed light never reaches the
   unsigned stages that follow.
4. **Player field.** After composition, and after the vision floor, the
   existing independent local-player presentation field (radius 8, center
   strength 80, neutral) is added exactly as MAP_RENDERING.md specifies today:
   primary depth only, only on currently authorized visible tiles (never on
   fog), with its existing clipping and before the window fade. It does not
   depend on the viewer's emitter, so an unlit player keeps it. Then the
   unchanged unsigned Q16 tone/LUT stage runs; negative intermediate values
   never reach it.
5. Recompute when emitters, occluders, coarse levels or the camera change.
   Recomputation is bounded per presented frame: a server may change an
   emitter, or move the cache window, on every depth in every update, so a
   frame rebuilds at most two depths' emitter fields and every other depth
   presents its previous field until following frames rebuild it. A depth
   whose cache window moved presents its previous field translated by the move,
   with cells newly exposed at its edge unlit; a depth that moved levels keeps
   the field of its store; a depth with no field yet, or whose window was
   resized, composes no emitter field (coarse level only) until it is built.
   Offline proofs disable the bound so every frame is fully rebuilt. A presentation-only time animates emitters whose flicker bit
   is set, without server traffic. The function is deterministic: `t` is the
   client presentation clock in milliseconds since the client started (the
   frame tick `LastTick`, origin 0), the key is `k = floor(t / 125)`, and key
   `k` has dip `d(k) = h(k) mod 52`, where `h(k)` is the low 32 bits of
   SplitMix64's finalizer applied to `k + 0x9e3779b97f4a7c15` (`z ^= z >> 30;
   z *= 0xbf58476d1ce4e5b9; z ^= z >> 27; z *= 0x94d049bb133111eb; z ^= z >> 31`,
   64-bit wrapping); with `f = (t mod 125) · 256 / 125` (integer division) the
   interpolated dip is
   `(d(k)·(256 - f) + d(k+1)·f + 128) / 256`, and the Q0.8 gain is
   `g = 256 - dip`, always in `205..256`. The flicker field `F` is the
   flickering emitters' contribution to the grouped field: `F = E - E_s`, where
   `E_s` is the field of the same tiles grouped without their flickering
   emitters (every steady and negative emitter kept), each resolved as in
   step 1, per scalar and channel. Because grouping is nonlinear per tile,
   `F` is not the flickering emitters' standalone field. `F` becomes
   `F · (g - 256) / 256`, truncated towards zero, so `E' - E` is that delta and
   `E'` always lies between `E_s` and `E`: flicker only dims a tile towards its
   steady light, never past it. Conformance compares
   flickered output at a fixed `t`. Flicker modulates the composed value only
   where the emitter field supplied it (`L = E`, decided before the `tli`
   term is added, and with `E ≠ 160C` when `C ≠ 0`, since an emitter field
   exactly equal to the floor may coincide with hidden steady light): there
   it becomes
   `L + E' - E` for flickered sum `E'`, so the coarse floor never masks the
   flicker of light the emitter field supplies. Where the coarse level
   dominates (`L = 160C ≠ E`) `L` is unchanged: the coarse level is anonymous
   by design, so the client cannot tell whether that floor comes from the
   flickering emitter or from hidden steady light, and presents it steady.
   Within an emitter's own block, its halo below the block's floor therefore
   does not flicker; that is the accepted cost of not attributing coarse light
   to sources. A cell whose flicker changes only its channels (grouping can cap
   the scalar while hue still flickers) flickers too. The delta is applied in
   raw light, replacing raw `L` by `L + E' - E` before the Q5.11 conversion of
   step 3 (for `E = 160`, `E_s = 0` and `g = 205` the delta is raw `-31`, so
   raw 129 converts to Q5.11 206), and the resulting signed, unbounded
   scalar and channels reach the vision floor;
   the vision floor, negative clamp, common-vector scaling and scalar
   saturation of step 3 then run once, after it, so XRAY still raises the
   flickered light to its floor and no clipped magnitude or hue is lost.

Using `max()`/`min()` instead of a sum avoids double counting, because the
coarse term already includes visible sources. The cost is underestimating
overlaps of a visible and a hidden source, which is an accepted presentation
trade-off.

Remembered (fog) tiles compose their signed base and their coarse levels,
which keep updating while the per-socket cache is valid, then receive the remembered-world lift of
MAP_RENDERING.md on the signed composed scalar and channels, after the vision
floor and before the negative clamp and common-vector scaling of step 3, as
`C_display = max(0, C + lift)` requires: whenever the composed scalar `S` is
below the floor of raw 40 (Q5.11 64), `lift = 40 - S` is added neutrally (raw `S = -40` with a channel at
raw `-100` lifts that channel to raw `-20`, which then clamps to 0), and only
then the window attenuation runs, so dark remembered
geometry keeps rendering at raw `(40, 40, 40)` rather than black. The lift is
raw 40 encoded, Q5.11 64, because `S` is Q5.11. They never compose the
remembered emitters of the
objects shown in fog, which may since have changed, and their faces are closed
by the server's face bits, so emitter rays from visible cells stop at their
boundary; the coarse level carries hidden and visible local light there.

## Celestial light

Celestial light (sun, moon, sky and starlight) moves to the client as well: it
is always on, with no option, negotiation or legacy path, exactly like local
light. The server keeps authority over game time, region profiles, lunar
overrides and gameplay darkness. It is specified in `CLIENT_CELESTIAL.md`:
the client solves the celestial closure it receives and composes the field
onto the base, which carries no celestial light.

The server's celestial field is built from every object of a cell, including
hidden and unsent doors and gates; a base carrying it would reveal an unsent
aperture opening on a visible tile. That is why protocol 1085 ships only with
celestial light removed from the base (see Rollout).

## Contract change: visible emitters and structure

MAP_RENDERING.md stated that no visual change may disclose radiance source
identity. This design deliberately narrows that invariant to **objects
the viewer cannot see**. For a sent object, MAP2 now discloses whether it emits
light, with its radius, flicker opt-in and color. Two visually identical sent
objects with different light properties are therefore distinguishable. That is
the price of computing presentation lighting on the client, and
MAP_RENDERING.md now says so.

A per-object opt-out (`light_private`, keeping a visible object's light
server-side) is not part of this proposal. It can be added later without a
protocol change, because such an object would simply send no emitter.

## Privacy argument

What a modified client can learn, beyond today's MAP2:

- **Sent emitters:** radius, flicker opt-in and color of objects it already
  sees. This is new, but it concerns only already-authorized objects.
- **Hidden light:** for each visible authorized tile and sub-layer, the signed
  1/8-daylight level of local light, with each block pooled as its strongest
  single-source masks before propagation. A known light the viewer controls
  never combines with a hidden one, even on the same tile, so stepping its
  radius reveals only the maximum, which the level already shows. A source anywhere inside a block gives every block the
  same level, so neither the peak nor the levels of neighboring blocks locate
  it more precisely than its block. Co-located sources are pooled alone, never
  grouped, so the level depends on the strongest single source only, not on
  which sources share a tile. Hue, count and profile are never sent.
  Previously sent tiles, including remembered ones in fog, keep receiving
  their light levels (base and coarse) while the per-socket cache
  is valid: that is the maintainer's disclosure boundary for light. They never
  receive creatures, objects, emitters or occluder changes.
- **Environment:** exact signed base radiance, including negative environment,
  on authorized tiles. It contains no local source and no gameplay state of
  others; the former aggregate fallback, which sent complete per-cell radiance
  of hidden sources around darkness floors, no longer exists.
- **Occluders and faces:** only properties of objects MAP2 sends to the
  viewer, and static seam structure between two cells both in view. Face bits
  never reflect residency or anything beyond the authorized set.
- **Never sent:** creatures, and the source identity, count, exact tile, color,
  profile or height of any local light source not itself sent in MAP2 (linked
  levels share one coarse value, so even the floor of a hidden source is not
  disclosed); no
  property of hidden or unsent objects; no dynamic state of cells outside the
  authorized set.

This is less disclosure than the current contract for hidden sources, and an
intentional increase for visible ones and for static structure.

## Rollout

Protocol 1085 ships the new model; the legacy model is removed. Nothing is
gated by an option or negotiated, so the steps below land as one stack, and a
release containing protocol 1085 contains all of them, together with the
celestial follow-up's removal of celestial light from the base radiance
(Celestial light).

1. Protocol 1085, server producer and client decoder: signed base radiance,
   emitters, the coarse local level and occluders on every connection. The
   legacy full-radiance encoding, the SETUP subtype and the server option are
   gone.
2. Client emitter composition, the only rendering path. `client_lighting.c` is
   the CPU reference: it groups emitters per tile, evaluates the shared
   `light_profile` tables with a line walk through the occluder grid, composes
   by sign with the coarse level as a floor. The map sampler adds the result to
   received base radiance per cell sample, before the existing GPU light rows.
   The client's Smooth lighting setting is removed; an existing settings file
   that names it still loads, because unknown settings are skipped. The walk
   generates the server's supercover events for the current depth only; it is
   the presentation path of this step, not the reference for linked depths.
3. GPU emitter stage replacing the CPU walk, with linked depths, plus
   qualification against an offline server oracle: `light_path_value()` run
   over only the objects sent to the viewer, which is three-dimensional and
   exact by construction: GPU frame budgets as in MAP_RENDERING.md, and offline
   player-view proofs comparing indoor, outdoor and cutaway Brynknot fixtures.
   The offline-only discrete projection, its conformance fixtures and goldens
   are retired with golden re-approval.
4. Update MAP_RENDERING.md to make this document normative.

## Test plan

- Floors: a hidden source moved between the floors of a linked stack leaves
  every floor's coarse level unchanged.
- Authorized light only: a multi-part object whose sampled space lies
  outside the window sends its record cell's base and coarse level, never the
  other space's; bit 6 of the local-light byte is rejected.
- Protocol: encode/decode round trips; reserved bits in the occluder word,
  vertical byte, emitter flags and the local-light byte (bits 0–4
  are the level, bit 5 `tli`, bit 7 vision floor; bit 6 is reserved); `ext_flags` value 32; radius 0 and
  the full `int8` range; local-light counts 0 and 8; positive radius with color
  `000000` accepted, negative radius
  with a color or with flicker; level `-16`; the signed base conformance
  vectors; truncated emitter/occluder fields; reconnect reset; and
  a failed parse leaving map state unchanged.
- Server: coarse level equals the block's signed quantized peak for fixtures
  with a hidden source behind a wall; a source moved through every position of
  its block leaves every block's level unchanged; the level sent to a viewer
  leaves out its own light (zero around a lone torch-bearer, unchanged for
  other viewers, and another source in its block still counts); remembered tiles keep
  receiving their light levels as hidden light changes and never receive
  objects, emitters or occluders; the producer emits one level per record (counts 1..7 and their reuse rule
  are decoder tests); per-face
  attenuation, including a cell whose only authored face is east; no emitter or occluder for an
  unsent object or cell; hidden and plugin-hidden blockers set no occluder bits;
  glass floors set `v` only; an emitter-only change (radius, flicker or color)
  produces a layer delta; `tli` is never in base radiance and its bit follows a
  toggle on visible and remembered tiles alike; negative environment
  is sent as negative base; gameplay darkness gating, compared against a
  fixture of which live objects each viewer may receive at each light level,
  is unchanged by protocol 1085; the vision-floor bit follows XRAY; face bits closed towards
  remembered and out-of-view neighbors and unchanged when an unrelated map
  loads or unloads.
- Client: the current-depth walk matches the server oracle for open, walled,
  roofed and transmissive fixtures, and the GPU stage matches it for
  multi-depth ones; per-tile grouping with exact constituent colors beyond eight
  colors; composition and block limits for positive, negative and zero levels;
  signed base cancellation; vision floor after composition; player field after
  composition, absent on fog; fog tiles compose no remembered emitters; flicker
  only for flagged emitters; GPU frame-time budgets.
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
- Client-computed lighting is the only model: always on, no option, no
  negotiation and no legacy full-radiance path (maintainer decision).
- Coarse light pools sources per block before propagation and is sent per
  sub-layer. Previously sent tiles, remembered ones included, keep receiving
  all their light levels while the per-socket cache is valid (maintainer
  decision); objects, emitters and occluders are sent only for currently sent
  objects.
- Horizontal transmission is per cardinal face.

## Open questions

- Whether the flicker opt-in should also cover authored flicker profiles
  (rate and depth) in the reserved emitter flag bits.

## Follow-up work outside this protocol change

- Replace the synchronous MAP2 fallback rebuild with serving the previous field
  while a budgeted job rebuilds it. This needs a decision on MAP_RENDERING.md's
  fail-closed contract.
