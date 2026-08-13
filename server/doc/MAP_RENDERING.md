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
storage remains capped at 8 MiB per retained depth context. Each entry is
charged its actual surface pitch times height plus a conservative 512-byte
entry/surface/allocator allowance, and no context retains more than 8,192
entries, so tiny sprites cannot bypass the byte cap through metadata overhead.

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

### Stacked celestial-lighting contract

This section freezes celestial-lighting schema version 1.  It is the sole
contract for structural sky exposure, regional celestial environments, and
their contribution to the v1078 radiance aggregate.  The server, content
validators, migration tools, and external editors must use the same meanings.
Implementation and bulk content migration are deliberately separate changes.

#### Pinned legacy inventory

The audit below used Classic revision
`e791a3b37103f74c83d5e06283dc9d9474c81291` and `content@main` revision
`65a88167d3a2bedcc2dc21508d94d7ca009a76d2`.  Re-running a migration against a
different revision requires a new inventory; these counts are evidence, not a
schema default.

| Legacy input | Pinned result | Celestial-v1 disposition |
| --- | ---: | --- |
| Map files | 3,651 | Every migrated map receives `sky_above`. |
| Map `outdoor 1` | 2,953 | Removed as lighting authority; never copied to a per-cell toggle. |
| Maps without `outdoor 1` | 698 | Not evidence of cover. |
| Object/archetype `outdoor` | 0 | `FLAG_OUTDOOR`/`P_OUTDOOR` are removed from lighting and from the authored schema. |
| Explicit map `darkness` | 847 | Values were `-1:419`, `1:81`, `2:198`, `3:134`, `4:5`, `5:4`, `7:6`; migrate only under the staged rules below. |
| Explicit map `light` | 0 | Reused as the sole v1 neutral map-ambient record. |
| Maps without a `region` header | 2,136 | Resolve explicitly to `world`; an unknown authored region is invalid. |
| `map_info` objects | 1,044 in 261 maps | Never structural authority. |
| `map_info.item_power` | 20 in 8 maps | Values were `-2:13`, `2:3`, `3:1`, `5:3`; replace explicitly. |
| `map_info` `cursed 1` | 545 in 191 maps | Presentation/world-maker metadata only; never means building or indoors. |
| Regions | 56 | Resolve one effective celestial profile per region. |

Of the outdoor maps, 163 also author `darkness`; 2,790 do not.  Of the maps
without `outdoor 1`, 684 author `darkness`; 14 do not.  This conflicting
prevalence is why neither value is an implicit fallback.

The Greyton luxury house is the required real mixed-map measurement.  Its two
48x34 maps have canonical logical IDs
`/shattered_islands/strakewood_island/greyton/house/luxury_house_0_0` and
`/shattered_islands/strakewood_island/greyton/house/luxury_house_0_0_1`; they contain
3,264 cells in total.  Both say
`outdoor 1` and `darkness -1`; the ground map has a 34x16, 544-cell
`map_info`/`cursed` rectangle, and the upper map has 543 authored roof objects.
The maps do not have a reciprocal `TILED_UP`/`TILED_DOWN` structural link.
Consequently the rectangle is not a building boundary and the pair does not
yet prove a valid stack.  Content migration must author a valid linked stack
or typed exceptions before celestial lighting is enabled for it.

#### Authored structural records

Every map must contain exactly one of these header records:

```text
celestial_schema 1
sky_above open
sky_above linked
sky_above sealed
```

`celestial_schema 1` is required exactly once; the three `sky_above` lines are
alternatives, not simultaneous records.  The version distinguishes migrated
content and swap files from legacy headers.

`open` asserts that the map is the resolved top of this stack and forbids a
`tile_path_9` (`TILED_UP`) link.  `linked` requires an aligned, reciprocal
`TILED_UP`/`TILED_DOWN` pair whose target is loaded and resolved.  Celestial-v1
defines no immutable-summary encoding; adding one requires a new schema
version and exact bitmap, transform, onward-link, revision, and digest vectors.
`sealed` says that unmodelled solid cover exists above the whole map and
forbids `TILED_UP`; typed `open` exceptions may cut apertures in that virtual
cover.  A missing record is an error.  An unloaded or missing target, bad
reciprocal link, coordinate/dimension mismatch, cycle, or traversal beyond
`MAP2_MAX_DEPTH` is an unresolved stack and fails closed as covered.

The shared structural-boundary predicate is true for a cell containing any of:

- a solid floor (`FLAG_IS_FLOOR`) above the contents being classified;
- a static gameplay-opaque vertical surface (`FLAG_BLOCKSVIEW`), a door
  aperture regardless of its current state, or an explicit `glass`/`grate`
  transmissive surface; or
- an explicit hidden `LAYER_WALL` roof/camera surface with `sky_boundary 1`.

`sky_boundary` is valid only on a hidden `LAYER_WALL` archetype.  It is a
validation error on another layer or on an object whose lifecycle is dynamic.
Ordinary floors, opaque walls, and roofs have implicit opaque transmission;
authors do not need to duplicate `sky_boundary` on those already classified
by the shared predicate.  Sprite pixels, object names, paths, map naming,
`map_info`, and cutaway/presentation rectangles never enter the predicate.

Boundaries are oriented faces, not whole blocking cells.  A solid floor,
hidden roof, or virtual cover owns the horizontal face below its cell: it
receives light on top and applies transmission only to the downward crossing.
It does not attenuate a horizontal ray travelling across that exposed top
face.  A wall, door, window, grate, or railing owns vertical exit faces.  Its
optional `celestial_faces` is either `all` or a comma-separated subset in
canonical `N,E,S,W` order; absent means `all`.  On a cardinal scan, a boundary
cell receives incoming light and its exit face in the travel direction applies
the coefficient to the next cell.  A diagonal exit applies the minimum of its
two cardinal exit faces.  `celestial_faces` is invalid on a horizontal
floor/roof.  Thus consecutive outdoor floor cells transmit horizontal sun
unchanged, while the same floors block light to the aligned depth below.

When a physical stack cannot express a local aperture or virtual cover, an
author may place a typed rectangular exception:

```text
arch sky_exposure
x X
y Y
hp WIDTH_MINUS_ONE
sp HEIGHT_MINUS_ONE
sky_state open
end
```

`sky_state` is exactly `open` or `covered`; the rectangle is inclusive from
`(x,y)` through `(x+hp,y+sp)`.  Validate with subtraction before addition:
`0 <= x < width`, `0 <= y < height`, `0 <= hp < width-x`, and
`0 <= sp < height-y`.  Rectangles must not overlap and are absolute rather
than XOR toggles.  `covered` creates a virtual opaque boundary immediately
above the selected cells.  `open` may cut only a
`sky_above sealed` virtual cover; it cannot pierce a real boundary or excuse an
unresolved `linked` stack.  An exception is mechanically invalid when the
currently resolved physical stack already supplies the same boundary or
aperture for every cell in its rectangle.  A migration review should also
prefer ordinary linked structure when a map is reasonably authorable, but
that preference is a lint, not a validator guess.  This keeps exceptions
narrow, reviewable, and removable without making tools infer author intent.

