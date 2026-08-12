# Rendered map lighting and multi-level MAP2

This design contract applies when changing classic server lighting,
`draw_client_map2()`, linked-map depth, or structural camera visibility.

## Lighting

### v1078 radiance contract

Protocol v1078 replaces the v1077 scalar-byte/RGB888 lighting layout as one
transactional change.  It does not retain a parallel RGB8 layout.  The server
serializes one aggregate, viewer-authorized scalar sample and one aggregate RGB
sample for every visible MAP2 sub-layer; it never serializes source positions,
source identities, or ambient and emitter components separately.  Gameplay
LOS, fog authorization, roof/cutaway policy, and hidden-object serialization
are unchanged.

| Item | Normative value |
| --- | --- |
| Unit | One unit is full world daylight: raw 1280. |
| Scalar and RGB sample | Independent network-order `uint16_t`, unsigned Q5.11. |
| Encode raw radiance | `round_half_up(raw * 8 / 5)`, after the aggregate negative clamp. |
| Decode radiance | `sample / 2048`. |
| Finite range | `0..65535`, or `0..31.99951171875` daylight. |
| Existing raw anchors | `0/20/40/80/160/320/640/1280` encode exactly as `0/32/64/128/256/512/1024/2048`. |
| Rounding | For non-negative integers, `round_half_up(n/d) = floor((2*n+d)/(2*d))`; no floating point is permitted in an implementation. |

Thus the least non-zero value is about -11 EV and the finite maximum is just
under +5 EV relative to daylight (about 16 stops total).  The scalar is the
preferred exposure key.  It retains the legacy raw computation and is not a
new visibility authority.  Discrete rendering continues to apply
`light_level_from_raw()` to the scalar projection and ignores RGB.

Authored six-digit colors are sRGB.  The implementation owns one checked
256-entry `srgb8_to_linear_q16[256]` LUT, generated with the IEC 61966-2-1
piecewise transfer and round-half-up to `0..65535`; it must not use platform
`pow()`.  Required LUT vectors are `00=0`, `01=20`, `0a=199`, `30=1937`,
`40=3360`, `60=7666`, `80=14146`, `c0=34544`, `d0=41337`, `e0=48850`,
`ff=65535`.
The inverse final-display transfer is a checked 65,536-entry
`linear_q16_to_srgb8` LUT using the inverse IEC transfer and round-half-up;
`linear 0/65535` round-trip to `00/ff`, and all LUT round trips are within one
8-bit code point.  Tests must check all 256 forward entries and all 65,536
inverse entries against the generator's checked-in vectors, not merely these
sentinels.

For an authored source `(r,g,b)`, decode each channel through that LUT, let
`peak = max(decoded)`, and reject a zero peak for a positive source.  Its
linear normalized vector is `round_half_up(decoded[channel] * 65535 / peak)`.
The existing scalar mask/attenuation is multiplied by that vector, then all
contributions are summed in signed 64-bit accumulators.  Same-cell positive
source grouping and its scalar cap remain authoritative; negative sources
subtract only the scalar-equivalent neutral vector.  After all additions and
subtractions, clamp each aggregate channel to zero.  Quantize once at the
wire boundary.  Checked/saturating helpers are mandatory: signed overflow,
order-dependent saturation, and independently clipping a finite RGB vector
are forbidden. If an RGB vector exceeds the finite raw range, multiply all
three channels by the Q0.16 common gain `65536 / max(rgb)` with round-half-up,
then clamp the one-past-maximum peak to `65535`; this makes the fixed overflow
vectors exact while preserving chromaticity. The scalar is encoded
independently.

The client caches and interpolates the Q5.11 fields before filtering or tone
mapping.  It derives one common gain/shoulder from the interpolated scalar,
using the existing raw anchors as the neutral display response.  That gain is
applied to the complete linear RGB vector, followed by a single gamut clamp,
the inverse LUT, and gamma-aware multiplication with sRGB texture pixels.
Using RGB maximum or luminance as the exposure key is not permitted.  An SDL
RGB-modulation shortcut is permitted only when a test proves a maximum error
of one output code point against this linear reference; otherwise composition
uses the reference path.  This prevents a bright colored vector from becoming
gray while preserving the exact neutral anchor presentation.

The following fixed vectors are normative.  The `raw RGB` column is after
aggregation/negative clamp and before Q5.11 encoding; `words` are the three
big-endian values expected on the wire.  Producer, shared preflight, and
client tests must use these exact words, including every truncated-field
boundary.

