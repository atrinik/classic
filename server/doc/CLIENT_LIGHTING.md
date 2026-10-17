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
below 1085 are rejected at setup, as for every revision. Reconnect clears all
client emitter, occluder and coarse-light state with the map cache.

Per MAP2 cell record (one record per cell and depth). The fields follow the
existing extended-tile data, in this order, after RGB, keyframe and animation
data. They are announced by `ext_flags` bits, because the `MAP2_MASK_*` cell
word already uses bits 6–15 for coordinates:

| Field | Bytes | When | Meaning |
| --- | --- | --- | --- |
| `MAP2_FLAG_EXT_LOCAL_LIGHT` (8) | 1 | changed | bits 0–4: signed level `-15..15` (two's complement; `-16` invalid); bits 5–6 reserved, zero; bit 7: apply the viewer's vision floor after composition |
| `MAP2_FLAG_EXT_OCCLUDER` (16) | 1 | changed | bit 0 blocks view, bit 1 floor, bit 2 roof surface, bit 3 closed door; bits 4–7: transmission attenuation `a` (coefficient `256 - 16a`; 0 is unattenuated) |

Per MAP2 layer (existing `flags2`):

| Field | Bytes | Meaning |
| --- | --- | --- |
| `MAP2_FLAG2_EMITTER` (2048) | 1 + 3 | signed `glow_radius` clamped to `-13..13` (0 invalid), then authored sRGB `RRGGBB`: nonzero for a positive radius, `000000` for a negative one |

The existing per-sub-layer Q5.11 lighting bytes carry **base radiance only**
when the server runs client lighting. The 8-byte per-sub-layer budget is
unchanged. Decoders reject an emitter radius of 0 or outside `-13..13`, a
positive radius with color `000000` (a zero linear peak, which MAP_RENDERING.md
rejects for positive sources), a negative radius with a nonzero color (darkness
is achromatic), a local level of `-16`, and nonzero reserved bits. Rejection happens before any state is
published, exactly as for other malformed MAP2 fields. A server running the
legacy path sends no `LOCAL_LIGHT`, `OCCLUDER` or `EMITTER` fields, and its base
bytes keep their current full-radiance meaning. A 1085 client therefore renders
both modes.

## Server

- **Local masks become scalar-only for gameplay.** With `client_lighting`
  enabled, the positive and color masks, and RGB accumulation for local
  sources, are no longer needed for presentation. A later step stops building
  them, which removes two thirds of local mask tracing (compare #616).
  Celestial fields are unchanged.
- **Coarse local light** is derived each time MAP2 sends a cell, from the
  already maintained per-cell `light_source_value`. No new per-map state is
  needed, and a change anywhere in the block reaches every cell of it on the
  next update:
  - the block is the fixed 3×3 square containing the cell, aligned to map-local
    coordinates (`x - x % 3`, `y - y % 3`) and clipped at the map edge; it never
    reads another map and never loads one;
  - take the strongest positive and the strongest negative local scalar in the
    block; the larger magnitude wins;
  - quantize the magnitude down to 1/8-daylight levels (raw 160), clamp to 15,
    and keep the sign.
- **Viewer overrides.** Base radiance is computed per viewer when MAP2 builds
  its view, so the existing `tli` lighting override is already folded into it.
- **Vision floor.** XRAY raises presentation light to raw 100 after all light
  is combined. The server therefore sends base radiance without that floor
  and sets bit 7 of the local-light byte for viewers with XRAY. The client then
  applies the same floor after composition. This is a property of the viewer
  only, so it discloses nothing about sources.
- **Emitters** are copied from `glow_radius` and `light_color` of the object
  MAP2 is already sending (the object on that tile, including the viewer's
  derived emitter). A negative radius is sent with color `000000`. The
  per-socket cache keeps, per cell, a bitmap of layers that carry an emitter,
  a bitmap of stale emitter layers and a digest of the cell's emitters
  (20 bytes). Gaining or losing an emitter forces a layer delta; a radius- or
  color-only change on a cached layer changes the digest and resends that
  layer on the next update. Emitters of objects not sent, whether hidden, out of sight, dark-gated,
  plugin-hidden or below the top object of a layer, are never sent.
- **Occluders** are derived only from the objects MAP2 sends to this viewer:
  the sent object of each layer, after plugin visibility and hidden-object
  filtering. A sent view blocker, floor, roof surface or closed door sets its
  bit, and a sent object's authored horizontal transmission
  (`celestial_transmission`, or the door's open/closed value) sets the
  attenuation, quantized up to the next step. Space-wide flags such as
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
   cell grid per depth.
2. A compute (or full-screen fragment) pass evaluates every light vertex of the
   existing compact light rows. It sums emitter profiles using the same radial
   tables as the server (`light_profiles`, radius ≤ 4, vertical ≤ 4) with a 3D
   DDA through the occluder grid, so it follows the same "stop after the exposed
   face" and floor-crossing rules as `light_path_value()`. Partial transmission
   applies each crossed cell's attenuation.
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
   saturated to `0..65535` before the vision floor (if bit 7 is set) and before
   the unchanged unsigned Q16 tone/LUT stage. Negative intermediate values never
   reach that stage.
4. Recompute when emitters, occluders, coarse levels or the camera change. A
   presentation-only time uniform permits flicker for emitters whose archetype
   opts in, without server traffic.

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
  the strongest local light in its fixed 3×3 block. All nine cells of a block
  share one value, so a source cannot be located more precisely than its block
  from the peak alone. Hue, count and profile are never sent.
- **Residual leakage, honestly stated:** moving a hidden source within its block
  can still change the levels of neighboring blocks through falloff. Comparing
  neighboring blocks therefore narrows the position somewhat, but only to the
  extent that the falloff crosses a level boundary. Quantization limits this;
  nothing short of hiding the field entirely removes it.
- **Occluders:** only properties of objects MAP2 already sends to the viewer.
- **Never sent:** source identity, count, exact tile, color, profile or height
  of anything not itself sent in MAP2, and no property of hidden or unsent
  geometry.

This is strictly less disclosure than the current contract for hidden sources,
and equal disclosure for visible ones.

## Rollout

1. Protocol 1085, server producer and client decoder, with server config
   `client_lighting` defaulting to off. The legacy path is unchanged, and 1085
   clients render it.
2. Client emitter composition. `client_lighting.c` is the CPU reference: it
   groups emitters per tile, evaluates the shared `light_profile` tables with a
   line walk through the occluder grid, composes by sign and clamps to the
   coarse level. The map sampler adds the result to received base radiance per
   cell sample, before the existing GPU light rows. The line walk approximates
   the server's 3D supercover rays and evaluates one depth.
3. GPU emitter stage replacing the CPU reference, with linked depths, plus
   qualification: GPU frame budgets as in MAP_RENDERING.md, plus offline
   player-view proofs comparing indoor, outdoor and cutaway Brynknot fixtures.
4. Enable `client_lighting` on development, then production.
5. Remove server positive/color local masks and the legacy full-radiance path.
   Update MAP_RENDERING.md to make this document normative.

## Test plan

- Protocol: encode/decode round trips; reserved bits; radius 0 and out of
  range; positive radius with color `000000` and negative radius with a color;
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
- Privacy: all nine cells of a block report the same level; levels never exceed
  15 in magnitude; moving a source within its block leaves its own block's level
  unchanged.

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