Legacy ambient migration uses separate non-structural records.  The existing
optional map header `light RAW` becomes the sole neutral map ambient in
`0..40959`; absence means zero, and a duplicate is invalid.  The v1 loader and
saver reject/remove legacy `darkness` and `outdoor`, and the saver emits
`light` exactly once only when non-zero.  Map swap save/reload uses those same
v1 rules, so parse order cannot select between legacy and v1 ambient.  A
reviewed rectangular override is:

```text
arch ambient_light_zone
x X
y Y
hp WIDTH_MINUS_ONE
sp HEIGHT_MINUS_ONE
ambient_strength RAW
end
```

It uses the same inclusive, in-bounds, non-overlap rules and replaces, rather
than adds to, the map ambient within its rectangle.  `ambient_strength` is
neutral raw radiance in `0..40959`.  Ambient records never affect exposure,
structure, transmission, region identity, or authored colored emitters.

`sky_exposure` and `ambient_light_zone` are dedicated static system-metadata
records, not `MAP_INFO` and not gameplay objects.  At load the map loader
validates and consumes them into immutable map metadata before object insertion.
They have no live object identity, layer, animation, plugin handle, or runtime
mutation path and are never considered by LOS or serialized by MAP2.  Canonical
map save and swap-save re-emit the authored rectangles in `(y,x,type)` order;
reload must reproduce the same metadata digest.  This preserves editor
round-trips without disclosing classification to clients.

Every structural boundary or aperture resolves one transmission class.  The
authored archetype field is `celestial_transmission`; its exact values are
shown below.  A coefficient is a `uint16_t` integer scaled by 256 in
`0..256`, not an 8-bit Q-format value.  Multiplication is
`round_half_up(value * coefficient / 256)` once per crossed boundary with a
checked 64-bit intermediate.

| Surface/state | Stable exposure role | Transmission | Integer / 256 |
| --- | --- | --- | ---: |
| Ordinary wall, solid floor, hidden roof | boundary | `opaque` | 0 |
| Closed opaque door | unchanged door aperture | `opaque` | 0 |
| The same open door | unchanged door aperture | `open` | 256 |
| Window or glass roof | boundary | `glass` | 192 |
| Grate or transmissive railing | boundary | `grate` | 224 |
| Empty aperture | no added boundary | `open` | 256 |
| Balcony floor | boundary below the balcony | `opaque` | 0 |

Opaque predicate matches default to `opaque`; every non-opaque surface must
author its class.  The field is invalid on an object that is neither a
structural surface nor a dynamic aperture.  A static solid floor cannot be
`open`, and only an actual dynamic aperture may switch classes at runtime.  If
several surfaces occupy one crossed edge, take their minimum coefficient so
object insertion order cannot weaken a blocker.

Only an object of exact type `DOOR` is a dynamic aperture in v1.  Its authored
closed class must be `opaque`; `FLAG_DOOR_CLOSED` selects coefficient 0 and its
absence selects coefficient 256.  No other type or script may mutate
transmission.  Its stable boundary/exposure classification never changes.
Thus opening one door admits a bounded beam and spill; it cannot reclassify
the enclosed room or building as outdoors.  A balcony's floor covers aligned
contents below while its railing may use `grate`; the balcony's own contents
are not covered merely because their current-level base floor exists.

#### Stack resolution and exposed faces

Stack resolution uses the same integer coordinate transform and reciprocal
link checks as multi-level MAP2.  Depth zero is the classified map; positive
depth is above it and negative depth below it.  Resolve no more than depths
`-MAP2_MAX_DEPTH..+MAP2_MAX_DEPTH`, thirteen maps at the current depth limit.
Every referenced map must be resident for construction; v1 treats an unloaded
upper target as unresolved rather than guessing from its path or last cache.

For each aligned column, scan from the highest resolved depth downward.  The
first structural boundary is the highest relevant boundary.  Its exposed face
may receive celestial light.  Objects strictly below that face are covered,
and the coverage continues through every lower linked level until a crossed
boundary or aperture transmits light.  A lower boundary cannot punch a hole in
an upper roof.  An ordinary floor on the current base level does not cover the
contents resting on that same face.  These are structural rules and are
independent of camera cutaway and gameplay LOS.

Horizontal `tile_path_1..tile_path_8` seams use the existing coordinate
transform.  Each crossed seam must be reciprocal and must declare, on both
maps, one of:

```text
celestial_boundary_N continuous
celestial_boundary_N discontinuous
```

For `continuous`, the two effective regional profile digests must match and
the field crosses the seam.  For `discontinuous`, both sides must declare the
opposing edge discontinuous; transport stops at the seam and each destination
map evaluates its own celestial field.  Vertical `tile_path_9`/`tile_path_10`
links have the same declaration and profile rule, in addition to their stack
alignment checks.  Missing, asymmetric, or contradictory declarations fail
validation.  A profile difference is local to that seam and never invalidates
unrelated maps globally.  Structural coverage still crosses a discontinuous
vertical seam: evaluating the lower profile locally cannot turn cells below an
upper boundary into sky-exposed cells.

At a discontinuous vertical seam, upper-profile radiance stops.  The lower map
evaluates its own profile at the same absolute `todtick` and injects that local
component at the seam, then applies the upper horizontal face's transmission:
`opaque` yields zero, `glass` yields `round_half_up(local*192/256)`, `grate`
yields `round_half_up(local*224/256)`, and an explicit open aperture yields the
full local value.  This interface injection is not stable sky exposure.  It
continues downward under the ordinary highest-boundary rule.  The stack cache
key contains the ordered profile digest for every depth, never one digest for
the whole stack.

#### Bounded field construction

The implementation constructs one shared field per resolved vertical stack,
never one field for an unbounded horizontal component and
never one emitter per sky-exposed tile.  More precisely, the cache manager
stores one shared field under the ordered lowest-to-highest tuple of canonical
map path, content revision, and transform; every loaded member references that
one tuple, so a thirteen-map stack is never retained thirteen times.  A v1 map
is at most 64x64; a larger map fails schema validation.  Stable exposure and
transmission are separate arrays.