| Case | Scalar raw | Raw RGB | Scalar word | RGB words |
| --- | ---: | --- | ---: | --- |
| zero / explicit neutral reset | 0 | `(0,0,0)` | 0 | `(0,0,0)` |
| neutral daylight | 1280 | `(1280,1280,1280)` | 2048 | `(2048,2048,2048)` |
| `ff6030`, daylight plus center strength 80 | 1360 | `(1360,1289,1282)` | 2176 | `(2176,2062,2051)` |
| `60d0ff`, daylight plus center strength 320 | 1600 | `(1317,1482,1600)` | 2560 | `(2107,2371,2560)` |
| white center strength 80 | 80 | `(80,80,80)` | 128 | `(128,128,128)` |
| red + green, each strength 80 | 160 | `(80,80,0)` | 256 | `(128,128,0)` |
| red + blue, each strength 80 | 160 | `(80,0,80)` | 256 | `(128,0,128)` |
| maximum finite | 40959 | `(40959,40959,40959)` | 65535 | `(65535,65535,65535)` |
| common RGB overflow | 40959 | `(81919,40959,20480)` | 65535 | `(65535,32768,16384)` |

The range audit at content revision `81d71a575e0969ebe964893aa7c0ef9bfb63cb16`
found authored colored lights with radii through 9 (the editor light fixtures)
and runtime masks capped by `MAX_LIGHT_SOURCE == 13`; current world/map
darkness anchors top out at raw 1280.  Q5.11 therefore covers normal world,
map-local, floor, vision, negative, and one-mask samples without clipping.
Dense overlap must use the common-vector scale above and is a bounded
aggregate-only disclosure for cells already authorized to the viewer.  It is
not HDR display precision and cannot recover texture detail absent from 8-bit
artwork.

Before implementation lands, the following hard budgets apply: a complete
MAP2 game payload remains at most 65,534 bytes; a full update uses at most
4,096 continuations; each serialized sub-layer uses at most 8 lighting bytes
(2 scalar + 6 RGB); server per-socket lighting cache growth is at most 56 bytes
per map cell; client lighting storage is at most 56 bytes per map cell; the
256-entry Q16 forward LUT plus the 65,536-entry inverse byte LUT consume at
most 66,048 bytes total; and the standard and large-viewport lighting passes
may regress no more than 10% and 15%,
respectively, from their recorded v1077 medians.  CI must measure dense initial
state packets, continuations, both cache sizes, and both frame-time baselines.
Frame-time comparison uses the release `--player-view-benchmark` harness on
the same runner for the v1077 base and candidate, with 5 warmups and the median
of 101 live map draws at 320x240 and 1920x1080; three alternating process
samples are retained in the CI evidence artifact.
The widened client raster sample is at most 10 bytes; each linked-depth context
owns two viewport-sized fields and one row scratch field. Lit-sprite cache
storage remains capped at 8 MiB per retained depth context.

- `src/server/light.c` propagates source masks as spherical 3D volumes across
  horizontal and `TILED_UP`/`TILED_DOWN` links. Opaque cells stop rays after
  receiving light on their exposed face, and a floor on the upper level blocks
  a ray crossing that vertical boundary. Search depth follows `MAP2_MAX_DEPTH`,
  the maximum depth serialized to the client.
- `glow_radius` is a bounded strength/profile selector, not a literal map-cell
  radius. Each profile defines center intensity, support radius, and falloff.
  The default `light_falloff = radial` profile preserves historical center
  intensity and uses integer fixed-point Euclidean distance with monotonic
  linear or squared falloff to zero at the support boundary. Small and medium
  sources gain one support cell, capped at the historical four-cell maximum,
  so smooth client interpolation receives enough samples for a centered pool.
  `light_falloff = legacy` retains the former ring masks for controlled A/B
  comparison; changing the option requires a server restart. Positive,
  colored, and negative sources always share the selected geometry.
  The one-cell expansion adds sampling work for small and medium sources while
  the previous four-cell maximum remains unchanged. Fixed distance lookup
  keeps the new hot path deterministic and bounded.
- Ambient/floor light remains in `MapSpace.light_value`; source contributions
  use `MapSpace.light_source_value` so masks can rebuild when an opaque object
  or floor changes. Apply lighting through `map_get_darkness()` rather than
  reading either component alone.
- Positive sources additionally accumulate normalized authored sRGB `RRGGBB`
  through the canonical scene-linear Q0.16 lookup in signed 64-bit fields.
  `light_radiance_from_raw()` resolves the aggregate Q5.11 scalar exposure
  reference and RGB vector after the existing positive-source grouping cap;
  negative sources remain scalar-only and therefore subtract achromatically.
  The resolver rounds once at the wire boundary and uses common-vector scaling
  on overflow. This keeps insertion order irrelevant, retains capped equal
  red/blue or red/green sources as magenta or yellow, and makes `ffffff`
  reproduce the scalar sample exactly. The v1078 transition removes the
  legacy RGB8 projection instead of retaining a parallel transfer path.
  Ambient, floors, world light, special vision, and `tli` stay neutral;
  negative sources affect only the scalar raw light and are therefore
  achromatic even if an object carries a non-white authored color.
- Map loading defers local source masks until floors/blockers load, then restores
  sources from loaded neighboring levels. Keep load/unload symmetric.
- A player's emitter is derived atomically from one source. Eligible inventory
  emitters are compared by `glow_radius`; the strongest wins, ties use the
  first object in canonical inventory order, and an inventory object wins a
  tie with the player archetype. Applyable lights are eligible only while
  applied. The selected object's radius and color are always copied together.
  Player saves omit this derived pair and reconstruct it from the archetype and
  saved inventory before map insertion, so reconnects and transfers cannot
  persist or briefly display a stale tint.
