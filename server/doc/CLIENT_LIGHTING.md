# Client-computed lighting (protocol 1085 proposal)

Status: design, not yet the active contract. Until the rollout below
completes, [MAP_RENDERING.md](MAP_RENDERING.md) remains normative.

## Problem

The server computes all lighting: local source masks traced through 3D
spatial rays, RGB accumulation, and celestial fields. It then samples
`map_get_darkness()` per visible cell, depth, and sub-layer for every player
every tick and sends Q5.11 radiance. That has three costs:

- **Server time.** Rebuilds run on the game thread and stall it (seconds while
  walking Brynknot on 5.99.0). The scalar, positive and color masks of a source
  trace identical rays, and MAP2 can trigger synchronous rebuilds.
- **Presentation quality is server-bound.** The client interpolates one sample
  per cell vertex. It cannot shade buildings, roofs, or cutaways with more
  detail than the server grid. It also cannot animate light, such as flicker,
  without new packets.
- **Disclosure is coarse but uncontrolled.** Radiance is aggregate-only, but at
  Q5.11 per cell with RGB, a client can triangulate a hidden source's tile and
  hue from the gradient around a wall.

## Goal

The client computes the presentation lighting of everything it can already see,
on the GPU. The server keeps authoritative gameplay light and sends only what
the client cannot legitimately know, in a form that does not identify sources:

1. **Base radiance**: ambient, floor, world and celestial light, plus the
   viewer's own `tli` override. None of these is a source the player could
   locate. This stays Q5.11 with RGB, because it describes the environment
   (time of day, sky exposure, map ambient).
2. **Emitters of visible objects**: glow radius and authored color, sent only
   with an object that MAP2 already sends. Its existence and tile are already
   disclosed by its face, so the emitter adds no new location information.
3. **Coarse local light**: one byte per visible cell record. It is a signed,
   quantized, neutral level of local light, pooled over a fixed 3×3 block. Light
   (and darkness) from hidden sources still bleeds through walls without
   identifying the source tile, count, or hue.
4. **Occluders of visible cells**: blocks-view, floor, roof and door bits plus a
   quantized transmission, derived only from objects MAP2 sends to this viewer,
   so the client can occlude and attenuate emitter light.

Gameplay keeps using the full authoritative server light. That includes
darkness gating of live objects in MAP2 (`raw_light <= 0`), infravision,
stealth, `tli`, and XRAY. Nothing in this design moves gameplay authority to
the client.

## Wire contract (protocol revision 1085)

`protocol/schema/game-commands.json` advances `protocol_version` to 1085. Peers
below 1085 are rejected at setup, as for every revision.

Client lighting is negotiated per connection. A client that composes emitters
sends SETUP subtype `CMD_SETUP_CLIENT_LIGHTING` (6) with capability byte
`CLIENT_LIGHTING_CAP_COMPOSITOR` (1); the server echoes the effective mode,
which is on only when its `client_lighting` option is enabled. A 1085 client
that never sends the subtype, including one released before the compositor,
keeps receiving complete radiance and no client-lighting fields. Reconnect clears all
client emitter, occluder and coarse-light state with the map cache.

Per MAP2 cell record (one record per cell and depth). The fields follow the
existing extended-tile data, in this order, after RGB, keyframe and animation
data. They are announced by `ext_flags` bits, because the `MAP2_MASK_*` cell
word already uses bits 6–15 for coordinates:

| Field | Bytes | When | Meaning |
| --- | --- | --- | --- |
| `MAP2_FLAG_EXT_LOCAL_LIGHT` (8) | 1 | changed | bits 0–4: signed level `-15..15` (two's complement; `-16` invalid); bits 5–6 reserved, zero; bit 7: apply the viewer's vision floor after composition |
| `MAP2_FLAG_EXT_LIGHT_AGGREGATE` (32) | 1 | changed | Bitmap of sub-layers whose radiance, including timed-light endpoints, is complete aggregate light with the legacy meaning; the client composes nothing for them. Bit 7 is reserved, zero. |
| `MAP2_FLAG_EXT_OCCLUDER` (16) | 2 | changed | bit 0 blocks view, bit 1 floor, bit 2 roof surface, bit 3 closed door; bits 4–7: transmission attenuation `a` (0 unattenuated, 1–14 coefficient `256 - 16a`, 15 opaque; a view blocker with authored transmission keeps its attenuation); bits 8–11: local light does not cross the north, east, south or west face; bits 12–13: nor the upward or downward face; bits 14–15 reserved, zero |

Per MAP2 layer (existing `flags2`):

| Field | Bytes | Meaning |
| --- | --- | --- |
| `MAP2_FLAG2_EMITTER` (2048) | 1 + 3 | signed `glow_radius` as stored (`int8`, 0 invalid), then authored sRGB `RRGGBB`: nonzero for a positive radius, `000000` for a negative one |

The emitter payload is the last optional layer field: it follows the ALPHA,
ROTATE, ZOOM, TARGET, PROBE and GLOW payloads in that order, and precedes the
next layer record. Radii are not clamped on the wire, because the server sums a
tile's radii before mapping the sum through `light_mask`, which saturates at
±13; clamping each emitter first would change that sum.

Budgets: MAP_RENDERING.md caps per-sub-layer *lighting samples* at 8 bytes and
the per-socket lighting cache at 56 bytes per cell. Emitters are object
attributes, not lighting samples: their 4 bytes ride on the layer record of
the object they describe, so a cell whose seven object layers of a sub-layer
all emit adds 28 bytes of object payload, within the unchanged complete-MAP2
and continuation limits. The client keeps an emitter in the layer record it
already allocates for that object (8 bytes per emitting record), and the
per-socket cache adds 24 bytes per cell (see Server). The rollout's final step
records these figures in MAP_RENDERING.md.

The existing per-sub-layer Q5.11 lighting bytes carry **base radiance only**
when the connection runs client lighting. So do the timed-light next-hour scalar and
RGB endpoints (`MAP2_FLAG_EXT_LIGHT_KEYFRAME`) and their per-socket caches:
they omit local sources and the XRAY floor, so interpolating towards them
never reintroduces either. The 8-byte per-sub-layer budget is
unchanged. Decoders reject an emitter radius of 0, a
positive radius with color `000000` (a zero linear peak, which MAP_RENDERING.md
rejects for positive sources), a negative radius with a nonzero color (darkness
is achromatic), a local level of `-16`, and nonzero reserved bits. Rejection happens before any state is
published, exactly as for other malformed MAP2 fields. A server running the
legacy path sends no `LOCAL_LIGHT`, `OCCLUDER` or `EMITTER` fields, and its base
bytes keep their current full-radiance meaning. A 1085 client therefore renders
both modes.

## Server

- **Local masks stay complete.** Gameplay needs only the scalar mask, but the
  positive and color masks remain, because aggregate fallback samples (see
  Negative environment) and clients that did not negotiate the compositor need
  exact legacy RGB. Celestial fields are unchanged.
- **Coarse local light** is derived each time MAP2 sends a cell, from the
  already maintained per-cell `light_source_value`. No new per-map state is
  needed, and a change anywhere in the block reaches every cell of it on the
  next update:
  - the block is the fixed 3×3 square containing the cell, aligned to map-local
    coordinates (`x - x % 3`, `y - y % 3`). A remainder of one or two cells at a
    map edge joins the adjacent block instead of forming a smaller one, so no
    block is narrower than three cells unless the map itself is. The block never
    reads another map and never loads one;
  - take the strongest positive and the strongest negative local scalar in the
    block; the larger magnitude wins;
  - quantize the magnitude down to 1/8-daylight levels (raw 160), clamp to 15,
    and keep the sign.
- **Viewer overrides.** Base radiance is computed per viewer when MAP2 builds
  its view, so the existing `tli` lighting override is already folded into it.
- **Negative environment.** Environmental light can be negative, for example a
  floor with negative light, and the unsigned base bytes cannot carry that. A
  sub-layer whose environmental scalar or any environmental RGB channel would
  be negative, now or at its ready next keyframe, is therefore sent as complete
  aggregate radiance with the legacy meaning (local light, the XRAY floor and
  timed-light endpoints included), and marked in
  `MAP2_FLAG_EXT_LIGHT_AGGREGATE`. The client composes nothing for it, so
  darkness cancels local light exactly as today, per sub-layer and across
  keyframes. Those samples disclose aggregate local light as the current
  contract does; this trade-off is an open question.
- **Vision floor.** XRAY raises presentation light to raw 100 after all light
  is combined. The server therefore sends base radiance without that floor
  and sets bit 7 of the local-light byte for viewers with XRAY. The client then
  applies the same floor after composition. This is a property of the viewer
  only, so it discloses nothing about sources.
- **Emitters** are copied from `glow_radius` and `light_color` of the object
  MAP2 is already sending (the object on that tile, including the viewer's own
  derived emitter). Another player's emitter is never sent: it is derived from
  the strongest eligible item in that player's private inventory, so its exact
  radius and color would disclose inventory state that item packets show only to
  the owner. Other players' light reaches the viewer only through the coarse
  level. A negative radius is sent with color `000000`. The
  per-socket cache keeps, per cell, a bitmap of layers that carry an emitter,
  a bitmap of stale emitter layers and a digest of the cell's emitters
  (20 bytes). Gaining or losing an emitter forces a layer delta; a radius- or
  color-only change on a cached layer changes the digest and resends that
  layer on the next update. Emitters of objects not sent, whether hidden, out of sight, dark-gated,
  plugin-hidden or below the top object of a layer, are never sent.
- **Occluders** are derived only from the objects MAP2 sends to this viewer:
  the sent object of each layer, after plugin visibility and hidden-object
  filtering. A sent view blocker, floor, roof surface or closed door sets its
  bit. Its authored horizontal transmission gives a coefficient: doors and
  exact-type gates use `celestial_transmission_closed` or
  `celestial_transmission_open` by their current state (`FLAG_DOOR_CLOSED` for
  doors, `FLAG_NO_PASS` for gates), other objects `celestial_transmission`; the cell's attenuation is the
  maximum over all sent objects (the minimum coefficient, as the server's face
  traversal takes), quantized up to the next step. Face bits 8–13 mark the
  horizontal, upward and downward faces that local light does not cross: a neighbor on another map whose seam is not
  continuous for local light (mixed schema, discontinuous boundary,
  nonreciprocal link or a different effective profile digest), or one that is
  not resident, exactly as `light_path_value()` stops there. Seams are map
  structure, not sources. Space-wide flags such as
  `P_BLOCKSVIEW` are never used, because they aggregate hidden and unsent
  objects too. Unsent geometry therefore contributes nothing, and the client
  may light past a blocker it cannot see; that errs towards disclosing less.
- **No per-player light recompute.** MAP2 reads cached base radiance and per-cell
  scalars. Celestial ensure and keyframe work move out of the per-cell loop into
  one pass per resident map per tick (see Follow-up work).

## Client

The GPU light pass gains an emitter stage before the existing tone/LUT stages:

1. Group the visible emitters of a tile exactly as the server groups a tile's
   counters. The scalar and positive terms sum the radii of the tile's sent
   emitters and map each sum through `light_mask`, which reproduces the
   same-cell cap. Color keeps every constituent emitter with its own profile
   and color, resolved against the grouped positive term exactly as the server
   resolves its color accumulators, so hue can vary with distance. When some of
   a tile's emitters are not sent, the client underestimates and the coarse
   level supplies the rest. Every visible emitter is evaluated; the GPU stage
   processes them in batches rather than truncating. The occluders are an R8
   cell grid per depth. That grid is 16-bit (R16), because face bits 8–11 must
   reach the ray walk.
2. A compute (or full-screen fragment) pass evaluates every light vertex of the
   existing compact light rows. It sums emitter profiles using the same radial
   tables as the server (`light_profiles`, radius ≤ 4, vertical ≤ 4) with a 3D
   DDA through the occluder grid, so it follows the same "stop after the exposed
   face" and floor-crossing rules as `light_path_value()`. Partial transmission
   applies each crossed cell's attenuation, and a step across a face marked
   discontinuous (bits 8–11) stops the ray, as at a discontinuous map seam.
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
   coarse term is neutral. Final radiance = base + `L` per scalar and channel,
   except for aggregate sub-layers, which are used as received.
   Negative channels clamp to zero; if the largest channel then exceeds the
   finite range, all three channels scale together so the strongest is 65535,
   as the server's common-vector overflow rule requires, keeping hue. The scalar
   saturates at 65535. The vision floor (if bit 7 is set) follows, before the
   unchanged unsigned Q16 tone/LUT stage. Negative intermediate values never
   reach that stage. Base radiance above the finite Q5.11 range (raw 40959,
   32× daylight) arrives saturated, so darkness composed onto it cannot recover
   the lost magnitude. Current world, map and celestial anchors stay at or below
   raw 1280, so this cannot occur with present content.
4. Recompute when emitters, occluders, coarse levels or the camera change. A
   presentation-only time uniform permits flicker for emitters whose archetype
   opts in, without server traffic. Flicker modulates the emitter field after
   composition (`L + E' - E` for flickered sum `E'`), so the coarse floor of a
   block that the emitter itself lights cannot suppress it. The delta is added
   to the signed, unbounded composed scalar and channels, and the negative clamp,
   common-vector scaling and scalar saturation of step 3 then run once, after
   it, so no clipped magnitude or hue is lost.

Using `max()`/`min()` instead of a sum avoids double counting, because the
coarse term already includes visible sources. The cost is underestimating
overlaps of a visible and a hidden source, which is an accepted presentation
trade-off. Remembered (FOW) cells keep their remembered occluders and no live
emitters or coarse light.

## Contract change: visible emitters

MAP_RENDERING.md currently states that no visual change may disclose radiance
source identity. This design deliberately narrows that invariant to **objects
the viewer cannot see**. For a sent object, MAP2 now discloses whether it emits
light, with its radius and color. Two visually identical sent objects with
different light properties are therefore distinguishable. That is the price
of computing presentation lighting on the client, and the rollout's final step
updates MAP_RENDERING.md to say so.

Content that must keep a visible object's light private can be excluded from
emitter disclosure. A proposed `light_private` object attribute would keep that
object's light server-side, contributing only through the coarse level, at the
cost of coarser presentation (see Open questions).

## Privacy argument

What a modified client can learn, beyond today's MAP2:

- **Visible emitters:** radius and color of objects it already sees. This is
  new, but it concerns only already-authorized objects.
- **Hidden light:** for each visible cell, the signed 1/8-daylight level of
  the strongest local light in its fixed block. All cells of a block share one
  value, so a single uncancelled source cannot be located more precisely than
  its block from the peak alone. Sources on one tile group before the
  nonlinear profile, so moving one of several co-located sources within the
  block can change the level (for example `+3` and `-1` group to `+2`, while
  separating them restores the `+3` peak). The level then reveals that the
  sources are no longer co-located, not where they are. Hue, count and profile
  are never sent.
- **Residual leakage, honestly stated:** moving a hidden source within its block
  can still change the levels of neighboring blocks through falloff. Comparing
  neighboring blocks therefore narrows the position somewhat, but only to the
  extent that the falloff crosses a level boundary. Quantization limits this;
  nothing short of hiding the field entirely removes it.
- **Occluders:** only properties of objects MAP2 already sends to the viewer.
- **Never sent:** source identity, count, exact tile, color, profile or height
  of anything not itself sent in MAP2, and no property of hidden or unsent
  geometry.

This is less disclosure than the current contract for hidden sources, and an
intentional increase for visible ones: their emitter radius and color are new
information (see Contract change).

## Rollout

1. Protocol 1085, server producer and client decoder, with server config
   `client_lighting` defaulting to off and `true` rejected at startup until the
   client compositor exists. The legacy path is unchanged, and 1085 clients
   render it.
2. Client emitter composition. `client_lighting.c` is the CPU reference: it
   groups emitters per tile, evaluates the shared `light_profile` tables with a
   line walk through the occluder grid, composes by sign and clamps to the
   coarse level. The map sampler adds the result to received base radiance per
   cell sample, before the existing GPU light rows, and `client_lighting=true`
   is accepted from this step on. The line walk approximates
   the server's 3D supercover rays and evaluates one depth.
3. GPU emitter stage replacing the CPU reference, with linked depths, plus
   qualification: GPU frame budgets as in MAP_RENDERING.md, plus offline
   player-view proofs comparing indoor, outdoor and cutaway Brynknot fixtures.
4. Enable `client_lighting` on development, then production.
5. Remove the legacy full-radiance path for compositor clients. Positive and
   color masks remain for aggregate fallback samples and other clients.
   Update MAP_RENDERING.md to make this document normative.

## Test plan

- Protocol: encode/decode round trips; reserved bits; radius 0 and the full
  `int8` range; positive radius with color `000000` and negative radius with a color;
  level `-16`; truncated emitter/occluder fields; reconnect reset;
  and a failed parse leaving map state unchanged.
- Server: coarse level equals the block's signed quantized peak for fixtures
  with a hidden source behind a wall; no emitter or occluder for an unsent
  object or cell; hidden and plugin-hidden blockers set no occluder bits;
  an emitter-only change produces a layer delta; `tli` stays in base radiance; gameplay
  darkness gating identical with the flag on and off; the vision-floor bit
  follows XRAY.
- Client: emitter field matches a CPU reference of `light_path_value()` for
  open, walled, roofed, transmissive and multi-depth fixtures; per-tile
  grouping with constituent colors; no truncation with many emitters;
  composition and block limits for positive, negative and zero levels;
  vision floor after composition; FOW cells carry no emitters; GPU frame-time
  budgets.
- Privacy: another player's inventory-derived emitter is never sent, while the
  viewer's own is; all cells of a block report the same level; edge remainders join
  the adjacent block; levels never exceed 15 in magnitude; moving a single
  uncancelled source within its block leaves its own block's level unchanged.

## Open questions

- Coarse resolution: fixed 3×3 blocks with 16 levels per sign is the most
  private choice that still shows bleed. Content review may prefer 2×2 blocks or
  32 levels.
- Directional faces: cell-wide transmission approximates walls that block only
  some faces. Per-face coefficients would need four nibbles per cell.
- Whether celestial light should also move to the client, using region profile,
  time and authored sky exposure. That would remove the remaining heavy server
  solver, but it requires sending sky-exposure structure for upper levels that
  are not visible.
- Flicker opt-in: an archetype attribute versus a profile flag.
- `light_private`: whether content needs visible objects whose light stays
  server-side, and whether it should be an attribute or an archetype default.

## Follow-up work outside this protocol change

- Move `celestial_light_keyframe_request()` and `celestial_light_ensure()` out
  of the per-cell MAP2 loop into a per-map, per-tick pre-pass.
- Replace the synchronous MAP2 fallback rebuild with serving the previous field
  while a budgeted job rebuilds it. This needs a decision on MAP_RENDERING.md's
  fail-closed contract.