Diffuse sky and starlight seed every sky-exposed face with their local-profile
value.  Directional sun and moon instead use scan lines in source-to-destination
order, with no per-cell reseed.  A ray entering the upstream 36-cell halo seeds
the applicable unattenuated local-profile direct value when its first face is
sky-exposed, otherwise zero.  For each cell, record the incoming value on the
exposed face, then multiply the outgoing value by the crossed surface's
coefficient.  Opaque therefore receives incoming light but emits zero; glass,
grates, and open doors emit their attenuated value.  Covered cells never
create a fresh seed.

After any coefficient below 256, suppress sky reseeding for the next 32 ray
steps.  On step 33, a sky-exposed face reseeds the unattenuated local direct
value; a covered face remains zero/attenuated.  The blocking cell is step zero,
so cells 1 through 32 behind an opaque wall are zero before softening and cell
33 returns to full direct light.  For diagonal rays, one supercover move is one
ray step and crosses both cardinal edges.  This is the exact 32-cell reach;
there is no Euclidean-distance alternative.

In an aligned vertical column, only the highest relevant boundary face seeds.
Open cells above a lower surface are transport nodes.  The value crossing each
lower face follows the same coefficient and rounding rule.

Diagonal sweeps use a supercover step.  A diagonal crosses both adjacent
cardinal edges; its coefficient is the minimum of those edge coefficients,
so two touching opaque corners never leak.  Ties use north before south and
west before east solely to make traversal reproducible; max-composition makes
the final field independent of that tie order.

Diffuse sky, starlight, and direct light that has crossed an aperture receive
exactly four relaxation passes, never an unbounded flood fill.  Each cell
takes the maximum of its prior value and transmitted neighbours, not their
sum.  Cardinal spill uses `192/256`; diagonal spill uses `181/256` after the
same corner minimum.  The fixed pass count bounds spill to four cells and
prevents area amplification.  Direct sun and moon are directional; diffuse
sky and starlight are not.  Insertion order, object order, load order, and
thread scheduling cannot change the result.

Every relaxation is a Jacobi pass: read an immutable prior buffer and write a
distinct next buffer.  For each neighbour, multiply its prior value by the
edge/corner transmission first, then by the cardinal or diagonal spill factor,
rounding half up after each integer-scaled-by-256 multiplication.  The next
value is the maximum
of the cell's prior value and all candidate values.  Swap buffers only after
all cells finish.  In-place propagation is forbidden because it can travel
more than one cell per pass and depend on traversal order.

The complete construction is O(number of resolved cells): classification, at
most two direct sweeps (sun and moon), and exactly four fixed spill passes for
each non-zero component.  It is forbidden to expand sky-exposed cells into
light-source objects or to use an open-ended queue.

A `continuous` horizontal seam supplies read-only authored structure and phase
inputs through a transient
36-cell halo: 32 cells for direct reach plus four for spill.  The halo may
cross further reciprocal continuous seams, but clips at the exact coordinate
radius and never transfers cache ownership.  At 64x64 and thirteen depths its
bounding envelope is at most 136x136x13, or 240,448 sampled cells, regardless
of the size of the tiled world.  Only the 64x64x13 output cells are retained.
A blocker invalidates output fields whose clipped halos contain it; profile
differences at a discontinuous seam cannot fan out beyond either owner.  This
makes a continuous two-map fixture byte-identical to a single-map sweep while
keeping Creation-sized horizontal tilings out of one cache/invalidation
domain.

#### Calendar and celestial model

`todtick` is the existing absolute count of gameplay hours.  Celestial logic
reads it but never changes it.  Spawns, scripts, sounds, clocks, persistence,
and the rest of gameplay continue to use the global clock.  The exact calendar
constants are 24 hours/day, 7 days/week, 4 weeks/month, 12 months/year: 672
hours/month and 8,064 hours/year.

The root solar elevation track uses `int32_t` integers scaled by 32768 in the
inclusive range `-32768..32768`; it is not stored in a signed 16-bit Q-format.
A positive value is above the horizon, zero is on it, and a negative value is
below it.  Source azimuth names
the compass direction from which light arrives; the scan travels in the
opposite direction.  The same track and bins are the moon's orbit track when
indexed by `moon_hour`.  This table is the normative vector for every sun and
moon direction bin.  `N,NE,E,SE,S,SW,W,NW` map exactly to the existing
direction integers `1,2,3,4,5,6,7,8`.

| Hour | Source | Elevation / 32768 | Summer direct sun | Diffuse/twilight | Solar total |
| ---: | --- | ---: | ---: | ---: | ---: |
| 0 | N | -32768 | 0 | 0 | 0 |
| 1 | N | -31651 | 0 | 0 | 0 |
| 2 | NE | -28378 | 0 | 0 | 0 |
| 3 | NE | -23170 | 0 | 0 | 0 |
| 4 | NE | -16384 | 0 | 0 | 0 |
| 5 | E | -8481 | 0 | 16 | 16 |
| 6 | E | 0 | 0 | 64 | 64 |
| 7 | E | 8481 | 248 | 130 | 378 |
| 8 | SE | 16384 | 480 | 192 | 672 |
| 9 | SE | 23170 | 679 | 245 | 924 |
| 10 | S | 28378 | 831 | 286 | 1,117 |
| 11 | S | 31651 | 927 | 311 | 1,238 |
| 12 | S | 32768 | 960 | 320 | 1,280 |
| 13 | S | 31651 | 927 | 311 | 1,238 |
| 14 | S | 28378 | 831 | 286 | 1,117 |
| 15 | SW | 23170 | 679 | 245 | 924 |
| 16 | SW | 16384 | 480 | 192 | 672 |
| 17 | W | 8481 | 248 | 130 | 378 |
| 18 | W | 0 | 0 | 64 | 64 |
| 19 | W | -8481 | 0 | 16 | 16 |
| 20 | NW | -16384 | 0 | 0 | 0 |
| 21 | NW | -23170 | 0 | 0 | 0 |
| 22 | NW | -28378 | 0 | 0 | 0 |
| 23 | N | -31651 | 0 | 0 | 0 |

Season multiplies elevation, not the clock or azimuth.  Month indices `0..11`
use unsigned 16-bit integers scaled by 32768:
`24576,25600,27648,29952,31744,32768,32768,31744,29952,27648,25600,24576`.
`solar_hour = local_solar_phase`, `season_month =
floor(local_season_phase / 672)`, and `lunar_age = local_lunar_phase`.
Multiply the absolute track value by the factor, round half up over 32768, and
restore the sign using checked 64-bit intermediates.  The seasonal factor
applies only to solar elevation; lunar orbit elevation always uses the
unscaled 24-hour track.  Summer means month 5 or 6.  At local noon the resulting
solar totals by month are exactly
`976,1014,1090,1176,1242,1280,1280,1242,1176,1090,1014,976`.