- Test buildings from outside and inside. Upper floors may own lights, while an
  exterior facade still receives nearby base-map lighting through unobstructed
  3D rays when viewed outdoors.

## Multi-level serialization and visibility

- `src/socket/request.c:draw_client_map2()` serializes each physical linked map
  into its own length-delimited `CLIENT_CMD_MAP` level block. Each depth owns an
  independent `MapCell` delta cache in `socket_struct.lastmap`; never fold upper
  or lower objects into base-map sublayers.
- `src/server/los.c` remains the two-dimensional gameplay LOS/fog authority.
  Its base mask protects actors, items, effects, targeting, and unexplored cells
  across depths. Structural shell/roof visibility is a camera decision in
  `request.c`; do not turn gameplay LOS into a voxel volume.
- Send objects on authoritative layers. Walls/roofs remain `LAYER_WALL`; the
  client uses explicit depth for projection. Send both halves of every
  `draw_double` object regardless of player quadrant.
- Mark doors with `MAP2_FLAG2_DOOR` independently of the generic second-pass
  bit and cache that semantic per socket layer so a type-only change emits a
  delta. Door reveal must not broaden LOS or disclose interiors.
- Mark only serialized, visible `EXIT` objects with `MAP2_FLAG2_EXIT` and cache
  that semantic per socket layer so removal or a type-only change emits a
  delta. The client outlines those objects after the complete world pass only
  at the player's physical depth. This presentation does not broaden line of
  sight or disclose layer-0/system exits, unexplored transitions, or hidden
  objects that the server did not serialize.
- Upper-level visibility is camera-top-down. A solid floor, gameplay-opaque
  cell, or hidden wall-layer roof limits enclosed storeys below to their
  structural boundary without removing a middle-storey exterior wall. A
  covered storey sends only that exterior wall, not its hidden floor/floor mask;
  lower structure must not cut holes in a roof above. Downward visibility is
  blocked by the player's current solid floor/opaque boundary.
- Derive building cutaways from the stack, not authored flags. When the player
  is beneath an upper solid surface/roof, flood-fill that connected overhead
  component in the client viewport and omit it. Include the adjacent one-cell
  wall boundary without recursively following unrelated wall chains. Send
  camera-occluded cells with `MAP2_MASK_HARD_CLEAR` so cached roofs/walls cannot
  reappear grey after re-entry.
- Base-map LOS limits gameplay content on positive depths but must not clear
  their complete cells. Downgrade to structural floor, floor-mask, and wall
  boundary so lower walls do not slice roofs. Stack occlusion decides whether
  positive-depth structure is disclosed.
- Base-depth blocked cells use ordinary fog clear: never-seen cells remain
  empty and seen contents remain grayscale unless a visible roof needs its
  structural column. Send base structural support elevation even when never
  seen so linked visible structure projects correctly, but do not disclose a
  floor face or gameplay content.
- A visible roof may send base floor, floor-mask, and wall layers in its
  structural column with explicit fog state to complete the silhouette while
  withholding actors, items, effects, and interiors.
- Connected UP/DOWN transitions include signed depth offsets so client/server
  shift existing caches rather than forcing a full refresh.
- Protocol v1078 carries Q5.11 scalar words and the
  `MAP2_FLAG_EXT_LIGHT_RADIANCE_RGB16` extension before the animation tail. It
  carries a complete seven-bit sub-layer bitmap followed by ascending RGB
  Q5.11 triples. A zero bitmap explicitly resets all sub-layers to their scalar
  samples. Scalar and RGB caches are independent, so hue-only changes and
  neutral resets emit.
- The first update declares the complete depth set. If its framed level blocks
  would exceed the 65,534-byte game payload, it reserves zero-length blocks for
  omitted depths and sends their complete payloads in deterministic
  `MAP_UPDATE_CMD_PARTIAL` continuation packets. Oversized individual depths
  split only between complete tile records and may repeat their depth in later
  continuations. After the player sub-layer, the full header carries a
  big-endian `uint16` continuation count; each partial carries its one-based
  sequence number in the same position. The client accepts only the exact next
  sequence with matching player coordinates, sub-layer, and a subset of the
  full update's declared depths, and clears pending state after the final
  partial or any cache reset. Continuations never scroll or replace the active
  depth mask, and every packet is independently preflighted before client state
  mutation.
  One 21x21 depth gains at most 9,702 RGB bytes; a dense regression fixture
  forces a previously valid level across the boundary and verifies that every
  resulting packet remains within and passes the shared preflight.
- Do not add authored building/balcony/overlook flags or make serialization
  borrow/zoom magic-mirror targets.

Tests must cover lighting rebuild/load/unload, inside/outside buildings, delta
cache semantic changes, fog versus hard clear, roof silhouettes, disclosure
boundaries, cutaway connectivity, and depth-transition cache shifts.
