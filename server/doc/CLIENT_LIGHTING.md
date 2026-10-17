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

1. **Base radiance**: ambient, floor, world, special-vision and celestial light.
   None of these is a source the player could locate. This stays Q5.11 with RGB,
   because it describes the environment (time of day, sky exposure, map ambient).
2. **Emitters of visible objects**: profile and authored color, sent only with
   an object that MAP2 already sends. Its existence and tile are already
   disclosed by its face, so the emitter adds no new location information.
3. **Coarse local light**: one byte per visible cell. It is a quantized,
   neutral, spatially pooled scalar of all local source light, so light from
   hidden sources still bleeds through walls without identifying the source.
4. **Occluders of visible cells**: blocks-view, floor, roof and door bits for
   geometry the client already sees, so the client can occlude emitter light.

Gameplay keeps using the full authoritative server light. That includes
darkness gating of live objects in MAP2 (`raw_light <= 0`), infravision,
stealth, and `tli`. Nothing in this design moves gameplay authority to the
client.

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
| `MAP2_FLAG_EXT_LOCAL_LIGHT` (8) | 1 | changed | Coarse local light level `0..15` (high nibble reserved, zero) |
| `MAP2_FLAG_EXT_OCCLUDER` (16) | 1 | changed | bit0 blocks view, bit1 floor, bit2 roof surface, bit3 closed door; bits 4–7 reserved, zero |

Per MAP2 layer (existing `flags2`):

| Field | Bytes | Meaning |
| --- | --- | --- |
| `MAP2_FLAG2_EMITTER` (2048) | 1 + 3 | signed `glow_radius` clamped to `-13..13` (0 invalid), then authored sRGB `RRGGBB` (`000000` for darkness) |

The existing per-sub-layer Q5.11 lighting bytes carry **base radiance only**
when the server runs client lighting. The 8-byte per-sub-layer budget is
unchanged. Decoders reject an emitter radius of 0 or outside `-13..13`, nonzero
reserved bits, and a local level above 15. Rejection happens before any state
is published, exactly as for other malformed MAP2 fields. A server running the
legacy path sends no `LOCAL_LIGHT`, `OCCLUDER` or `EMITTER` fields, and its base
bytes keep their current full-radiance meaning. A 1085 client therefore renders
both modes.

## Server

- **Local masks become scalar-only for gameplay.** With `client_lighting`
  enabled, the positive and color masks, and RGB accumulation for local
  sources, are no longer needed for presentation and are not built. That alone
  removes two thirds of local mask tracing (compare #616). Celestial fields are
  unchanged.
- **Coarse local light** is computed per map cell, not per player, when that
  cell's local scalar changes. It is cached in `MapSpace` and costs one byte:
  - take the maximum local source scalar over the 3×3 cell neighborhood on the
    same depth;
  - quantize to 16 levels of 1/8 daylight (raw 160), rounding down;
  - clamp at level 15.

  MAP2 sends it like any other per-cell field, through the client delta cache.
- **Emitters** are copied from `glow_radius` and `light_color` of the object
  MAP2 is already sending. A player's derived emitter keeps its existing atomic
  selection rules. Emitters of objects not sent, whether hidden, out of sight,
  dark-gated or plugin-hidden, are never sent.
- **Occluders** are derived from the same per-space flags MAP2 already
  consults, for cells that MAP2 sends.
- **No per-player light recompute.** MAP2 reads cached base radiance, coarse
  level and flags. Celestial ensure and keyframe work move out of the per-cell
  loop into one pass per resident map per tick (see Follow-up work).

## Client

The GPU light pass gains an emitter stage before the existing tone/LUT stages:

1. Upload the visible emitters as a structured buffer. At most 256 per
   viewport; beyond that, the strongest win, ties broken by row-major cell.
   Upload occluders as an R8 cell grid per depth.
2. A compute (or full-screen fragment) pass evaluates every light vertex of the
   existing compact light rows. It sums emitter profiles using the same radial
   tables as the server (`light_profiles`, radius ≤ 4, vertical ≤ 4), with a 3D
   DDA through the occluder grid. That gives the same "stop after the exposed
   face" and floor-crossing rules as `light_path_value()`.
3. Local radiance per vertex = `max(emitter_sum, coarse_local)` in scene-linear
   scalar. The emitter sum carries hue; the coarse term is neutral. The final
   vertex radiance = base + local, before the unchanged Q16 tone/LUT stage.
4. Recompute when emitters, occluders, coarse levels or the camera change. A
   presentation-only time uniform permits flicker for emitters whose archetype
   opts in, without server traffic.

Using `max()` instead of a sum avoids double counting, because the coarse term
already includes visible sources. The cost is underestimating overlaps of a
visible and a hidden source, which is an accepted presentation trade-off.
Remembered (FOW) cells keep their remembered occluders and no live emitters.

## Privacy argument

What a modified client can learn, beyond today's MAP2:

- **Visible emitters:** profile and color of objects it already sees. This is
  new, but it concerns only already-authorized objects.
- **Hidden light:** for each visible cell, the 3×3-pooled, 16-level, neutral
  maximum of local light. Pooling removes single-cell peaks, so a source is
  located only to within its pooled footprint. Quantization removes the
  fine gradient that today's Q5.11 exposes. Neutral output removes hue, so a
  red magic light and a torch are indistinguishable. Negative (darkness)
  sources only lower the level; they cannot be told apart from the absence of
  light.
- **Never sent:** source identity, count, exact tile, color, profile or height
  of anything not itself sent in MAP2.

This is strictly less disclosure than the current contract for hidden sources,
and equal disclosure for visible ones.

## Rollout

1. Protocol 1085, server producer and client decoder, with server config
   `client_lighting` defaulting to off. The legacy path is unchanged, and 1085
   clients render it.
2. Client GPU emitter stage plus qualification: GPU frame budgets as in
   MAP_RENDERING.md, plus offline player-view proofs comparing indoor, outdoor
   and cutaway Brynknot fixtures.
3. Enable `client_lighting` on development, then production.
4. Remove server positive/color local masks and the legacy full-radiance path.
   Update MAP_RENDERING.md to make this document normative.

## Test plan

- Protocol: encode/decode round trips; reserved bits; radius 0 and out of
  range; a level above 15; truncated emitter/occluder fields; reconnect reset;
  and a failed parse leaving map state unchanged.
- Server: coarse level equals pooled and quantized local scalar for fixtures
  with a hidden source behind a wall; no emitter or occluder for an unsent
  object or cell; gameplay darkness gating identical with the flag on and off.
- Client: emitter field matches a CPU reference of `light_path_value()` for
  open, walled, roofed and multi-depth fixtures; `max()` composition; FOW cells
  carry no emitters; GPU frame-time budgets.
- Privacy: a fixture with one hidden torch at each of the 9 tiles of a 3×3
  block behind a wall produces identical coarse levels on the visible side.

## Open questions

- Coarse resolution: 3×3 pooling with 16 levels is the most private choice
  that still shows bleed. Content review may prefer 2×2 pooling or 32 levels.
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