For positive seasonal elevation `e`, direct sunlight is
`round_half_up(960*e/32768)` and diffuse sky is
`64 + round_half_up(256*e/32768)`.  At `e == 0`, diffuse sky is 64.  At the one
twilight sample `e == -round_half_up(8481*season/32768)`, diffuse sky is 16;
at lower samples it is zero.  Direct sun is zero at and below the horizon.
These components add before structural transmission.  Root summer noon is
therefore exactly raw 1,280, the full-daylight anchor.

The root lunar synodic period is exactly 672 hours, and absolute `todtick == 0`
is new moon.  Let `L` be the effective authored period and `a` the local lunar
age in `0..L-1`; validation requires `L` to be a multiple of 24 and 8.  The
illuminated fraction is unsigned Q0.16:

```text
a <= L/2: illumination = round_half_up(a * 65535 / (L/2))
a >  L/2: illumination = round_half_up((L-a) * 65535 / (L/2))
elongation_bin = floor(a * 24 / L)
moon_transit = (12 + elongation_bin) mod 24
moon_hour = (solar_hour - moon_transit + 12) mod 24
```

Index the shared elevation/azimuth table by `moon_hour`.  Moonlight is zero at
new moon, when its elevation is at or below zero, or when `moon_max` is zero.
The lifecycle `visible` bit is exactly `moon_elevation > 0`, independent of
illumination; it is zero at both horizon samples and below, even though a new
moon can have `visible == 1` while contributing zero.
Otherwise its raw direct strength is evaluated with one rational and one final
rounding using a checked unsigned 64-bit numerator and denominator.  The
maximum numerator is `20*65535*32768 == 42,949,017,600`, so 32-bit arithmetic
is forbidden:

```text
round_half_up(moon_max * illumination * moon_elevation /
              (65535 * 32768))
```

The root `moon_max` is 20, exactly 1/64 of full daylight.  The coherent orbit
therefore makes new moon follow the sun, full moon oppose it, and waxing and
waning quarters occupy different windows; there is no unrelated hourly moon
table multiplied by phase.  Horizon samples are visibility boundaries but
have zero direct radiance.

| Phase | Root age | Illumination | Transit | Rise | Set | Direct at transit |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| new | 0 | 0 | 12 | 6 | 18 | 0 |
| waxing crescent | 84 | 16,384 | 15 | 9 | 21 | 5 |
| first quarter | 168 | 32,768 | 18 | 12 | 0 | 10 |
| waxing gibbous | 252 | 49,151 | 21 | 15 | 3 | 15 |
| full | 336 | 65,535 | 0 | 18 | 6 | 20 |
| waning gibbous | 420 | 49,151 | 3 | 21 | 9 | 15 |
| last quarter | 504 | 32,768 | 6 | 0 | 12 | 10 |
| waning crescent | 588 | 16,384 | 9 | 3 | 15 | 5 |

For the below-horizon reference, full moon at solar hour 12 has
`moon_hour == 0`, elevation -32768, source N, and raw moonlight 0.  At solar
hour 0 it has `moon_hour == 12`, elevation 32768, source S, and raw moonlight
20.  The 24-row orbit table above is also tested at each moon hour for all
eight phase anchors, which covers every lunar direction and both visibility
edges.

Root starlight is raw 2, exactly 1/640 full daylight, non-directional, and uses
the fixed four-pass spill.  It is emitted only when the solar total is zero;
it is independent of moon visibility and adds to any visible moonlight.  It
does not appear during the raw-16 twilight sample.

#### Regional celestial environments

The existing `region_struct::parent` hierarchy owns celestial profiles.
`regions.reg` accepts the following versioned records; values shown are the
complete required root defaults:

```text
celestial_schema 1
celestial_solar_mode global
celestial_solar_rate 1/1
celestial_solar_epoch 0
celestial_solar_phase 0
celestial_season_mode global
celestial_season_rate 1/1
celestial_season_epoch 0
celestial_season_phase 0
celestial_lunar_mode global
celestial_lunar_rate 1/1
celestial_lunar_epoch 0
celestial_lunar_phase 0
celestial_lunar_period 672
celestial_day_color ffffff
celestial_night_color 6080c0
celestial_day_brightness 256
celestial_night_brightness 256
celestial_moon_color c0d0ff
celestial_moon_max 20
celestial_starlight_color 6080c0
celestial_starlight_strength 2
```

Each absent child field inherits independently.  There is no textual `null` or
`inherit` value, and a duplicate or unknown `celestial_*` key is an error.  The
registry must contain exactly one parentless region.  At the pinned content
revision its stable identity is `world`; that region is the semantic root and
must explicitly define every field.  Every other region has one known parent,
all region names are unique, and every parent chain must terminate at `world`.
Unknown parents, self-parenting, duplicate names, and any parent cycle reject
the complete registry.  Resolve inheritance only after this graph check, then
validate each effective profile; an invalid combination rejects the complete
region registry.  A map with no `region` header resolves explicitly to
`world`; a non-empty unknown region name is invalid.

Solar, season, and lunar controls are separate.  Their periods `P` are 24,
8,064, and `celestial_lunar_period`, respectively.  A mode has these exact
constraints:

| Mode | Rate | Epoch | Phase | Meaning |
| --- | --- | ---: | --- | --- |
| `global` | `1/1` | 0 | 0 | Exact `todtick mod P`. |
| `scaled` | reduced `N/D`, each `1..16` | unsigned 64-bit | `0..P-1` | Deterministic rational local time. |
| `fixed` | `0/1` | 0 | `0..P-1` | The authored phase never advances. |

For `scaled`, compute without signed or unsigned wrap:

```text
M = P * D
t = todtick mod M
e = epoch mod M
delta = (t >= e) ? (t-e) : (M-(e-t))
local_phase = (phase + floor(delta * N / D)) mod P
```

Intermediate multiplication is checked unsigned 64-bit.  The limits make it
safe for the defined periods; overflow, a non-reduced fraction, zero
denominator, out-of-range phase, or mode/rate mismatch rejects the registry.
The epoch-ahead vector `P=24,N/D=1/2,todtick=3,epoch=5,phase=0` has `M=48`,
`delta=46`, and local phase 23; implementations must not rely on unsigned
subtraction wrap.
The lunar period range is `168..8064`, inclusive, and it must be divisible by
24 and 8.  Changing the solar rate or phase changes sun/sky only; season and
lunar age retain their independently resolved controls.  None changes the
authoritative gameplay `todtick`.

Colors are exactly six unprefixed hexadecimal sRGB digits and decode through
the canonical scene-linear LUT above.  Brightness values are unsigned Q8.8 in
`0..1024`, inclusive (0x through 4x); zero is meaningful.  `moon_max` and
`starlight_strength` are raw radiance in `0..20` and `0..2`, respectively.
These upper bounds are the maximum permitted moonlight and starlight vectors;
neither regional override may approach daylight.

