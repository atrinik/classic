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

1. **Base radiance**: ambient, floor, world and celestial light. None of these
   is a source the player could locate. This stays Q5.11 with RGB, because it
   describes the environment (time of day, sky exposure, map ambient).
2. **Emitters of visible objects**: glow radius and authored color, sent only
   with an object that MAP2 already sends. Its existence and tile are already
   disclosed by its face, so the emitter adds no new location information.
3. **Coarse local light**: one byte per visible cell record. It is a signed,
   quantized, neutral level of local light, pooled over a fixed 3×3 block. Light
   (and darkness) from hidden sources still bleeds through walls without
   identifying the source tile, count, or hue.
4. **Occluders of visible cells**: blocks-view, floor, roof and door bits plus a
   quantized transmission for geometry the client already sees, so the client
   can occlude and attenuate emitter light.

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
| `MAP2_FLAG2_EMITTER` (2048) | 1 + 3 | signed `glow_radius` clamped to `-13..13` (0 invalid), then authored sRGB `RRGGBB` (`000000` for darkness) |

The existing per-sub-layer Q5.11 lighting bytes carry **base radiance only**
when the server runs client lighting. The 8-byte per-sub-layer budget is
unchanged. Decoders reject an emitter radius of 0 or outside `-13..13`, a local
level of `-16`, and nonzero reserved bits. Rejection happens before any state is
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
- **Vision floor.** XRAY raises presentation light to raw 100 after all light
  is combined. The server therefore sends base radiance without that floor
  and sets bit 7 of the local-light byte for viewers with XRAY. The client then
  applies the same floor after composition. This is a property of the viewer
  only, so it discloses nothing about sources.
- **Emitters** are copied from `glow_radius` and `light_color` of the object
  MAP2 is already sending (the object on that tile, including the viewer's
  derived emitter). Each sent layer caches its last emitter, so a change of
  radius or color alone forces a layer delta even when the face is unchanged.
  Emitters of objects not sent, whether hidden, out of sight, dark-gated,
  plugin-hidden or below the top object of a layer, are never sent.
- **Occluders** are derived from the per-space flags and light geometry MAP2
  already consults, for cells that MAP2 sends. The transmission attenuation is
  the cell-wide minimum of its four horizontal face coefficients
  (`spatial_cell_light_face()`), quantized up to the next attenuation step.
- **No per-player light recompute.** MAP2 reads cached base radiance and per-cell
  scalars. Celestial ensure and keyframe work move out of the per-cell loop into
  one pass per resident map per tick (see Follow-up work).

## Client

The GPU light pass gains an emitter stage before the existing tone/LUT stages:

1. Group the visible emitters of a tile exactly as the server groups a tile's
   `light_source` counters: sum the radii of the tile's sent emitters, then map
   the sum through `light_mask`. When every emitter of a tile is sent, this
   reproduces the server's same-cell cap; when some are not, the client
   underestimates and the coarse level supplies the rest. Upload at most 256
   grouped emitters per viewport (strongest first, ties by row-major cell) as a
   structured buffer, and the occluders as an R8 cell grid per depth.
2. A compute (or full-screen fragment) pass evaluates every light vertex of the
   existing compact light rows. It sums emitter profiles using the same radial
   tables as the server (`light_profiles`, radius ≤ 4, vertical ≤ 4) with a 3D
   DDA through the occluder grid, so it follows the same "stop after the exposed
   face" and floor-crossing rules as `light_path_value()`. Partial transmission
   applies each crossed cell's attenuation.
3. Combine the client emitter sum `E` with the coarse level `C` (raw step 160):
   - `C > 0`: `L = max(E, 160C)`; `C < 0`: `L = min(E, 160C)`; `C = 0`: `L = E`;
   - then clamp `L` into the server's level: at most `160(C + 1) - 1` when
     `C >= 0` and `C < 15`, and at least `160(C - 1) + 1` when `C <= 0` and
     `C > -15`. The client never shows more light, or less darkness, than the
     authoritative level allows.

   The emitter sum carries hue; the coarse term is neutral. Final radiance =
   base + `L`, then the vision floor if bit 7 is set, before the unchanged
   Q16 tone/LUT stage.
4. Recompute when emitters, occluders, coarse levels or the camera change. A
   presentation-only time uniform permits flicker for emitters whose archetype
   opts in, without server traffic.

Using `max()`/`min()` instead of a sum avoids double counting, because the
coarse term already includes visible sources. The cost is underestimating
overlaps of a visible and a hidden source, which is an accepted presentation
trade-off. Remembered (FOW) cells keep their remembered occluders and no live
emitters or coarse light.

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
- **Never sent:** source identity, count, exact tile, color, profile or height
  of anything not itself sent in MAP2.

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
  range; level `-16`; truncated emitter/occluder fields; reconnect reset;
  and a failed parse leaving map state unchanged.
- Server: coarse level equals the block's signed quantized peak for fixtures
  with a hidden source behind a wall; no emitter or occluder for an unsent
  object or cell; an emitter-only change produces a layer delta; gameplay
  darkness gating identical with the flag on and off; the vision-floor bit
  follows XRAY.
- Client: emitter field matches a CPU reference of `light_path_value()` for
  open, walled, roofed, transmissive and multi-depth fixtures; per-tile
  grouping; composition and clamping for positive, negative and zero levels;
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
- Per-socket cache budget: detecting emitter changes needs one 32-bit entry per
  MAP2 layer (49 per cell record, 196 bytes) plus three bytes per record. That
  exceeds MAP_RENDERING.md's 56-byte lighting cache budget, which must be
  revised or replaced by a compact change token.
- Whether celestial light should also move to the client, using region profile,
  time and authored sky exposure. That would remove the remaining heavy server
  solver, but it requires sending sky-exposure structure for upper levels that
  are not visible.
- Flicker opt-in: an archetype attribute versus a profile flag.

## Follow-up work outside this protocol change

- Move `celestial_light_keyframe_request()` and `celestial_light_ensure()` out
  of the per-cell MAP2 loop into a per-map, per-tick pre-pass.
- Replace the synchronous MAP2 fallback rebuild with serving the previous field
  while a budgeted job rebuilds it. This needs a decision on MAP_RENDERING.md's
  fail-closed contract.