For solar direct, diffuse, and twilight, interpolate the effective normalized
night and day color vectors and brightness values in scene-linear space with
`p = clamp(seasonal_elevation, 0, 32768)`:

```text
B = round_half_up((night_brightness * (32768-p) +
                   day_brightness * p) / 32768)
C[channel] = round_half_up((night_color[channel] * (32768-p) +
                            day_color[channel] * p) / 32768)
solar_scalar = round_half_up((transported_direct + transported_diffuse) *
                             B / 256)
solar_rgb[channel] = round_half_up(solar_scalar * C[channel] / 65535)
```

Direct and diffuse remain separate scalar fields through their distinct sweep,
transmission, and spill rules.  At each output cell, the displayed order and
roundings are mandatory: sum the two transported scalars, scale that sum once,
then project the rounded scalar through the interpolated normalized color.
Do not color or round the two components independently.  Moon and star scalar
fields are projected through their own colors after transport and then added
to the RGB aggregate.  These are full-replacement celestial endpoints, not
tints over
the root colors.  Moon and stars use their own full-replacement colors and raw
strengths; they are not multiplied by day/night brightness.  No celestial
profile color or brightness modifies authored local emitters.

An effective profile's identity is the lowercase SHA-256 of this exact UTF-8,
LF-terminated serialization: prefix `atrinik-celestial-profile-v1`, followed
by every effective field in the order shown in the root block as
`name=value`, with rates reduced and colors lowercase.  The final line also
has LF.  Map links compare the full 32-byte digest, never a prefix.

At absolute `todtick == 18`, the root phases are solar 18, season 18, lunar 18.
A solar-only `1/2x` profile (`scaled`, rate `1/2`, epoch 0, phase 0) resolves
solar 9; a solar-only `2x` profile resolves solar 12.  In both, season and
lunar phase remain 18.  A fixed-full-moon profile uses fixed solar phase 12,
fixed season phase 3,360 (month 5 start), and fixed lunar phase 336.  The exact
profile-digest vectors appear with the lifecycle vectors below.

#### Aggregate radiance vectors

Celestial scalar and RGB values are aggregate-only inputs to the existing
signed 64-bit v1078 accumulation.  Map ambient, floor radiance, special
vision, positive/colored local sources, and negative neutral sources retain
their existing meanings.  Add all transmitted celestial components and local
components, apply negative neutral sources, clamp once, and quantize once at
the wire boundary.  Celestial-v1 never exposes a component, source position,
profile, or structural classification to an unauthorized viewer.

The normalized linear vectors for the root colors are day
`(65535,65535,65535)`, night/stars `(14544,26837,65535)`, and moon
`(34544,41337,65535)`.  The warm replacement `ffe0c0` is
`(65535,48850,34544)`.  These come from the mandatory sRGB LUT and normalization
rule above.  The following values are exact before and after Q5.11 encoding.

| Case | Scalar raw | Raw RGB | Scalar word | RGB words |
| --- | ---: | --- | ---: | --- |
| root dawn/dusk horizon | 64 | `(14,26,64)` | 102 | `(22,42,102)` |
| root twilight | 16 | `(4,7,16)` | 26 | `(6,11,26)` |
| root starlight only | 2 | `(0,1,2)` | 3 | `(0,2,3)` |
| neutral summer noon | 1,280 | `(1280,1280,1280)` | 2,048 | `(2048,2048,2048)` |
| warm `ffe0c0` summer noon | 1,280 | `(1280,954,675)` | 2,048 | `(2048,1526,1080)` |
| cool `6080c0` summer noon | 1,280 | `(284,524,1280)` | 2,048 | `(454,838,2048)` |
| zero brightness summer noon | 0 | `(0,0,0)` | 0 | `(0,0,0)` |
| dim brightness 128 summer noon | 640 | `(640,640,640)` | 1,024 | `(1024,1024,1024)` |
| bright brightness 512 summer noon | 2,560 | `(2560,2560,2560)` | 4,096 | `(4096,4096,4096)` |
| night brightness 0 twilight | 0 | `(0,0,0)` | 0 | `(0,0,0)` |
| night brightness 128 twilight | 8 | `(2,3,8)` | 13 | `(3,5,13)` |
| night brightness 512 twilight | 32 | `(7,13,32)` | 51 | `(11,21,51)` |
| full moon, no stars | 20 | `(11,13,20)` | 32 | `(18,21,32)` |
| full moon plus stars | 22 | `(11,14,22)` | 35 | `(18,22,35)` |
| quarter moon plus stars | 12 | `(5,7,12)` | 19 | `(8,11,19)` |
| crescent moon plus stars | 7 | `(3,4,7)` | 11 | `(5,6,11)` |
| gibbous moon plus stars | 17 | `(8,10,17)` | 27 | `(13,16,27)` |
| noon plus warm local `ff6030` strength 80 | 1,360 | `(1360,1289,1282)` | 2,176 | `(2176,2062,2051)` |
| noon plus cool local `60d0ff` strength 320 | 1,600 | `(1317,1482,1600)` | 2,560 | `(2107,2371,2560)` |
| noon plus warm 80 minus neutral 40 | 1,320 | `(1320,1249,1242)` | 2,112 | `(2112,1998,1987)` |

Moon/stars rows apply when the solar total is zero.  The crescent, quarter,
gibbous, and full rows use root ages 84, 168, 252, and 336 at their transit.
Waxing and waning have the same illumination at paired ages but retain the
distinct transit/rise/set vectors above.  A below-horizon moon contributes
zero even when full; new moon contributes zero even when above the horizon.

The exact effective-profile digest vectors are:

| Profile change from root | SHA-256 |
| --- | --- |
| root | `0e2277f88f263570761db60430a0e5cbbb84c28c3fd543beec5a8bde3bdc3b08` |
| solar `1/2x` | `5d22f851bacbbcb6da58f9560a36db97ea05940416e9f4b23669bf1928632b7a` |
| solar `2x` | `b3cff5f335d829e7b729f31b39f5fce686e0748aab3cab2b879abc4d6f143669` |
| fixed summer noon/full moon | `6b5ebba1b3516c9df5395905442e202b3543a108f10a6a0aeca6af49911179cd` |
| fixed summer noon/new moon | `ae488fbaa2e0393fb925e13f56e36307f6261d34aff251c9da5b41f0b4d0d304` |
| fixed summer hour 7/new moon | `239885416be9b22d32f2b1e83a64fb39612bd11dcbbbb3fc9d7e1d9e44708045` |
| day color `ffe0c0` | `a957a66106a014e8ec9bd8a4c00e054289b7c2b0a8ca667f1c69d4b07438ef23` |
| day brightness 0 | `ab2e04a45a17742863e1a329b59109231f89559f3bb4dd084632ca2ac4885a45` |
| day brightness 128 | `938ed4d391065c87c0dbd04cfb213ddea8ebc340cfb3c15073fbc07b4ca1b8e6` |
| day brightness 512 | `0a477c1a6840e74f310502a11f3e8b64b3bd594c961ad9791e62c6f2d3ea8fe4` |
| night brightness 0 | `0032fafb6a36b1baed8687f9d0223931fd9dee2e07b81af7c34f070de7c0459e` |
| night brightness 128 | `da8fd047219d20573ac54b077b2703b858863ec712fbbc4e1b240f1b1d2c40e3` |
| night brightness 512 | `dc825693a9efde4047e999be249282deedd2af6a10327effa224d0b2118f6d99` |

The fixed full profile changes all three cycle modes to `fixed`, all three
rates to `0/1`, solar phase to 12, season phase to 3360, and lunar phase to
336.  The two fixed-new profiles use the same fixed modes/rates and season,
lunar phase 0, and the stated solar phase.  All unmentioned fields in every
other row are the root values.  The independent
day-brightness rows leave root twilight unchanged, and the independent
night-brightness rows leave summer noon unchanged; moon and stars remain
unchanged in both sets.

#### Cache lifecycle and reference hashes

The cache manager owns one classification cache and directional field per
canonical ordered vertical-stack tuple.  Every loaded map in that tuple holds
a reference; while any reference exists, the manager loads and pins every
vertical dependency.  Horizontal neighbours provide bounded scratch-halo
inputs but never join cache ownership.  When the last reference is released,
the tuple and its pins may be evicted.  Sockets own only their existing
viewer-authorized aggregate/delta caches.  No cache is persisted.  Reload
reconstructs it solely from authored maps, effective region profiles, absolute
`todtick`, and current dynamic objects.

Keys are deliberately split:

- The classification key contains ordered canonical paths/content revisions,
  dimensions/transforms/depths, structural metadata digests, and seam records.
- Each retained field-tile key contains the classification key, the ordered
  effective profile digest and three phases for every depth, and only the
  stable aperture ID/generation pairs whose clipped influence intersects that
  output tile.  There is no component-wide dynamic generation.
- The final per-socket aggregate key contains field-tile generations, the
  existing local-radiance revision, and viewer-authorization/delta state.
  Local sources never enter classification or celestial-transport keys.

All generations are unsigned 64-bit and increment after a complete mutation;
wrap is a fatal error, never an invitation to reuse a cache entry.

- Loading or linking a map synchronously requests and pins its complete
  vertical stack before publishing a field.  A transient unavailable upper
  target publishes a fully covered, zero-celestial field, records the failed
  dependency, and retries atomically when that target loads.  A malformed,
  cyclic, mismatched, or permanently missing target is a startup/content
  validation error.  A missing horizontal halo sample contributes zero
  celestial radiance and registers the same load/eviction retry; it cannot
  create false sky.  Eviction invalidates only tuples or clipped halos naming
  the dependency.  Tests unload and reload a dependency while its owner stays
  active and require fail-closed then byte-identical recovery.
- An hour, seasonal phase, lunar phase, or effective-profile change rebuilds
  only field tiles using the changed phase/profile.  It does not touch maps
  beyond discontinuous seams.
- A structural edit increments the structural revision and rebuilds the
  affected stack fields atomically.
- A door state change increments that door's stable-ID generation and
  invalidates only intersecting field tiles.  Readers see the complete old or
  complete new field, never an intermediate classification.
- Local source changes retain the existing source rebuild path.  They do not
  invalidate stable exposure or celestial transport; their revision appears
  only in the final aggregate cache key.

For reproducible lifecycle tests, hash
`atrinik-celestial-lifecycle-v1`, LF, one payload below, and a final LF as
UTF-8 SHA-256.  Payload fields, separated by `|`, are exactly: case, profile
digest, absolute `todtick`, `map@revision`, geometry, ordered stack, ordered
seams, ordered depths, structural revision, dynamic state/revision, ordered local sources,
`solar_phase,season_phase,elevation,direction,direct,diffuse`,
`age,illumination,moon_hour,elevation,direction,visible,direct`, and starlight.
`none` is literal; lists use commas and semicolons exactly as printed.

```text
star-only|0e2277f88f263570761db60430a0e5cbbb84c28c3fd543beec5a8bde3bdc3b08|0|fixture/open@1|5x5|0:fixture/open@1|none|0|1|none|none|0,0,-24576,N,0,0|0,0,0,-32768,N,0,0|2
summer-noon|ae488fbaa2e0393fb925e13f56e36307f6261d34aff251c9da5b41f0b4d0d304|0|fixture/open@1|5x5|0:fixture/open@1|none|0|1|none|none|12,3360,32768,S,960,320|0,0,12,32768,S,1,0|0
full-moon|0e2277f88f263570761db60430a0e5cbbb84c28c3fd543beec5a8bde3bdc3b08|336|fixture/open@1|5x5|0:fixture/open@1|none|0|1|none|none|0,336,-24576,N,0,0|336,65535,12,32768,S,1,20|2
full-below|6b5ebba1b3516c9df5395905442e202b3543a108f10a6a0aeca6af49911179cd|0|fixture/open@1|5x5|0:fixture/open@1|none|0|1|none|none|12,3360,32768,S,960,320|336,65535,0,-32768,N,0,0|0
door-closed|239885416be9b22d32f2b1e83a64fb39612bd11dcbbbb3fc9d7e1d9e44708045|0|fixture/door@1|7x5|0:fixture/door@1|none|0|1|door:3,2:opaque:r1|none|7,3360,8481,E,248,130|0,0,7,8481,E,1,0|0
door-open|239885416be9b22d32f2b1e83a64fb39612bd11dcbbbb3fc9d7e1d9e44708045|0|fixture/door@1|7x5|0:fixture/door@1|none|0|1|door:3,2:open:r2|none|7,3360,8481,E,248,130|0,0,7,8481,E,1,0|0
door-reclosed|239885416be9b22d32f2b1e83a64fb39612bd11dcbbbb3fc9d7e1d9e44708045|0|fixture/door@1|7x5|0:fixture/door@1|none|0|1|door:3,2:opaque:r3|none|7,3360,8481,E,248,130|0,0,7,8481,E,1,0|0
seam-depth-local|ae488fbaa2e0393fb925e13f56e36307f6261d34aff251c9da5b41f0b4d0d304|0|fixture/seam@1|5x5|0:fixture/seam@1,+1:fixture/roof@1|E:continuous:ae488fbaa2e0393fb925e13f56e36307f6261d34aff251c9da5b41f0b4d0d304,UP:continuous:ae488fbaa2e0393fb925e13f56e36307f6261d34aff251c9da5b41f0b4d0d304|0,+1|2|none|0:2,2,+80,ff6030;1:2,2,+320,60d0ff;0:2,2,-40,neutral|12,3360,32768,S,960,320|0,0,12,32768,S,1,0|0
```

Expected hashes, in the same order, are:

| Case | SHA-256 |
| --- | --- |
| star-only | `2029ae0444c29b688aa8890efb1c94e3f7664726da764b5de029fd1122013cfd` |
| summer-noon | `8a407b148df960dc3f87a9008d3efcaf9744c8fbbe17539cd2259930c433124e` |
| full-moon | `82a152015fd80fda936262b5999d57666ff7afd07d667d8cfe6af1ad7d5994a0` |
| full-below | `87e5428c68daa6e3832592edec18e19e97a64822a32b76063d8e3da71108cdcd` |
| door-closed | `5122ec1fd7bf62933acfe01e814d9142c0bdabc5950ae5db69d230d3b113a3ae` |
| door-open | `70923b27cc254a3fb2c3270bc3ea911b826f2a052c2a88ae0635e6649f3d452e` |
| door-reclosed | `a81d9bb97c70052b38d8c850b2c9fdffad379ca799f69bfb424d5b6bc75a327d` |
| seam-depth-local | `980b2e7ec76a667515e7f2520ac876d6ccc89344eace436e4e62f8cf7a992aff` |

Every payload's phase tuple is derived solely from its named profile and
absolute `todtick`; fixed profiles isolate noon/hour-7 vectors without
inventing an impossible global tuple.  These hashes deliberately cover calendar/phase inputs, direction and horizon
visibility, starlight and maximum moonlight, blocker revision, map identity,
seams, linked depths, and positive, colored, and negative local sources.  They
are input/lifecycle checksums, not viewer-visible protocol fields.

#### Synthetic structural fixtures

Implementation tests must build the following maps in memory.  Coordinates
are zero-based.  Unless stated otherwise, use a fixed summer phase, isolate the
direct-sun component at raw 960, and send it from east to west across a 7x5
map.  `front` means the east-facing exposed cell; `behind` is the immediately
west-adjacent cell.  Assertions are made before diffuse spill unless a spill
value is named.

| Fixture | Exact authored structure | Required assertion |
| --- | --- | --- |
| exterior wall | Opaque wall at `(3,1..3)` | Each front cell receives 960; direct behind is 0. |
| door closed/open | Wall above, door at `(3,2)`, wall below | Closed matches the wall without changing exposure; open transmits 960 only through that aperture and changes the two lifecycle hashes above. |
| window | `glass` at `(3,2)` | Front receives 960; behind receives exactly 720. |
| grate | `grate` at `(3,2)` | Front receives 960; behind receives exactly 840. |
| open run and reach | 40x1 open map, opaque wall at `(35,0)` | With E source, `x=39..35` is 960, `x=34..3` is 0 (steps 1..32), and `x=2..0` is reseeded 960. |
| outdoor floor run | Solid floors across depth 0, open sky, no vertical walls | Horizontal direct remains 960 across every top face; aligned depth -1 is 0 beneath each floor. |
| enclosed room | 7x7 opaque perimeter and solid roof at depth +1 over `(1..5,1..5)` | Roof faces are lit; all aligned base interior cells are covered. No exterior edge leaks around a corner. |
| courtyard | Enclosed-room roof except `(3,3)` | Only the courtyard column is stably exposed; exactly four max/spill passes soften adjacent covered cells. |
| glass roof | Enclosed-room roof with `glass` at `(3,3)` | The aligned base cell receives 720 from direct 960 without becoming sky-exposed. |
| balcony | Solid depth +1 floor at `(2..4,2)` and `grate` railing at `(2..4,1)` | Base cells below the floor are covered; objects on the balcony face are exposed; the railing transmits 840. |
| upper floor and hidden roof | Solid floor at depth +1 `(2,2)` and `LAYER_WALL sky_boundary 1` at `(3,2)` | Both top faces receive light; aligned depth 0 contents are covered; base floors under their own contents do not cover them. |
| multiple covers | Opaque faces at the same coordinate on depths +2 and +1 above depth 0 | The +2 face wins and covers both lower depths. Removing it exposes only the +1 face; unresolved +2 remains fail-closed. |
| horizontal seam | Two reciprocal 5x5 east/west maps | `continuous` with equal profile digests is byte-identical to one 10x5 sweep; `discontinuous` stops transport and evaluates the destination locally. |
| linked depth | Reciprocal aligned 5x5 up/down maps, then a missing, cyclic, and shifted variant | The valid pair follows highest-face coverage; each invalid variant reports validation failure and exposes no false sky. |
| unequal-profile depth | Reciprocal 3x3 up/down maps with a discontinuous seam; upper raw direct 960, lower raw direct 320 | The lower aligned cell is 0 under opaque, 240 under glass, 280 under grate, and 320 under open; upper-profile 960 never crosses. |

The door fixture additionally checks four-pass spill, the 32-cell direct-reach
limit, and a rapid closed/open/closed sequence.  The final closed field and
classification bytes equal the initial closed state; its lifecycle hash is the
distinct `door-reclosed` hash because the canonical generation advances to
`r3`.
All fixtures run with maps loaded in both orders and objects inserted in both
orders; aggregate radiance and classification must be identical.

For the 7x5 cardinal fixtures, serialize the pre-softening direct scalar grid
as five rows in increasing `y`, each row listing `x=0..6`.  The exact grids are:

```text
wall-or-closed=960,960,960,960,960,960,960;0,0,0,960,960,960,960;0,0,0,960,960,960,960;0,0,0,960,960,960,960;960,960,960,960,960,960,960
door-open=960,960,960,960,960,960,960;0,0,0,960,960,960,960;960,960,960,960,960,960,960;0,0,0,960,960,960,960;960,960,960,960,960,960,960
window=960,960,960,960,960,960,960;0,0,0,960,960,960,960;720,720,720,960,960,960,960;0,0,0,960,960,960,960;960,960,960,960,960,960,960
grate=960,960,960,960,960,960,960;0,0,0,960,960,960,960;840,840,840,960,960,960,960;0,0,0,960,960,960,960;960,960,960,960,960,960,960
```

A separate 11x11 covered-plane/aperture fixture seeds raw 256 at `(5,5)` and
runs the four Jacobi softening passes.  Along a cardinal ray, distances 0..5
are exactly `256,192,144,108,81,0`; along a clear diagonal they are
`256,181,128,91,64,0`.  Adding opaque touching corners makes every diagonal
value beyond the corner zero.  These arrays, the reach vector, and the
unequal-profile vector are the normative output references; input hashes alone
are not substitutes for comparing the complete generated arrays.

#### Numeric implementation budgets

Budgets are measured by the implementation issue on one pinned release runner,
with five warmups and the median of 101 builds/updates.  The representative
case is the two-map, 3,264-cell Greyton pair after it has valid migrated
structure.  The worst supported synthetic mixed component is thirteen 64x64
maps, 53,248 cells, with alternating opaque/transmissive cover, all eight
horizontal seams exercised, and the complete depth range.

| Resource | Greyton representative | Worst supported mixed component |
| --- | ---: | ---: |
| Cold build | <= 25 ms and 78,336 cell visits | <= 750 ms and 5,770,752 sampled-cell visits |
| Hour/season/profile field update | <= 12 ms and 58,752 cell visits | <= 500 ms and 4,328,064 sampled-cell visits |
| One blocker toggle | <= 2 ms and 4,096 affected cells | <= 10 ms and 16,384 affected cells |
| Retained structural/celestial memory | <= 120,832 bytes | <= 1,810,432 bytes |
| Lighting delta, one occupied sub-layer in dense 21x21 | <= 7,056 bytes at two depths | <= 45,864 bytes at thirteen depths |
| Lighting delta, all seven sub-layers in dense 21x21 | <= 49,392 bytes at two depths | <= 321,048 bytes across thirteen depth payloads |

The memory limits are exactly 32 bytes per resolved cell plus 8,192 bytes per
loaded map.  The lighting delta is at most eight bytes per serialized
sub-layer.  The representative two-depth, seven-layer lighting delta fits the
65,534-byte complete MAP2 payload before ordinary object overhead; the legal
thirteen-depth maximum necessarily uses the existing deterministic depth/tile
continuations, each independently capped at 65,534 bytes and 4,096 total
continuations.  It is an aggregate update budget, not a promise that every
depth fits in the first packet.  Ordinary object data follows those same
bounded continuation rules.
A worst-case 136x136x13 scratch halo may additionally consume at most
1,923,584 transient bytes at eight bytes per sample and is released after
atomic publication; it is not retained cache memory.

A sampled-cell visit is one read/classify/transport or relaxation evaluation
for an output or halo coordinate; repeats in different passes count again.
The worst cold limit is exactly `240448*24`, and the phase-update limit is
exactly `240448*18`, so halo construction, seam dependency reads, both direct
sweeps, and all fixed passes are inside the enforced count.  Implementations
may reuse already validated neighbour classification, but may not exclude halo
work from the counter.
Content validation rejects a door whose maximum clipped influence would exceed
the toggle limit before the map becomes runnable.  Runtime lighting never
vetoes or rolls back authoritative gameplay door state.  If a dependency
change nevertheless exceeds the estimate, commit gameplay state immediately,
publish the fail-closed zero-celestial affected tiles, and schedule one bounded
atomic structural rebuild; never perform an unbounded synchronous flood.

CI records visit counts independently of wall-clock timing and retains the raw
samples.  It also proves insertion-order independence, exact unload/reload
reconstruction, old-or-new atomic publication, bounded region/seam
invalidation, aggregate-only authorization, and byte-identical output after
returning every phase and dynamic blocker to its original value.

#### Breaking migration and editor vocabulary

Celestial-v1 is enabled only after this contract, server/schema validation,
regional profile support, content migration, and mutable-map upgrade support
land in their staged order.
Unsupported Classic/content pairings fail at startup; no legacy fallback is
allowed.

1. Structural validation and fail-closed linked loading land under #187,
   regional profiles under #189, and lunar/starlight evaluation under #191,
   while the old lighting path remains selected.
2. `content@main` migration #181 authors every `sky_above`, link boundary,
   structural exception, required archetype transmission, and complete root
   region profile.  The Greyton house receives explicit topology or reviewed
   exceptions; its legacy rectangle is not translated into structure.
   The migration retains the stable `world` region identity and explicitly
   validates the 2,136 maps that resolve to it by omitted header.
3. Before activation, the loader upgrades each recoverable swapped map under
   its map-layout lock: read its canonical source map's v1 structural metadata,
   preserve runtime objects and the current `light` value, discard legacy
   `darkness`/`outdoor`, write `celestial_schema 1` plus canonical v1 headers to
   a temporary sibling, fsync, and rename atomically.  Missing/changed source
   identity quarantines the swap for operator recovery rather than overwriting
   mutable data.  Save/swap never synthesizes structure from live objects.
4. Field implementation #188 enables celestial-v1 only after #170 consumes a
   verified content revision with the complete schema.  Partial or mixed
   migration is a hard validation error.

`MAP_FLAG_OUTDOOR`, map `outdoor`, object `FLAG_OUTDOOR`/`P_OUTDOOR`, and the
legacy per-cell XOR are removed from lighting and from validation.  On maps
formerly treated as outdoor, legacy `darkness` was ignored on ordinary cells
and is not mechanically translated.  On reviewed non-outdoor maps, a legacy
darkness anchor may become explicit neutral map `light` using the
existing exact table (`-1 -> 0`, `1 -> 20`, `2 -> 40`, `3 -> 80`, `4 -> 160`,
`5 -> 320`, `6 -> 640`, `7 -> 1280`); missing means zero.

`map_info.item_power == -2` is never mechanically converted to open sky.  It
requires structural inspection and either a valid stack or a typed exception.
Reviewed values 2, 3, and 5 may become `ambient_light_zone` strengths 40, 80,
and 320; afterwards `map_info` has no lighting role.  `map_info` `cursed` may
remain for presentation/world-maker compatibility only.  Floor
`last_sp` remains neutral local/floor radiance and never means exposure.
Legacy `world_darkness` is removed from lighting, but global `todtick` remains
the gameplay clock.

Editors and reviews use these exact terms:

| Term | Meaning |
| --- | --- |
| sky anchor | A map's required `sky_above` declaration. |
| sky-exposed | No resolved higher boundary covers the classified face. |
| covered | A higher physical or virtual boundary blocks stable sky exposure. |
| structural boundary | A floor, opaque vertical face, or explicit hidden roof recognized by the shared predicate. |
| transmission | The fixed or dynamic coefficient through one boundary; it does not alter exposure. |
| celestial environment | The effective inherited region profile and its three independent phase mappings. |

External tooling must not present an “outdoor toggle,” infer a “building
rectangle,” or call a door opening an exposure change.  It must visualize
unresolved links as validation failures, distinguish stable exposure from
current transmission, show inherited versus overridden regional fields, and
display the complete profile digest used for continuous-link validation.

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
