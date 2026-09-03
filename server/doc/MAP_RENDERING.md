# Rendered map lighting and multi-level MAP2

This design contract applies when changing classic server lighting,
`draw_client_map2()`, linked-map depth, or structural camera visibility.

## Classic production GPU renderer

The Classic production client has one mandatory GPU renderer. It creates one
SDL GPU device and uses its GPU-backed 2D renderer for the complete window,
with raw SDL_GPU passes for the ordered map albedo/owner and integer
light/tone stages. Supported production backends are Vulkan, Direct3D 12, and
Metal on hardware devices that provide RGBA8 and R32_UINT render targets plus
fragment storage buffers. There is no window-surface presentation,
CPU-completed frame, renderer selection, or software fallback.

Decoded faces, immutable effects, glyphs, region maps, minimap output, and
widget canvases become retained GPU resources. Small compatible sources and
glyphs share retained atlases; exceptional sources use standalone textures.
Widget composition targets retained GPU canvases, and the completed window is
kept in a GPU render target until presentation. An unchanged warm scene must
create, destroy, or upload no source resources. A screenshot is the only
ordinary full-frame transfer: it explicitly copies the completed GPU target
to a download transfer buffer and waits for that transfer's fence before PNG
encoding. It does not establish a retained CPU framebuffer.

The primary map keeps semantic state in sparse pointer slots and allocates a
cell only when a validated generation publishes content for that coordinate.
The GPU albedo pass preserves painter order and writes an exact integer owner
and compact-light index; the final pass consumes compact Q5.11 quad vertices
directly with the checked tone/LUT rules. It does not allocate viewport-pixel
light fields per physical depth. The production logical setting remains 17.
The 25-by-25 and 28-by-28 views are qualification-only fixtures until their
hardware, correctness, and performance release gates pass; empty state for 28
by 28 across all 13 depths remains below 64 MiB.

Resize, fullscreen, display migration, foreground resume, swapchain failure,
and submission failure all use the same complete reconstruction path. A
partial frame is never presented. Recovery gets one attempt; a second failure
shows backend/device/driver diagnostics and terminates instead of selecting a
CPU path. The frozen image, MAP, asset, and ordering fixtures remain as
immutable conformance inputs, but the CPU renderer and its executable replay
harness have been removed from every build.

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
| Encode raw radiance | After the aggregate negative clamp: zero below 1, saturate to 65535 at raw >=40959, otherwise `round_half_up(raw * 8 / 5)`. |
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
are forbidden. If an RGB vector exceeds the finite raw range, calculate each
wire word directly as
`round_half_up(channel * 65536 / max(rgb))` with a checked unsigned 64-bit
product, then clamp the peak's one-past-maximum result to `65535`.  There is no
stored or quantized intermediate gain; calling `65536/max(rgb)` a Q0.16 value
is forbidden because valid peaks 40,960..65,535 require a gain above one.  This
single rational rounding makes the fixed overflow vectors exact while
preserving chromaticity. The scalar is encoded independently.

The production client retains and interpolates compact Q5.11 map-space
vertices in the integer GPU light pass before filtering or tone mapping. It
derives one common gain/shoulder from the interpolated scalar,
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
| first RGB overflow | 40959 | `(40960,20480,1)` | 65535 | `(65535,32768,2)` |
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
Historical frame-time records remain migration evidence only. Current
qualification uses fenced production GPU frames on the documented reference
and minimum-supported hardware, with three fresh processes per matrix row and
separate CPU submission, GPU completion, and present-wait attribution. The
client owns no viewport-sized CPU lighting fields or lit-sprite compositor
cache.

- `src/server/light.c` propagates source masks as spherical 3D volumes across
  horizontal and `TILED_UP`/`TILED_DOWN` links. Opaque cells stop rays after
  receiving light on their exposed face, and a floor on the upper level blocks
  a ray crossing that vertical boundary. Search depth follows `MAP2_MAX_DEPTH`,
  the maximum depth serialized to the client.
- `glow_radius` is a bounded strength/profile selector, not a literal map-cell
  radius. Each profile defines center intensity, support radius, and falloff.
  Radial falloff preserves historical center intensity and uses integer
  fixed-point Euclidean distance with monotonic linear or squared falloff to
  zero at the support boundary. Small and medium sources gain one support cell,
  capped at the historical four-cell maximum, so smooth client interpolation
  receives enough samples for a centered pool. Positive, colored, and negative
  sources always share this geometry. Operators must remove `light_falloff`
  from custom configuration files before upgrading; it is no longer accepted.
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
  negative sources carry no authored hue and therefore subtract the same
  neutral raw amount from the scalar accumulator and from each of the three
  RGB accumulators.
  The resolver rounds once at the wire boundary and uses common-vector scaling
  on overflow. This keeps insertion order irrelevant, retains capped equal
  red/blue or red/green sources as magenta or yellow, and makes `ffffff`
  reproduce the scalar sample exactly. The v1078 transition removes the
  legacy RGB8 projection instead of retaining a parallel transfer path.
  Ambient, floors, world light, special vision, and `tli` stay neutral;
  negative sources affect both the scalar and RGB aggregate, but only through
  that neutral subtraction; an authored color on a negative object is ignored.
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
`a9f02373382deea5dc8edcd4017e880a34a43270` and `content@main` revision
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
| Authored exits targeting `/random/` | 2 | Keep the feature; generated maps receive explicit sealed v1 metadata before insertion. |
| Live plugin `CreateMap()` scripts | 1 | Migrate the factory API so every `/python-maps/` result is explicit before it becomes runnable. |
| Exact-type `GATE` map instances | 196 in 39 maps | Explicitly migrate 15 `gate_open`, 5 `gate_closed`, 115 `grate_open`, 41 `grate_closed`, 12 `portcullis_closed`, 4 `piston_down`, and 4 `piston_up` instances; never infer them from `FLAG_BLOCKSVIEW`. |
| Exact-type `DOOR` map instances | 1,115 in 309 maps | Explicitly migrate 126 `door_wood1`, 121 `door_wood2`, 66 `curtain1`, 706 `door1_locked`, 77 `gate1_locked`, and 19 `door_bar1` instances. |
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
`TILED_UP`/`TILED_DOWN` pair whose target is loaded and resolved. Either member
may be supplied by an existing filename-derived coordinate link; Classic derives
that runtime link before validating the sky anchor, while explicit paths remain
authoritative.  Celestial-v1
defines no immutable-summary encoding; adding one requires a new schema
version and exact bitmap, transform, onward-link, revision, and digest vectors.
`sealed` says that unmodelled solid cover exists above the whole map and
forbids `TILED_UP`; typed `open` exceptions may cut apertures in that virtual
cover.  A missing record is an error.  An unloaded or missing target, bad
reciprocal link, alignment/dimension mismatch, cycle, or vertical component
larger than `MAP2_LEVELS == 2 * MAP2_MAX_DEPTH + 1` (thirteen maps at the
current constants) is an unresolved stack and fails closed as covered.

All runtime-created playable maps obey the same schema before their first
object is inserted.  The random-map generator validates dimensions in
`1..64`, writes
`celestial_schema 1`, `sky_above sealed`, and
`celestial_generated_origin ORIGIN_PATH`, and persists an explicit `region`
equal to the effective region of the origin map (`world` when its header was
omitted).  It creates no structural exceptions, upper map, or horizontal tile
links.  If a future generator creates any such link, it must emit the exact
reciprocal `tile_path_N` and `celestial_boundary_N` records specified below;
filenames never synthesize a boundary policy.  The random-map parameter
`darkness` is converted once to the neutral `light` header by the exact legacy
table in the migration section and never enters celestial structure.

Generated-map save and swap-save re-emit those headers canonically, including
origin and resolved region identity, so reload has no dependency on the
origin still being resident.  A pre-v1 `/random/` swap has no canonical source
from which its cover and region can be recovered: loading quarantines it for
operator recovery and follows the existing player savebed fallback rather
than guessing or overwriting it.  The generator fixture uses this complete
input: path `/random/0`, origin `/fixture/origin` at `(1,1)` with omitted region
header, `Xsize=40`, `Ysize=40`, `expand2x=0`, `layoutstyle=rogue`, all three
layout options zero, `symmetry=0`, `floorstyle=ice`, `wallstyle=ice`,
`doorstyle=none`, `exitstyle=stairs_stone`, `monsterstyle=none`,
`num_monsters=0`, `decorstyle=none`, `decorchance=0`, `orientation=1`,
`difficulty=48`, `dungeon_level=48`, `dungeon_depth=114`,
`level_increment=0`, empty final-map/music/name fields, `random_seed=186`, and
`darkness=3`.  No unset/default parameter may consume random state.  It
requires `world`, `sealed`, and neutral `light 80`; every cell has zero celestial and
raw aggregate 80, and canonical metadata and complete aggregate arrays are
byte-identical after save/reload.  Hash the following ASCII name/value payload
with real LF separators and a final LF:

```text
atrinik-celestial-generated-v1
path=/random/0
origin=/fixture/origin
size=40x40
region=world
sky_above=sealed
light=80
```

Its SHA-256 is
`1b3e03b8a22d44821885bc376a0ca976ff230b441588049798d1933d49b728c8`.
A second oracle hashes the canonical saved object stream: the bytes begin with
the first `arch` after the map-header `end` and continue through the final
object `end` and its LF, excluding the header whose v1 fields are new.  At the
Classic base `e74fd3bd616046c61c27412235e4c5e4ea625483` and content revision
`65a88167d3a2bedcc2dc21508d94d7ca009a76d2`, that stream is 89,370 bytes and
has SHA-256
`b12bcdccc0bc8503a05017a6910344c76428600dbb6e3f74fd305594f8bd3d43`;
the terminal `RMParms.rng.state` is the unsigned decimal
`17031909990014969516`.  Generation, canonical save, and save/reload must
reproduce both values exactly.  Thus a new default, a changed random draw, or
different layout/object placement cannot pass merely because sealed celestial
output is constant.
A second origin in a child region must persist that child name, and dimensions
65x40 must fail before map insertion.

The Python `CreateMap()` API is also a validated v1 factory.  Its signature
becomes `CreateMap(width, height, path, origin_map, sky_above, light)`, where
`origin_map` must be a resident validated map and `sky_above` is exactly
`open` or `sealed`; `linked` is invalid because this factory accepts no upper
link.  It validates `1..64`, writes schema 1, persists the origin path and the
origin's effective region exactly as the random generator does, and validates
neutral raw `light` in `0..40959`.  This value is immutable after creation and
is persisted canonically across save/swap/reload.  It returns no map object
until all metadata is valid.
`path` is a case-sensitive relative identifier of 1..240 ASCII bytes.  It is
one or more slash-separated segments; every segment is 1..64 bytes, starts
with `[A-Za-z0-9]`, and then contains only `[A-Za-z0-9_.-]`.  Empty segments,
`.` or `..`, a leading/trailing slash, backslash, non-ASCII input, and any
string changed by canonicalization are invalid.  The identity is exactly
`/python-maps/` followed by those unchanged bytes.  Before allocation, under
the map-layout lock, reserve that identity and reject a collision with an
authored path, loaded or swapped map, prior reservation, or persisted plugin
map.  `food_haven` and `tests/arena-1` are valid vectors; empty, `/x`, `x/`,
`x//y`, `x/../y`, `x\\y`, a 65-byte segment, a 241-byte identifier, and a
second concurrent `food_haven` all fail before allocation.
The one pinned live `food_haven.py` caller migrates explicitly to `sealed`,
uses its activating map as the origin, passes raw `light 1280`, and removes its
later `m.darkness = 7`; its generated map must remain raw 1280 before and after
save/reload.  Test-only callers pass an explicit intended value, normally zero,
and the legacy darkness-property test becomes an immutable-light/factory test.
A plugin cannot mutate schema, anchor, region, generated origin, exceptions,
links, boundary declarations, or neutral light after creation.  The old
three-argument API and the map `.darkness` setter
are removed at activation and raise before mutation/allocation, rather than
creating a mixed-schema map.  Any future engine/plugin map factory must call the same
validated initializer; bare `get_empty_map()` remains test/internal allocation
and cannot be published, inserted into `first_map`, entered, saved, or swapped
until initialized.

Filename-coordinate link synthesis is not gated by the celestial schema.  When
a map has a signed coordinate suffix, the loader applies the legacy ten-slot
coordinate lookup to every missing `tile_path_N`; explicit paths remain
authoritative.  With `MAP_NO_DYNAMIC`, a derived slot is populated only when
the candidate map exists, so an omitted direction remains a valid terminal
edge and creates no path, retry, or invisible map.  Without that flag, the
legacy dynamic-map rules are preserved as well.  Derived links carry no
authored boundary policy, while explicit celestial links continue to require
their reciprocal boundary declarations and vertical-stack validation.  The
static authored-exit validator mirrors the existing-map coordinate lookup.
Fixtures cover explicit and derived links, an omitted terminal edge, vertical
coordinates, an authored missing target, asymmetry, different profiles, and
reload.

The shared structural classifier returns a five-bit oriented face mask
`DOWN,N,E,S,W` for each cell, never a cell-level boundary boolean.  It sets
faces for a cell containing any of:

- a solid floor (`FLAG_IS_FLOOR`) above the contents being classified;
- a static gameplay-opaque vertical surface (`FLAG_BLOCKSVIEW`), a `DOOR` or
  `GATE` aperture regardless of its current state, or an explicit
  `glass`/`grate` transmissive surface; or
- an explicit hidden `LAYER_WALL` roof/camera surface with `sky_boundary 1`.

`sky_boundary` is valid only on a hidden `LAYER_WALL` archetype.  It is a
validation error on another layer or on an object whose lifecycle is dynamic.
Ordinary floors, opaque walls, and roofs have implicit opaque transmission;
authors do not need to duplicate `sky_boundary` on those already classified
by the shared classifier.  Sprite pixels, object names, paths, map naming,
`map_info`, and cutaway/presentation rectangles never enter the classifier.

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
The vertical coverage scan tests only `DOWN`; `N,E,S,W` can never cover the
aligned cell on a lower map.  Directional horizontal sweeps test only the
cardinal exit face(s) crossed by that step.  An upper-level wall with no
floor/roof therefore leaves the aligned lower cell sky-exposed while still
blocking the relevant horizontal ray.  This exact upper-wall/no-floor case and
the same cell with a floor are required regression fixtures.

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
reload must reproduce the same canonical serialized rectangle bytes.  This
preserves editor
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
| Closed/open solid door | unchanged door aperture | `opaque` / `open` | 0 / 256 |
| Closed/open curtain | unchanged door aperture | `glass` / `open` | 192 / 256 |
| Closed/open bar or door-gate | unchanged door aperture | `grate` / `open` | 224 / 256 |
| Closed/open solid gate | unchanged gate aperture | `opaque` / `open` | 0 / 256 |
| Closed/open grate gate | unchanged gate aperture | `grate` / `open` | 224 / 256 |
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

Only objects of exact type `DOOR` or `GATE` are dynamic apertures in v1.  Both
must author `celestial_transmission_closed` and
`celestial_transmission_open`, each using the four transmission names above.
Each placed dynamic-aperture object must also author
`celestial_aperture_id` as exactly sixteen lowercase hexadecimal digits other
than `0000000000000000`.  The identity is the pair of canonical map path and
that ID; IDs are unique within a map file.  The loader rejects a missing,
malformed, or duplicate ID, and rejects the field on every other object type.
It does not use process-local `object.count`, insertion order, coordinates,
archetype name, animation frame, or object-save order as identity.  Canonical
object save persists the ID unchanged.  A `DOOR` or `GATE` may not move to a
different map or coordinate under v1; a future movable aperture requires a new
schema.  Content migration assigns the IDs once, and a runtime factory that
places an aperture must reserve a previously unused ID before insertion.

An aperture generation is cache-only unsigned 64-bit state.  It initializes
to 1 after a load into an empty cache, advances after each authoritative state
transition, is never serialized, and may restart at 1 after a complete cache
eviction/reload because no cache survives that event.  Multiple apertures on
one edge remain distinct and their sorted `(map path, aperture ID,
generation)` tuples all enter the affected tile key before their coefficients
are combined by minimum.  Fixtures use IDs `00000000000000ba` and
`00000000000000bb`, reverse object insertion/save order, exercise a same-edge
pair, reject a duplicate, and require identical output and key tuples after
reload.
Validation permits at most four dynamic apertures contributing to one
oriented edge, four in one cell, 256 in one 64x64 map, and 256 total across any
one owner's complete 136x136x13 clipped halo envelope.  A factory reserves
against all four limits before insertion; authored content or a seam topology
exceeding one is invalid.  Each map retains one fixed 24-byte record per aperture (ID,
generation, packed influence bounds/state), at most 6,144 bytes; the remaining
2,048 bytes of the per-map 8,192-byte allowance cover owner references and
indexes.  A field-tile key retains one 32-byte SHA-256 of the canonical sorted
intersecting tuple stream rather than duplicating the tuples.  Cold/update
preflight inspects at most 256 aperture records for an owner, and at most four
while performing any one counted cell classification; it remains inside the
tabulated wall-clock and sampled-cell limits.  One toggle affects at most
4,096 unique output cells in the representative fixture and 16,384 in the
worst supported component, exactly matching the budget table below.  These caps make
same-edge composition, key construction, and retained memory bounded.
For `DOOR`, `FLAG_DOOR_CLOSED` selects the closed class and its absence selects
the open class.  For `GATE`, the authoritative state is closed exactly while
`FLAG_NO_PASS` is set and open otherwise; the coefficient changes at the same
object update that changes that flag, including midway through an animation.
`FLAG_BLOCKSVIEW`, sprite, animation frame, `stats.wc`, and object name never
select the coefficient.

The reviewed migration assigns `opaque`/`open` to solid door, gate, and piston
families; `glass`/`open` to curtains; and `grate`/`open` to bars, door-gates,
grates, and portcullis families.  A closed class of `open` is invalid, and the
open class must be `open` in v1.  These values are
then explicit archetype data, not runtime name heuristics.  No other object
type or script may mutate transmission.  Reload derives the current
coefficient from the persisted authoritative gameplay flag and the two
immutable authored classes.  Every aperture-state transition advances its
stable-ID generation and uses the same bounded invalidation.  Its stable
boundary/exposure classification never changes.  Thus opening one door or
gate admits a bounded beam and spill; it cannot reclassify the enclosed room
or building as outdoors.  A balcony's floor covers aligned contents below
while its railing may use `grate`; the balcony's own contents are not covered
merely because their current-level base floor exists.

#### Stack resolution and exposed faces

Celestial structural resolution and viewer-relative MAP2 depth are distinct.
Resolve the complete reciprocal UP/DOWN component first, with at most
`MAP2_LEVELS == 2 * MAP2_MAX_DEPTH + 1` members (thirteen maps and twelve
vertical crossings at the current constants).  Assign canonical stack indexes
`0..member_count-1` from the unique bottom member to the unique top member.
That ordered component is the one shared cache tuple regardless of which
member is currently classified.  A component with fourteen members, a branch,
a cycle, or no unique terminal at either end is invalid before publication.

MAP2 packet depth remains relative to the viewer's current member: index
difference zero is that member, positive is above, negative is below, and only
`-MAP2_MAX_DEPTH..+MAP2_MAX_DEPTH` is serialized.  A valid member outside that
display window is not serialized for the current view but remains resolved and
pinned and still contributes structural cover and transport.  Moving the
viewer changes the display slice, never the canonical component or field key.
A thirteen-member fixture viewed at its bottom, middle, and top must resolve
one tuple and identical aligned structural results; the three MAP2 depth sets
are respectively `0..6`, `-6..6`, and `-6..0`.  A fourteen-member fixture
fails closed before any member becomes runnable.

Every vertical pair has an identity cell transform: it requires equal map
width and height, and maps lower `(x,y)` to upper `(x,y)` for every coordinate.
No vertical offset, crop, wrap, rotation, or filename/`coords[]` origin is
recognized in v1.  Both reciprocal records and policies must agree with that
identity relation.  Unequal dimensions, an offset declaration, or a pair for
which either `(0,0)` or `(width-1,height-1)` does not map identically is invalid;
fixtures cover 64x64/64x64 success, 64x64/63x64 failure, and attempted `(1,0)`
offset failure.  Adding a vertical transform requires a new schema version.

Every referenced map must be resident for construction; v1 treats an unloaded
upper target as unresolved rather than guessing from its path or last cache.

For each aligned column, scan from the highest resolved depth downward.  The
first set `DOWN` face is the highest relevant horizontal boundary.  Its exposed face
may receive celestial light.  Objects strictly below that face are covered,
and the coverage continues through every lower linked level until a crossed
boundary or aperture transmits light.  A lower boundary cannot punch a hole in
an upper roof.  An ordinary floor on the current base level does not cover the
contents resting on that same face.  These are structural rules and are
independent of camera cutaway and gameplay LOS.

Horizontal `tile_path_1..tile_path_8` seams use the existing coordinate
transform.  The serialized suffix is one-based and maps to the existing
zero-based constants exactly as follows:

| Suffix | Direction | Reciprocal suffix |
| ---: | --- | ---: |
| 1 | `TILED_NORTH` | 3 |
| 2 | `TILED_EAST` | 4 |
| 3 | `TILED_SOUTH` | 1 |
| 4 | `TILED_WEST` | 2 |
| 5 | `TILED_NORTHEAST` | 7 |
| 6 | `TILED_SOUTHEAST` | 8 |
| 7 | `TILED_SOUTHWEST` | 5 |
| 8 | `TILED_NORTHWEST` | 6 |
| 9 | `TILED_UP` | 10 |
| 10 | `TILED_DOWN` | 9 |

Every explicitly authored non-empty `tile_path_N` requires exactly one
same-suffix declaration, and a declaration without that path is invalid:

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

Canonical save orders each authored `tile_path_N` immediately before its
`celestial_boundary_N`, with suffixes increasing from 1 through 10.  A
runtime-derived filename link has no boundary policy and, on celestial maps,
is not serialized; it exists only for coordinate traversal and must not be
used to infer `continuous`.  A derived vertical link is recomputed from its
coordinate-suffixed filename on save/reload, so it does not require a serialized
`tile_path_9`/`tile_path_10` record.  The fixture abbreviations `N`, `E`,
`S`, `W`, `NE`, `SE`, `SW`, `NW`, `UP`, and `DOWN` mean precisely the suffixes
in this table and are not additional serialized spellings.  Parser/save
vectors cover reciprocal pairs `2/4`, `5/7`, and `9/10`, both policies, a
terminal edge, and signed coordinate-derived links; the explicit records
round-trip byte-identically while a missing candidate creates no link.

Every pre-v1 mutable map without an indexed canonical authored source is
unprovable, regardless of its path or old flags.  This includes `/random/N`,
`/python-maps/*`, and a missing coordinate neighbour previously materialized
through `ready_map_name(path, originator, ...)`, including one marked
`MAP_NOSAVE` that nevertheless acquired an ordinary temporary swap.  Activation
never derives its region, sky anchor, ambient, links, or structure from the
path, originator, resident objects, or current map list.  It quarantines the
swap and its `temp.maps` entry without overwrite; player entry follows the
existing savebed fallback and non-player state remains unavailable.  Exact
fixtures cover all three categories, including the live legacy Food Haven path
and a synthesized neighbour, restart, diagnostics, and operator recovery.

Per-player unique maps are a fourth, source-backed mutable category and are
never omitted merely because they bypass `temp.maps`.  Discovery begins from
the canonical account files, not a blind recursive file scan.  Activation
requires the configured account-name policy to be exactly the pinned default:
`4..16` ASCII `[A-Za-z0-9]`.  Storage identities are lowercase `[a-z0-9]`;
login's accepted uppercase spelling is canonicalized before lookup and is
never a second on-disk identity.  A
different configured length or allowed-character policy is an unsupported
cohort.  Open
`DATAPATH/accounts` by descriptor without following links.  At descriptor
depths 1 through 4 accept only directories whose name is the first 1, 2, 3, or
4 bytes of a prospective lowercase account name.  Within that depth-4
directory accept only a regular `ACCOUNT.dat` leaf where `ACCOUNT` satisfies
that policy,
`string_tolower(ACCOUNT)` is byte-identical to `ACCOUNT`, and
`account_make_path(ACCOUNT)` reproduces the descriptor-relative path byte for
byte.  No other regular file, extra directory depth, symlink, hard-link alias,
device/socket/FIFO, duplicate inode, path/case alias, or unowned entry is
allowed.  The validated lowercase `ACCOUNT` leaf name is the account identity
because the legacy file contains no separate identity field.  Parse every
account roster and require the configured character-name policy to be exactly
the pinned default: `4..20` ASCII letters, digits, spaces, apostrophes, or
hyphens.  Every roster name must itself have 4..20 bytes, contain only that
exact set, be byte-identical to `string_title(name)`, and have a case-folded
form that occurs in exactly one roster entry across the complete account tree.
A different configured length or
allowed-character policy is an unsupported cohort.  Derive its directory only
through the existing `player_make_path(character, "player.dat")` rule: ASCII
lowercase the canonical roster spelling, use its first one-, two-, and
three-byte prefixes, then the complete lowercase name directory beneath
`DATAPATH/players/`.  The derived descriptor-relative path must round-trip
byte-identically.  An orphan character directory, duplicate roster owner,
roster/path case mismatch, normalization alias, or unexpected directory entry
fails preflight.
Open the directory by descriptor without following links and accept exactly:

- required regular `player.dat`;
- optional regular `metrics.dat`; and
- zero or more regular unique-map leaves matching
  `\$SEGMENT(?:\$SEGMENT)*`, where `SEGMENT` is 1..64 ASCII bytes, begins with
  `[A-Za-z0-9]`, and continues with `[A-Za-z0-9_.-]`.

The roster archetype token must resolve uniquely to an archetype whose clone is
exact type `PLAYER`.  A zero-length reserved `player.dat` is a pending first
login and may own no unique-map leaves.  For every non-empty `player.dat`, the
strict predecessor loader must consume exactly one root player object; its
saved object name must be byte-identical to the canonical roster name, its
type must be `PLAYER`, and its archetype must be the exact roster archetype.
No case-folded name match, compatible type, or later path derivation repairs a
mismatch.  Any violation is a character-group failure before a destination
path or journal sibling is created.

`player.dat.tmp`, unknown regular files, subdirectories, device/socket/FIFO
entries, hard-link aliases, symlinks, path escape, and duplicate physical
files fail preflight rather than being guessed or silently skipped.  Account
files and `metrics.dat` are cohort state but never map candidates.  Canonical
predecessor logical map IDs use `/SEGMENT(?:/SEGMENT)*` under the same segment
grammar, explicitly forbid `$`, and contain at most 245 total bytes including
the leading `/`.  Demangling replaces the leading and every
subsequent `$` separator with `/`; encoding performs the inverse.  The two are
therefore bijective, and decode-then-encode must reproduce the leaf byte for
byte before an exact predecessor index row can authorize migration.  Since
`unique-v1:` is ten bytes, the resulting player-field token is at most 255
bytes and fits `MAX_BUF == 256` including NUL.  A 245-byte logical ID is the
positive boundary; 246 bytes fails before allocation, save, or migration and
is never truncated.

The provenance ledger row additionally binds stable account and character
identities, the canonical character-relative unique leaf, its exact file
SHA-256, the demangled source path/revision/SHA-256, and the activation
generation.  V1 player records never persist a physical state-root pathname.
Their `map` and `bed_map` fields use `unique-v1:LOGICAL_ID` for a private map,
where `LOGICAL_ID` is the canonical decoded absolute source ID and the
character identity is implicit in the owning `player.dat`; ordinary authored
map IDs retain their existing spelling.  On load, the validated v1 marker and
roster resolve that token to the selected versioned root, character directory,
and bijectively encoded leaf.  A token for a missing/quarantined file is
invalid and cannot be passed to ordinary `ready_map_name()` path handling.
The loader rejects any unterminated or overlength `map`/`bed_map` field rather
than applying the legacy truncation behavior.

Upgrade one character as a journaled group: all eligible private maps, that
character's provenance-ledger rows, and `player.dat` are the live records in
one digest-addressed transaction.  Preserve mutable objects only after the
ambient and unique aperture-locator rules succeed, and rewrite every exact
old physical `map`/`bed_map` reference to its `unique-v1:` token.  Resolve and
validate the current `map,x,y` and savebed `bed_map,bed_x,bed_y` destinations
independently after map-local quarantine.  When both remain valid, preserve
their respective coordinates and rewrite only the physical identities.

If the current destination is invalid but the savebed remains a validated,
non-quarantined ordinary or unique map whose coordinates are in bounds and
legal for login placement, copy that savebed identity into both `map` and
`bed_map` and copy `bed_x,bed_y` into the player object's saved `x,y`; no
coordinate from the invalid current map survives.  If the current destination
remains valid but the savebed does not, preserve `map,x,y` and write
`EMERGENCY_MAPPATH,EMERGENCY_X,EMERGENCY_Y` only to the savebed fields.  If
neither destination remains valid, write those emergency values into both
identity/coordinate triples.  Every emergency rewrite first validates that
exact emergency placement.  A physical reference that does not exactly match
this character's inventory, a duplicate `map`/`bed_map` field, an overlength
field, or a unique token in legacy input quarantines the complete character
group.

Failures have two exact scopes.  A private-map-local failure—missing/changed
authored source, aperture locator mismatch, invalid celestial header/object
data, or map-file digest mismatch already present in the immutable cohort
snapshot taken at activation-lease acquisition—quarantines only that map as
evidence; the character transaction may publish a rewritten
`player.dat` using the fallback rule above, and excludes that leaf from the v1
inventory.  A character-group failure—invalid roster/ownership/directory,
malformed or duplicate player reference, physical reference outside the owned
inventory, no validated emergency target, journal digest/state corruption, or
an I/O durability failure—publishes none of the
character's v1 maps, player record, or ledger and quarantines the complete old
group.  Global account-tree ambiguity or an unrecognized entry aborts the
entire activation before any marker.  Fixtures separately cover a failed
current private map with valid private savebed, a valid current map with failed
private savebed, both private references failed with emergency fallback, exact
`map`/`bed_map` and both coordinate-pair rewrites, out-of-bounds current and
savebed coordinates, an unrelated failed private map, and a complete-group
quarantine.

The group journal names old/new SHA-256 values for every map, ledger fragment,
and `player.dat`.  After `PREPARED`, publish and directory-fsync all map
siblings, advance `MAPS_COMMITTED`, publish/fsync the ledger, advance
`LEDGER_COMMITTED`, and publish/fsync `player.dat` last.  That final rename is
the logical group commit; advance `DONE` and clean up afterward.  Earlier
states are durable and recoverable but remain offline/non-servable under the
exclusive cohort lease.  Thus a rewritten player reference is never the live
record until all referenced maps and ledger rows are live.

Restart recovery verifies the complete tuple, never the state word alone:
all-old discards only named siblings and retries; a prefix of new maps with old
ledger/player completes the remaining verified map renames; all-new maps plus
old ledger/player publishes the verified ledger; all-new maps/ledger plus old
player publishes the verified player; and all-new records is committed cleanup.
Any new player with an old/unknown map or ledger, a non-prefix map mixture,
unknown digest, missing required sibling, or state/digest disagreement
quarantines the complete group without further rename.  Replaying any valid
state any number of times reaches the same committed tuple.  Missing/changed
source, aperture mismatch, or a map digest defect already captured by the
lease-acquisition snapshot uses map-local quarantine.  Any map, player, roster,
ledger, index, or content byte mutation after that snapshot which was not made
by the lease holder is a global activation abort, even when detected before
`PREPARED`; after `PREPARED`, a journal-member digest disagreement additionally
quarantines the character group as recovery evidence.
Snapshot, rollback, complete-inventory generation checks, and the activation
marker cover accounts, player records, and unique files as well as ordinary
swaps.  Fixtures cover successful apartment/reference upgrade and login,
current and bed-map references, state-only aperture change, non-map regular
files, uppercase account storage and account-tree aliases, unsupported
account/character policy, short/overlong/invalid/non-title roster names,
malformed fan-out, orphan trees, duplicate same-account or cross-account roster
ownership, cross-account root-name mismatch, wrong root type/archetype,
`$` source/collision rejection,
255/256-byte token boundaries, missing/changed source, a
post-snapshot/pre-`PREPARED` mutation, both failure
scopes and coordinate-safe fallbacks, and crashes before
and after every map/ledger/player write, fsync, rename, and commit state.

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
value.  Directional sun and moon use scan lines in source-to-destination order
with an exact state `(direct, aperture_direct, cooldown)`.  Initialize all
three to zero on halo entry.  At each cell, in this order:

1. If `cooldown == 0` and the receiving face is sky-exposed, replace `direct`
   with the unattenuated local-profile direct value.  This is the only sky
   seed; a covered face never seeds.  It lets a ray that entered under cover
   acquire direct light when it first reaches exposed terrain.
2. Record `direct` as the cell's incident directional value and record
   `aperture_direct` in a separate spill-eligible seed buffer.
3. Multiply both transport values by the crossed face coefficient with the
   ordinary rounding rule.  If the crossed face is an authored transmissive
   surface or exact `DOOR`/`GATE` aperture, set outgoing `aperture_direct` to
   the resulting outgoing `direct`; this includes an open dynamic aperture
   whose coefficient is 256.  Crossing an empty edge never qualifies it.
4. If the coefficient is below 256, set outgoing `cooldown = 32`; otherwise,
   if incoming `cooldown > 0`, subtract one, and leave zero at zero.  A later
   sky seed replaces `direct` but does not manufacture
   `aperture_direct`; that buffer changes only by transport or a real aperture
   crossing.

The boundary cell is step zero.  Its next 32 cells observe cooldown values
32 down to 1 before decrement and cannot sky-seed; cell 33 observes zero and
may return to full direct light.  An opaque boundary receives incident light
but emits zero, glass/grate emits the attenuated value, and an open door emits
the full value while marking the separate aperture buffer.  For diagonal rays,
one supercover move is one ray step and crosses both cardinal edges.  This is
the exact 32-cell reach; there is no Euclidean-distance alternative.

In an aligned vertical column, only the highest relevant boundary face seeds.
Open cells above a lower surface are transport nodes.  The value crossing each
lower face follows the same coefficient and rounding rule.

Diagonal sweeps use a supercover step.  A diagonal crosses both adjacent
cardinal edges; its coefficient is the minimum of those edge coefficients,
so two touching opaque corners never leak.  Ties use north before south and
west before east solely to make traversal reproducible; max-composition makes
the final field independent of that tie order.

Diffuse sky, starlight, and the separate `aperture_direct` seed buffer receive
exactly four relaxation passes, never an unbounded flood fill.  Each cell
takes the maximum of its prior value and transmitted neighbours, not their
sum.  Cardinal spill uses `192/256`; diagonal spill uses `181/256` after the
same corner minimum.  The fixed pass count bounds spill to four cells and
prevents area amplification.  Direct sun and moon are directional; diffuse
sky and starlight are not.  Insertion order, object order, load order, and
thread scheduling cannot change the result.  Required whole-array fixtures
include a 40-cell ray covered at cells 0..4 then exposed with no boundary
(`direct` is 0 for 0..4 and full for 5..39; `aperture_direct` is all zero), and
otherwise identical exposed rays with an empty exit versus an open door exit
after cell 4 (both direct arrays stay full, while only the door case has full
`aperture_direct` from cell 5 onward and therefore spills laterally).

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
A halo coordinate must resolve to exactly one `(canonical map path, x, y,
canonical stack index)` independent of traversal path.  Validation composes
the existing integer seam transforms for every continuous path reachable
within 36 cells and rejects either two different targets for one logical
coordinate or one target assigned to two logical coordinates.  Every
commuting square must agree: E-then-N, N-then-E, and a direct NE edge resolve
the same map/cell when present, and crossing a horizontal seam then UP equals
UP then that depth's corresponding horizontal seam.  The check is bounded by
the same halo envelope and runs before publication.  Fixtures include a valid
three-route NE diamond, a conflicting offset/target diamond, a valid
horizontal/vertical square, and a conflicting depth square; no traversal-order
tie-break is permitted.
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
| quarter moon plus stars at root hour 20 | 11 | `(5,7,11)` | 18 | `(8,11,18)` |
| crescent moon plus stars at root hour 20 | 3 | `(1,2,3)` | 5 | `(2,3,5)` |
| gibbous moon plus stars | 17 | `(8,10,17)` | 27 | `(13,16,27)` |
| noon plus warm local `ff6030` strength 80 | 1,360 | `(1360,1289,1282)` | 2,176 | `(2176,2062,2051)` |
| noon plus cool local `60d0ff` strength 320 | 1,600 | `(1317,1482,1600)` | 2,560 | `(2107,2371,2560)` |
| noon plus warm 80 minus neutral 40 | 1,320 | `(1320,1249,1242)` | 2,112 | `(2112,1998,1987)` |

Moon/stars rows apply when the solar total is zero.  The crescent and quarter
rows use root ages 84 and 168 at solar hour 20, producing moon hours 17 and 14,
elevations 8,481 and 28,378, and direct strengths 1 and 9 before adding stars.
The gibbous and full rows use root ages 252 and 336 at their transits, solar
hours 21 and 0.  Waxing and waning have the same illumination at paired ages
but retain the distinct transit/rise/set vectors above.  A below-horizon moon
contributes zero even when full; new moon contributes zero even when above the
horizon.

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
  dimensions/identity transforms/canonical stack indexes, seam records, and
  each member's unsigned 64-bit structural revision and resulting complete
  classification digest.  A clipped horizontal-halo classification key also
  contains the same path/content/structural-revision/digest tuple for every
  sampled neighbour.
- Each retained field-tile key contains those exact classification and clipped
  halo keys, the ordered
  effective profile digest and three phases for every depth, and only the
  SHA-256 of the canonical sorted `(canonical map path, stable aperture ID,
  generation)` tuple stream whose clipped influence intersects that output
  tile.  Hashing consumes every tuple but the retained key is fixed-size.
  There is no component-wide dynamic generation.
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
- A structural edit increments the affected map's structural revision and
  rebuilds classification plus affected stack/halo fields atomically.  The
  revision remains part of every dependent key even when a later edit restores
  byte-identical geometry.  The remove/re-add fixture has revisions 1, 2, and
  3: revision-3 classification/output bytes equal revision 1, but its key must
  differ and no revision-1 entry may be reused under the revision-3 identity.
- A `DOOR` or `GATE` state change increments that aperture's stable-ID
  generation and
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
| transmissive doors | Same aperture using a curtain, bar, and door-gate | Closed curtain exactly matches the 720 window array; closed bar and door-gate match the 840 grate array; all three open states match `door-open`, and reload preserves the authoritative flag and exact array. |
| gate closed/open/reload | Same aperture using `GATE` | Solid `opaque`/`open` state produces the exact closed-wall/open-door arrays; grate `grate`/`open` state produces the grate/open-door arrays. `FLAG_NO_PASS` survives save/reload and reproduces the matching array byte-for-byte. |
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
runs the four Jacobi softening passes.  The `corner` case makes only the two
undirected edges `(5,5)-(5,4)` and `(5,5)-(4,5)` opaque; diagonal candidates
crossing either edge therefore use coefficient zero.  Rows are increasing
`y`, columns are increasing `x`, and the complete normative scalar arrays are:

```text
clear
0,0,0,0,0,0,0,0,0,0,0
0,64,68,72,77,81,77,72,68,64,0
0,68,91,96,102,108,102,96,91,68,0
0,72,96,128,136,144,136,128,96,72,0
0,77,102,136,181,192,181,136,102,77,0
0,81,108,144,192,256,192,144,108,81,0
0,77,102,136,181,192,181,136,102,77,0
0,72,96,128,136,144,136,128,96,72,0
0,68,91,96,102,108,102,96,91,68,0
0,64,68,72,77,81,77,72,68,64,0
0,0,0,0,0,0,0,0,0,0,0

corner
0,0,0,0,0,0,0,0,0,0,0
0,0,0,0,0,0,0,0,0,0,0
0,0,0,68,72,77,81,77,72,68,0
0,0,68,72,96,102,108,102,96,72,0
0,0,72,96,102,136,144,136,102,77,0
0,0,77,102,136,256,192,144,108,81,0
0,0,81,108,144,192,181,136,102,77,0
0,0,77,102,136,144,136,128,96,72,0
0,0,72,96,102,108,102,96,91,68,0
0,0,68,72,77,81,77,72,68,64,0
0,0,0,0,0,0,0,0,0,0,0
```

Canonical digest input is the ASCII prefix
`atrinik-celestial-spill-v1` plus LF, `case=clear` or `case=corner` plus LF,
`size=11x11` plus LF, and exactly the eleven comma-separated rows plus LF.
The SHA-256 digests are
`12c4e1b66bb212d046319e309d1255d57627a74b4a4c8ee79cc4faa47b971275`
for `clear` and
`c7282150e81a78050d305772f4649ebc12fa7a6f1f42b9ea972b97934df8510a`
for `corner`.  Whole-array equality and both digests are required; cardinal,
diagonal, reach, and unequal-profile spot checks do not substitute for them.

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
| Complete colored MAP2 delta, sub-layer 0 only, dense 21x21 | <= 11,486 raw wire bytes (11,483 command data) | <= 74,625 raw wire bytes (74,618 command data), one continuation |
| Complete colored MAP2 delta, one sub-layer 1..6, dense 21x21 | <= 20,306 raw wire bytes (20,303 command data) | <= 131,991 raw wire bytes (131,980 command data), two continuations |
| Complete colored MAP2 delta, all seven sub-layers, dense 21x21 | <= 53,823 raw wire bytes (53,819 command data) | <= 349,909 raw wire bytes (349,882 command data), six continuations |

The memory limits are exactly 32 bytes per resolved cell plus 8,192 bytes per
loaded map.  MAP2 volume is measured from the complete current serializer,
not by multiplying an isolated scalar/RGB field width.  With no object layers,
fog, support-height, or animation change, a colored lighting-only tile record
is 13 bytes for sub-layer 0, 23 bytes for one changed sub-layer 1..6 (the
`MAP2_MASK_LIGHT_LEVEL_MORE` form always carries all six higher scalar words),
and 61 bytes for all seven.  Each record includes the two-byte tile mask,
scalar word(s), one-byte layer count, one-byte extension flags, one-byte RGB
bitmap, and six RGB bytes for every occupied colored sub-layer.

For 441 tiles, a framed depth chunk adds its one-byte depth and four-byte
length.  A same-map first packet adds seven command-data bytes; a continuation
also adds seven.  Applying the serializer's greedy framing gives command-data
totals of 11,483/20,303/53,819 bytes for two depths and
74,618/131,980/349,882 bytes for thirteen depths.  The corresponding raw wire
upper bounds in the table add the current two- or three-byte transport length
and one-byte command for every packet.  These are uncompressed raw-wire upper
bounds; the default measured fixture disables optional compression.  Before
compression can be enabled for this budget, `packet_compress()` must retain
the original whenever the compressed stream plus its five-byte wrapper is not
smaller, and CI must remeasure final framed bytes.  The thirteen-depth
first-packet and
continuation command-data sizes are, respectively: 63,135 then 11,483 for
sub-layer 0; 60,930, 60,895, then 10,155 for one higher sub-layer; and 53,874,
five packets of 53,819, then 26,913 for all seven.

The representative two-depth, seven-layer lighting delta therefore fits the
65,534-byte complete MAP2 command-data limit before ordinary object overhead;
the legal thirteen-depth maximum uses the existing deterministic depth/tile
continuations, each independently capped at 65,534 bytes and 4,096 total
continuations.  It is an aggregate update budget, not a promise that every
depth fits in the first packet.  CI obtains all totals from the real MAP2
serializer/preflight, compares the exact packet split and aggregate raw bytes,
and fails if either increases.  Ordinary object data follows those same
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
Content validation rejects a dynamic aperture whose maximum clipped influence
would exceed the toggle limit before the map becomes runnable.  Runtime
lighting never vetoes or rolls back authoritative gameplay aperture state.  If
a dependency change nevertheless exceeds the estimate, commit gameplay state
immediately, publish the fail-closed zero-celestial affected tiles, and
schedule one bounded atomic structural rebuild; never perform an unbounded
synchronous flood.

CI records visit counts independently of wall-clock timing and retains the raw
samples.  It also proves insertion-order independence, exact unload/reload
reconstruction, old-or-new atomic publication, bounded region/seam
invalidation, aggregate-only authorization, and byte-identical output after
returning every phase and dynamic blocker to its original value.

#### Activation and cache digest serialization

All activation indexes are ASCII with no BOM or CR, use one LF after every
line including the last, lowercase hexadecimal, canonical decimal without a
leading zero (except `0`; a negative value is `-` plus that magnitude), and
literal tab separators.  A source revision is exactly 40 lowercase hexadecimal
digits; SHA-256 and aperture IDs have the exact lengths defined above.  Artifact paths are
case-sensitive repository-relative paths using `/`; they have no leading or
trailing slash, empty/`.`/`..` segment, backslash, tab, LF, CR, NUL, or
normalization alias.  Reject a duplicate or a row not in raw unsigned-byte path
order before hashing.  File SHA-256 values cover exact artifact bytes.

The complete manifest payload is the line
`atrinik-celestial-artifact-manifest-v1`, followed by one
`PATH<TAB>FILE_SHA256` line for every runtime-addressable artifact file.  The
schema sub-index payload is the line
`atrinik-celestial-schema-index-v1`, followed by one
`PATH<TAB>ROLE<TAB>SCHEMA<TAB>FILE_SHA256` line for every typed input.  Roles
are exactly `map`, `regions`, `archetype`, `python-factory`, `random-style`, or
`random-input`; schema is `1`.  Its file hash must equal the manifest row.

The migration payload is the line
`atrinik-celestial-migration-index-v1`.  A map row is
`map<TAB>PATH<TAB>SOURCE_REVISION<TAB>SOURCE_SHA256<TAB>DARKNESS<TAB>LIGHT<TAB>TARGET_V1_SHA256`;
an aperture row is
`aperture<TAB>PATH<TAB>LOCATOR_SHA256<TAB>APERTURE_ID`.  Sort by path, then
`map` before `aperture`, then by the remaining raw line bytes.  A locator digest
hashes `atrinik-celestial-legacy-aperture-v1`, LF, then canonical `map`, `type`,
`arch`, `x`, `y`, `faces`, `closed`, and `open` name/value lines in that order
with a final LF.  `faces` uses only the canonical non-empty `N,E,S,W` subset
order accepted by `celestial_faces`; `DOWN` is invalid for a dynamic aperture
and locator.  The other values use the exact schema spellings.  The target digest covers the
complete canonical v1 authored file, so no tool invents a separate undefined
“metadata digest.”

For a field-tile aperture key, hash
`atrinik-celestial-aperture-key-v1`, LF, followed by the intersecting sorted
`MAP_PATH<TAB>APERTURE_ID<TAB>GENERATION` lines and a final LF; canonical map
paths here are logical IDs with a leading `/`.  Zero intersecting apertures is
the prefix line alone.  The following tiny positive vectors freeze the
cross-tool byte contract:

```text
atrinik-celestial-artifact-manifest-v1
maps/a	0000000000000000000000000000000000000000000000000000000000000000
python/x.py	ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
```

Manifest SHA-256:
`e35a0622848b3ac1a36c6933ac73b0681150569b79c43dd9d09e5bbe7b8fc3cd`.

```text
atrinik-celestial-schema-index-v1
maps/a	map	1	0000000000000000000000000000000000000000000000000000000000000000
python/x.py	python-factory	1	ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
```

Schema-index SHA-256:
`9607b435619647542ae606641c02f4d2befa21623324e6927559f54f41b41269`.

```text
atrinik-celestial-migration-index-v1
map	maps/a	1111111111111111111111111111111111111111	0000000000000000000000000000000000000000000000000000000000000000	7	1280	2222222222222222222222222222222222222222222222222222222222222222
aperture	maps/a	3333333333333333333333333333333333333333333333333333333333333333	00000000000000ba
```

Migration-index SHA-256:
`00cc8a9a67dbf8d02ca1a8320ba6583fc78030110c6319acc289514b4d03d42c`.

```text
atrinik-celestial-aperture-key-v1
/maps/a	00000000000000ba	1
/maps/a	00000000000000bb	2
```

This aperture-key vector has SHA-256
`65bef960234de32faea4afb142d11bce52fac66d502534c1e311c4d31da8b48f`.
Tests also reject the same rows with swapped order, duplicate paths/IDs,
uppercase hex, CRLF, missing final LF, traversal segments, or a file digest
that differs from the manifest; aperture-locator tests also reject `DOWN`.

#### Breaking migration and editor vocabulary

Celestial-v1 is enabled only after this contract, server/schema validation,
regional profile support, content migration, and mutable-map upgrade support
land in their staged order.
Unsupported Classic/content pairings fail at startup; no legacy fallback is
allowed.

1. Structural validation and fail-closed linked loading land under #187,
   regional profiles under #189, and lunar/starlight evaluation under #191,
   while the old lighting path remains selected.  Runtime map factories,
   swap provenance/quarantine, and activation preflight land under #256.  The
   v1 factories and every caller migrate before ledger activation disables the
   legacy ambient setter, so generation never depends on that rejected path.
2. `content@main` migration #181 authors every `sky_above`, link boundary,
   structural exception, required static/`DOOR`/`GATE` archetype transmission,
   placed dynamic-aperture ID, and complete root region profile.  The Greyton
   house receives explicit
   topology or reviewed exceptions; its legacy rectangle is not translated
   into structure.
   The migration retains the stable `world` region identity and explicitly
   validates the 2,136 maps that resolve to it by omitted header.
3. Before any content cutover, #256 starts a versioned state-side provenance
   ledger for mutable maps: exact content revision, canonical source path, and
   source-file SHA-256 are written under the map-layout lock.  The migrated
   content artifact supplies a canonical predecessor-revision/source-digest to
   v1-metadata-digest index.  For each eligible source it also records the
   legacy loader-and-saver's normalized `(darkness,light)` pair and the reviewed
   target v1 neutral `light`.  Bootstrap compares a swap to that normalized
   source pair; equality means no proven runtime ambient override and uses the
   index's reviewed v1 value, never the swap's derived `light_value`.  This is
   especially required for legacy outdoor maps: saved `(7,1280)` derived from
   source `darkness -1` remains ignored legacy state and must not become v1
   ambient.  A different pre-ledger pair is ambiguous and quarantines.

   The same migration index maps each predecessor placed `DOOR`/`GATE` to its
   reviewed v1 aperture ID through a legacy locator digest.  The locator hashes
   canonical source map path, exact object type and archetype, `(x,y)`, and the
   reviewed face/closed/open classes; it deliberately excludes only the
   authoritative door/gate state selector.  Upgrade constructs the same
   candidates from the swap and requires one unique one-to-one match for every
   indexed and every resident dynamic aperture before assigning IDs.  A changed
   closed/open gameplay flag is preserved.  An insertion, deletion, moved or
   retyped aperture, non-state mutation affecting the locator, duplicate
   same-cell locator, unmatched candidate, or more than one possible matching
   quarantines the complete map without modifying it.  Content with duplicate
   predecessor locators is marked swap-upgrade-ineligible even though its
   authored target receives distinct IDs.  Fixtures cover state-only changes,
   reordered objects, duplicate same-cell apertures, insertion, deletion, and
   save/reload of assigned IDs.

   Ledger activation and removal of every legacy runtime ambient setter are one
   startup compatibility boundary.  From the instant provenance capture can
   begin, `set_map_darkness()`, Python `.darkness`, and any plugin equivalent
   reject before mutating resident state; generated v1 maps receive immutable
   ambient through their factory.  The bootstrap ledger records only the exact
   normalized swap pair it observed under the map-layout lock.  Equality with
   the indexed source pair uses the reviewed target value; any different pair,
   whether intentional or accidental before the ledger existed, is unprovable
   and quarantined for operator recovery.  No ambient-override ledger record
   exists, so there is no split transaction between an in-memory setter, a
   later swap save, and the ledger.  Fixtures cover unchanged indoor source,
   pre-ledger mismatch quarantine, rejected post-ledger mutation with no byte
   or ledger change, outdoor `darkness -1`/saved `(7,1280)` becoming its
   reviewed indexed value, restart, and the migrated immutable Food Haven
   factory value.

   Only an exact ledger/index match is recoverable.  Preserve runtime objects,
   choose neutral ambient by the rule above, discard legacy
   `darkness`/`outdoor`, and validate v1 bytes in a temporary sibling.  Each map
   upgrade is one journaled transaction under the map-layout lock.  A durable
   `PREPARED` journal names a unique transaction ID and the exact old/new
   swap and ledger paths plus SHA-256 digests.  Write and fsync both new
   siblings, atomically publish the journal, and fsync every containing
   directory before changing either live record.  Then rename the swap first,
   fsync its directory, atomically advance the journal to `MAP_COMMITTED`,
   rename the ledger, fsync its directory, atomically advance to `DONE`, and
   only then retire the journal and known transaction siblings and fsync those
   directories.  Every journal publication/advance is itself a sibling
   write-and-file-fsync, rename, and journal-directory-fsync sequence.

   Restart recovery is digest-driven and idempotent: live `(old swap, old
   ledger)` discards only journal-named siblings and retries; `(new swap, old
   ledger)` completes the verified ledger rename; `(new swap, new ledger)`
   records/recognizes `DONE` and cleans up; `(old swap, new ledger)`, an unknown
   digest, malformed journal, missing required sibling, or unjournaled temp is
   quarantined without overwriting or deleting evidence.  Journal state alone
   never overrides digest comparison.  Fault injection covers every write,
   file fsync, journal publication/advance, rename, directory fsync, and cleanup
   boundary and requires the same result after any number of restarts.  A
   pre-ledger, missing, changed, or malformed source is likewise unprovable and
   quarantined for operator recovery; player entry uses the existing savebed
   fallback and non-player state stays unavailable.  No mutable data is
   overwritten or structure synthesized from live objects.
4. Field implementation #188 enables celestial-v1 only after #256 verifies an
   immutable content-artifact feature, the artifact's complete canonical file
   manifest, and a sorted complete celestial schema sub-index before accepting
   connections.  The manifest covers every runtime-addressable artifact file
   with its path and exact SHA-256, including all maps, regions, archetypes,
   Python modules/scripts and live factory callers, random-map style maps,
   style directories/catalogs, and generator parameter inputs; one aggregate
   digest commits the ordered complete set.  The schema sub-index assigns the
   relevant role/version/manifest file SHA-256 to every map, region file,
   structural
   archetype, factory caller, and generator input despite ordinary lazy map
   loading.  No runtime loader may consume an extra or unindexed artifact
   file.  Missing, extra, mixed, partial, wrong-version, old-API caller, or
   digest-mismatched content aborts startup.  Before the feature is selected,
   the parser may understand v1 while legacy lighting remains authoritative;
   after selection, v1 is the sole path and no config toggle or fallback can
   weaken the gate.

Activation is operationally forward-only and entirely offline.  The
version-aware wrapper/supervisor first stops every game server and background
writer, then holds one exclusive cohort activation lease spanning content,
mutable state, `temp.maps`, provenance/journals, and the state-root selector.
No server, save tool, migration, or operator writer may run from snapshot start
through marker/root commit.  Record the generation/digest of every member at
lease acquisition and recheck them immediately before commit; any change not
made by the lease holder aborts without publishing the marker.  Per-map layout
locks remain internal transaction protection and never replace this global
writer exclusion.  Fault tests attempt a player save, swap eviction,
`temp.maps` rewrite, content replacement, and second activation during every
phase and require exclusion or abort.

Under that lease, before the first v1 mutable upgrade, take one recoverable
snapshot containing the exact Classic build, complete content artifact,
complete mutable state/provenance/journal set, activation metadata, and legacy
state-root selector, then verify its aggregate digest.  Upgraded state is
written only to a new versioned root `celestial-v1/GENERATION`; never in place
under the legacy root.  After every upgrade and preflight succeeds, commit one
durable marker containing schema `1`, generation, Classic build identity,
manifest/schema/migration aggregate digests, and snapshot identity, then
atomically replace the supervisor-owned state-root selector with that marker.
The legacy selector path becomes a regular fail-stop sentinel, not a directory.
Only then release the lease and accept connections.

Already-released binaries cannot be made to understand a future marker, so
refusal is enforced outside them: the supported wrapper/supervisor owns the
selected state root and supplies it only after exact build/marker validation;
the game process cannot traverse state roots directly.  It never starts a
pre-v1 binary for a v1 selector.  An attempted direct pre-v1 launch at the
legacy path encounters the non-directory sentinel and fails before opening or
creating mutable state; permissions deny it traversal of the v1 root.  A v1
binary likewise refuses a missing/mismatched marker.  Fixtures execute the
last supported pre-v1 binary through the wrapper and directly at the legacy
path and require failure before any file creation.

There is no in-place downgrade or partial fallback.  Rollback reacquires the
exclusive offline cohort lease and atomically restores the complete matched
pre-cutover Classic/content/state snapshot plus legacy selector before any
connection; restoring only map bytes, content, ledger, selector, or binary is
invalid.  Fixtures cover a crash before/after marker and selector commits,
mismatched cohort refusal, and successful full offline restoration.

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
rectangle,” or call a door or gate opening an exposure change.  It must visualize
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
- Mark only serialized, visible `EXIT` objects with an established usable
  destination with `MAP2_FLAG2_EXIT`, and cache that semantic per socket layer
  so gaining or losing destination eligibility emits a delta. Explicit-path,
  same-map coordinate, and tiled-direction exits are eligible. Pathless exits
  with a nonzero subtype use the automatic-link contract and are presumed
  eligible without scanning for a peer. Destination checks for rendering
  neither load maps nor choose randomly among auto-linked exits. The client
  outlines eligible objects after the complete world pass only at the player's
  physical depth. This presentation does not broaden line of sight or disclose
  layer-0/system exits, unexplored transitions, or hidden objects that the
  server did not serialize.
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
  partial or any cache reset. The client retains every independently validated
  envelope as bounded wire data until the complete sequence is present, then
  applies the batch without returning to input, widgets, or presentation.
  Continuations therefore never expose staged coordinates, cells, targets,
  music, weather, footsteps, or ambient-sound movement across a command-budget
  yield. Continuations never scroll or replace the active depth mask, and any
  apply-time failure rolls back the complete batch before its effects publish.
  One 21x21 depth gains at most 18,963 RGB bytes; a dense regression fixture
  forces a previously valid level across the boundary and verifies that every
  resulting packet remains within and passes the shared preflight.
- Do not add authored building/balcony/overlook flags or make serialization
  borrow/zoom magic-mirror targets.

Tests must cover lighting rebuild/load/unload, inside/outside buildings, delta
cache semantic changes, fog versus hard clear, roof silhouettes, disclosure
boundaries, cutaway connectivity, and depth-transition cache shifts.

## Remembered-world visibility and lighting semantics

This section is the normative Classic client contract for the remembered-world
work beginning with issue #386. It freezes presentation semantics before the
state and compositor implementations in #387, #388, and #389 change. It also
defines the authority boundary for the optional shadow research in #390 and the
measurements in #391. A later implementation may change representation, but it
must preserve these observable results and limits.

### Authority and terms

The server remains authoritative for every current MAP2 clear, fog-of-war
result, serialized object, door/gate state, and Q5.11 scalar/RGB radiance
sample. The client may retain only geometry that the server previously
authorized. It must never infer a current-visible cell, blocker, source, or
hidden object from artwork, remembered geometry, a wall silhouette, or a
client-side light value. The player's field is presentation-only: it is never
written to `MapCell.light_*`, sent back to the server, or used for gameplay,
targeting, pathfinding, or serialization.

The contract uses these terms:

| Term | Meaning |
| --- | --- |
| current | Geometry and live records authorized by the latest accepted MAP2 state for the cell and physical depth. |
| remembered | Previously authorized static geometry retained in the bounded five-window client cache after a soft clear or scroll. |
| transient | A living object, temporary effect, animation state, or interaction record whose lifetime follows current authorization. |
| soft clear | An ordinary MAP2/FOW loss that removes current/live presentation while retaining eligible remembered geometry. |
| hard clear | A map/cache invalidation that removes remembered geometry, live records, ownership metadata, and dependent resources. |
| sky/structural visibility | Camera presentation of authorized floors, walls, roofs, and linked-depth structure; it is separate from gameplay LOS. |
| player field | A fixed-point neutral presentation contribution centered on the local player and clipped by current server authorization. |
| stale | A revoked live record retained only while its bounded fade completes; it is not targetable or annotatable. |

### MAP2 classification

Classification is semantic and explicit. A layer or heuristic is not allowed to
turn an interactive or temporary object into remembered geometry merely because
it is drawn on a static-looking layer. The decoder records the classification
with each authorized object and applies the same result to live decoding and
the frozen MAP fixture validators.

| MAP2 object/layer | Remembered after soft clear | Current/live only | Required outcome |
| --- | --- | --- | --- |
| `FLOOR`, floor mask, support height, elevation, and painter metadata | Yes | No | Retain the latest authorized face, transform, owner depth, and support data. |
| `WALL`, roof, tree, and persistent map decoration | Yes | No | Retain static geometry and its explicit depth/elevation; do not retain actors attached to it. |
| Closed/open door or gate shell | Yes | No | Retain the shell and aperture class; a state change replaces the remembered geometry atomically. |
| Window, grate, curtain, and other persistent aperture decoration | Yes | No | Retain the authored/transmitted surface class; openness never broadens gameplay LOS by itself. |
| Multipart, tall, stretched, transformed, or `draw_double` static geometry | Yes | No | Retain every face and the painter/support metadata needed to reproduce the same projection. |
| Animated map decoration with no living/interactive state | Yes | No | Retain the static frame/identity and advance only while its bounded animation remains authorized. |
| `ITEM`/`ITEM2`, pickable item, container, corpse, and scripted object | No | Yes | Clear on soft loss; never make a pickable or script-controlled object permanent by layer. |
| Living player, monster, NPC, familiar, or summoned creature | No | Yes | Track as a transient record with deterministic alpha and expiry. |
| Spell, weather, firestorm, projectile, particle, temporary animation, or other effect | No | Yes | Track as a transient record; expiry cannot keep an idle map redraw alive. |
| Name, probe, target bar, pointer cue, exit interaction, status marker, or other annotation | No | Yes | Keep only with its current authoritative owner and cutoff; draw after lighting when eligible. |
| Script-controlled visibility or plugin-produced object | No implicit result | Yes unless explicitly classified by MAP2 metadata | Require an explicit server-authorized static classification; otherwise clear on soft loss. |

Doors, gates, corpses, containers, multipart objects, animated decoration,
and script-controlled visibility therefore have distinct outcomes. `blocksview`
controls the current server result; it is not a remembered-geometry predicate.
An object absent from authorized MAP2 history is never fabricated from a nearby
cell, a reused cache coordinate, or a client-side inference.

### State and transition contract

The remembered cell owns only static geometry, projection data, physical depth,
elevation/owner data, source/resource identity, and the last authorized map
revision. The live record owns current object identity, current alpha, latest
authoritative update, animation/effect state, interaction metadata, and target
eligibility. Both are keyed by map identity, physical depth, map coordinate, and
cache generation; coordinate reuse cannot resurrect a prior map.

| Input/event | Remembered state | Live/transient state | Invalidation and publication rule |
| --- | --- | --- | --- |
| New MAP2 cell/layer | Replace with the complete validated static result, or publish no remembered state for a transient-only result. | Replace only the records present in the transaction. | Validate the whole framed command and all continuations before publishing any part. |
| Same-cell delta | Keep unchanged static fields and replace only fields named by the delta. | Replace or remove only authoritative records named by the delta. | A semantic blocker, fog, depth, or resource change increments the affected revision. |
| Partial/continuation update | Retain validated envelopes without mutating live state; apply the complete batch in one unpublished transaction. | Retain validated envelopes without mutating live state; publish deferred audiovisual and interaction effects only with the final map generation. | The prior valid map state and retained primary/minimap projections remain the sole displayed/read/action generation across command-drain yields; missing, duplicate, out-of-order, oversize, malformed, interrupted, or renderer-recovery continuations discard the buffered batch or roll back apply-time state, including the movement reference. |
| Soft LOS/FOW clear | Keep eligible remembered static geometry and its last authorized resources. | Remove current records or start bounded fade-out; remove interaction metadata at its cutoff. | No player boost or current live object is allowed behind the lost authorization. |
| Hard clear/map replacement | Destroy all remembered geometry, owners, fields, source locks, and cache entries for the affected identity. | Destroy every live/transient record and annotation. | Publish an empty generation only after dependents are invalidated; stale data cannot reappear. |
| Scroll out of the live 17-by-17 window | Translate/reuse only the matching physical cache coordinates; retain no more than the five-window bound. | Mark out-of-window records not currently authorized and begin fade/expiry. | Reuse is valid only when map identity, depth, revision, transform, and resource identity match. |
| Scroll back/A-to-B-to-A | Reproduce the last authorized remembered geometry for A. | Do not restore actors/effects/annotations without a newer authoritative update. | A hard reset or map identity change makes A unavailable, not recoverable from B. |
| Linked-depth add/remove/shift | Preserve signed physical-depth/elevation relationships; allocate only within `MAP2_LEVELS`. | Clear records belonging to removed or shifted depths. | Missing, cyclic, misaligned, or unresolved links fail closed and invalidate affected columns. |
| Visibility enter | Keep remembered geometry and accept current records from the new MAP2 transaction. | Fade in only newly authorized records; local player is always fully visible. | Entering a radial field never substitutes for server authorization. |
| Visibility leave | Keep remembered geometry at the memory presentation floor. | Fade out, remove interactions, then expire at the fade bound. | No live target or annotation remains after current visibility is lost. |
| Stale expiry | Unchanged. | Remove the revoked visual/interaction payload while retaining a zero-alpha, generation-bound presentation tombstone for later fade-in. | The delta protocol never treats elapsed time without a packet as authoritative absence; expiry is deterministic and cannot schedule continuous redraw after alpha reaches zero. |
| Teleport, reconnect, logout, renderer shutdown, or reset | Hard clear the affected map/session generation. | Hard clear all live records and annotations. | Reconnect invalidates both retained world and minimap targets before a split first update can render; no client cache is trusted across identity change. |

The local render clock is an injected monotonic integer-millisecond clock. It
advances only while the presentation window is active, so minimizing or hiding
the client suspends rather than completes in-progress fades. It does not use
wall time or random state. Pausing, focus loss, and minimized
windows suspend the presentation clock; they do not advance fades or expire a
record. Resume consumes the next authoritative update and then advances from
the saved clock value. A new map, renderer reset, or reconnect starts a new
clock generation.

### Fixed visibility and light transfer

All distances and transfers use map-coordinate integer arithmetic. The field is
not calculated in screen pixels and does not become an isometric ellipse. The
frozen constants are:

| Quantity | Value |
| --- | ---: |
| Normal daylight raw radiance | 1280 |
| Remembered-geometry neutral floor (`M`) | 512 raw (40% of daylight) |
| Player-field neutral center (`P`) | 640 raw (50% of daylight) |
| Inner radius squared | 16 (radius 4) |
| Outer radius squared | 64 (radius 8) |
| Field weight unit | 256 |
| Actor/effect fade duration | 250 ms |
| Full alpha | 255 |
| Interaction cutoff | alpha below 192 or no current authorization |

For `d2 = dx*dx + dy*dy`, the field weight is `256` when `d2 <= 16`, `0`
when `d2 >= 64`, and otherwise:

```text
weight(d2) = floor((2 * (64 - d2) * 256 + 48) / (2 * 48))
```

This is round-half-up for the linear falloff between the two squared-radius
boundaries. The conformance vector is:

| `d2` | 0 | 1 | 4 | 8 | 16 | 17 | 25 | 32 | 36 | 48 | 49 | 64 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| weight | 256 | 256 | 256 | 256 | 256 | 251 | 208 | 171 | 149 | 85 | 80 | 0 |

The player contribution for a current-authorized cell is
`round_half_up(P * weight / 256)` added equally to scalar and RGB linear
channels. It is applied after interpolating the server's Q5.11 samples and
before tone mapping. It never changes the cached authoritative samples. The
local player uses alpha 255 and receives the same contribution at all field
positions where the player is rendered.

Remembered static geometry outside current authorization receives a neutral
memory lift, not a current-visibility grant. Let `S` be the decoded server
scalar and `C` its decoded linear RGB vector. The display-only values are:

```text
memory_lift = max(0, 512 - S)
S_display   = S + memory_lift
C_display   = max((0, 0, 0), C + (memory_lift, memory_lift, memory_lift))
```

Thus zero-radiance remembered geometry is `(512,512,512)`, a low colored local
sample keeps its color while receiving only the missing neutral floor, and a
negative or zero endpoint cannot produce a negative display value. The RGB
component clamp is display-only and does not alter the cached sample. At or
above raw 512 the server sample is unchanged. Current visible geometry does not get
the memory lift; it receives the authoritative sample plus the player-field
contribution. Q5.11 encoding remains the existing checked round-half-up
`raw * 8 / 5` operation at the wire boundary; this contract adds no protocol
field and no second visibility authority.

Fade alpha is integer and monotonic for one authoritative transition:

```text
fade_in(elapsed)  = min(255, floor((elapsed * 255 + 125) / 250))
fade_out(elapsed) = max(0, 255 - floor((elapsed * 255 + 125) / 250))
```

An authoritative reappearance cancels fade-out and starts from the current
alpha without overshoot. A newer authoritative absence replaces the prior
target and timestamp. At alpha zero the record and any redraw request are
removed; alpha-only changes repaint the affected bounded region but never
rebuild remembered geometry or the light field.

### Opt-in per-tile lighting diagnostics

Issue #483 adds a local, opt-in diagnostic view for the client-side lighting
pipeline. It does not add a MAP2 field, server request, socket flag, or source
identity. The server continues to send only the viewer-authorized aggregate
Q5.11 scalar/RGB sample described above; the client labels that received
aggregate and reports the transformations already used by the renderer.

Activation is through the client console:

```text
/d_lighting
/d_lighting tile <x> <y> [depth [sub-layer]]
/d_lighting grid [radius [depth [sub-layer]]]
```

Coordinates are the logical MAP2 wire-window coordinates, not absolute map
coordinates or a map path. The no-argument form reports the player-centered
wire tile. The optional grid is centered on that tile and bounded to radius
`0..4`. Every report starts with `lightdiag v1` and is one copyable key/value
line. `lightgrid v1` is a bounded status grid; its cells never contain raw
fogged values.

The single-tile record has these stages and fields:

| Field | Meaning |
| --- | --- |
| `state` | `visible`, `remembered`, `fogged`, `stale`, `missing`, and `cleared` flags for the requested tile/depth. |
| `source` / `received` | The current viewer-authorized server MAP2 aggregate and whether its scalar endpoint is present. No source position, emitter, component, or hidden object is included. |
| `unit` / `scalar` / `rgb` | Unsigned wire words in Q5.11; a sample word represents `sample / 2048`. RGB values are the received aggregate channels, not an inferred source color. |
| `keyframe` / `next` | The optional bounded MAP2 timed endpoint, generation, interval, and next scalar/RGB words. `replaced` means the timed endpoint supersedes the provisional current target. |
| `working` | The client value after temporal interpolation and the existing bounded nearest-known-ring fallback. On a visible primary-depth tile it also includes the presentation-only local-player field. `interpolated` and `borrowed` identify those transformations. |
| `presentation` | Smooth mode reports tone-mapped sRGB illumination RGB and neutral brightness; discrete mode reports the existing scalar level lookup and neutral RGB. This is illumination presentation, not a texture/albedo pixel. |
| `reasons` | Comma-separated `zero`, `unavailable`, `stale`, `clamped`, `replaced`, and `borrowed` explanations; `none` means no listed condition. |

Fogged tiles deliberately redact `received`, `keyframe`, `working`, and
`presentation` numbers (the record prints `redacted`) while retaining the
visibility/remembered/stale/missing/cleared state. This makes fog-boundary
inspection useful without creating a hidden-map disclosure channel. The
diagnostic reads the already-published local cache only when the user invokes
it; normal MAP2 decoding, rendering, wire size, allocations, redraws, and
benchmark counters are unchanged while it is disabled. If a MAP2 transaction
is still being assembled, the diagnostic reports the tile as unavailable until
the complete publication is visible.

### Unified composition and resource contract

The normal MAP decoder and `map_draw_map()` path remain the only scene-building
path. The compositor performs these phases in order:

1. Validate and publish MAP2 state, including current fog/clear, depth, owner,
   alpha, transform, and server Q5.11 samples.
2. Resolve the bounded remembered/live scene without synthesizing absent cells.
3. Paint albedo, alpha, color-key, transformed, double-face, stretch, and
   multipart geometry in the established global isometric order.
4. For every final visible pixel/span, retain the physical depth/elevation
   owner (or an equivalent surface-light coordinate) and select its authorized
   interpolated sample.
5. Apply the player contribution or remembered memory lift, then perform one
   scene-linear tone-map/multiply traversal for the complete primary map.
6. Draw names, probes, target bars, pointer cues, exits, and other annotations
   in one documented post-light phase only when their current cutoff permits.

Color-key pixels remain transparent and write no owner. True alpha and surface
alpha modulate the final albedo contribution; they do not discard the owner
metadata of a partially transparent surface unless the existing painter marks
the span transparent. Outlines and glows use the same owner/light result as
their source sprite. UI annotations are unlit. A texture, allocation, shader,
target, submission, swapchain, device, or output failure discards the partial
frame, stops presentation, and performs at most one complete GPU
device/resource reconstruction followed by a complete scene republish. It
never displays a partially composed, stale, or previous-map frame and never
selects a software fallback.

For viewport pixel count `N = width * height`, active physical depths `D` are
bounded by `MAP2_LEVELS` and the configured viewport limit. Retained buffers
must satisfy these hard formulas, including pitch and allocator overhead:

| Resource | Bound |
| --- | --- |
| Albedo target | one RGBA8 `N`-pixel GPU texture |
| Owner/sample target | one R32_UINT `N`-pixel GPU texture |
| Final map target | one RGBA8 `N`-pixel GPU texture |
| Compact scalar/RGB light data | one record per projected populated light cell; record-count proportional and never `N * D` |
| Compact spatial lookup | one coarse viewport bucket table plus bounded quad/bucket overlaps |
| Static transformed/effect cache | existing explicit byte/entry cap; no uncapped fallback cache |
| Live records | at most the bounded MAP2 command/object count for the active generation |
| Painter submission | retained primary/auxiliary command arrays plus one persistent, cycled GPU instance stream; only adjacent equal texture/scissor state is batched |
| Retained physical depths | `D <= MAP2_LEVELS == 2 * MAP2_MAX_DEPTH + 1` |
| Compositions | one ordered albedo/owner pass and one final integer light/tone pass per complete primary draw |

The implementation exposes GPU counters for command construction,
batches/draws, source and compact-light uploads, resource creation/destruction,
albedo/owner work, final light/tone work, UI, submission, fenced completion,
present wait, retained bytes, recovery, and fallbacks. After warmup an unchanged
scene has no source/effect upload or resource churn. Idle after fades and timed
buckets settle has no visibility, shadow, or map-state reconstruction work.
Player screenshots enqueue a completed-frame GPU copy and return immediately;
the client polls its fence on later iterations before PNG encoding. Synchronous
readback exists only for explicit conformance checkpoints.

The primary world traversal visits the complete negotiated wire window,
including its two-tile overscan on all four edges. Candidate admission never
uses only the 48-by-24 owning-tile anchor: wide and tall sprites may project
into the viewport from any overscan owner. GPU scissoring rejects their actual
off-screen pixels. Projected ground/roof lighting resolves one compact row key
per destination pixel; structural sprites retain their fixed sample row. The
projected-row lookup uploads only changed contiguous key runs after warmup.

### Optional shadows and dependency gates

The server's aggregate celestial/local radiance and structural spill remain
authoritative. Optional client contact or directional shadows may be proposed
only as a bounded presentation derived from visible authorized geometry and the
already decoded aggregate field. They must not expose source positions,
identities, hidden blockers, celestial profiles, or new MAP2 authority. #390
must measure at least two bounded techniques on the same deterministic fixtures,
separate server-produced shadow/spill from client projection, and either land a
separate reviewed contract or reject the feature. No production shadow or wire
change is implied by this document.

The implementation dependency graph is:

| Work | Consumes | Must not redefine |
| --- | --- | --- |
| #387 remembered/live state | classification and transition tables above | MAP2 framing, server disclosure, or light math |
| #388 visibility and fades | current authorization, field vectors, fade/expiry rules | gameplay LOS, server radiance, or compositor ownership |
| #389 unified compositor | remembered/live state, player transfer, owner/elevation, resource bounds | classification, protocol authority, or shadow research |
| #390 optional shadows | unified compositor and aggregate radiance | hidden source identities, new wire data, or parent budgets |
| #391 performance proof | all counters, cache bounds, and redraw rules | a second benchmark schema or portable timing exception |
| #142 renderer context | the complete renderer state/lifecycle contract | a parallel decoder or alternate map renderer |

These children may develop in parallel only against this contract. A change to
one frozen value or authority boundary requires a new design review and a
coordinated update to all consumers before implementation proceeds.

### Conformance vectors and review gates

The following vectors are mandatory in unit/fixture coverage:

| Case | Remembered display scalar/RGB | Player contribution | Alpha |
| --- | --- | --- | ---: |
| never-seen cell | empty; no output | none | 0 |
| remembered, zero server radiance | `(512,512,512)` | none | 255 for static geometry |
| remembered, raw server `(80,0,0)` | `(512,432,432)` after neutral lift | none | 255 |
| current visible, neutral raw 1280, center | `(1280,1280,1280)` | `640` each channel before tone mapping | 255 |
| current visible, neutral raw 1280, `d2=25` | `(1280,1280,1280)` | `520` each channel | 255 |
| fade-in at 125 ms | unchanged light | unchanged | 128 |
| fade-out at 125 ms | unchanged static geometry | none | 127 |
| revoked at 500 ms | unchanged remembered geometry only | none | 0 for live record |

Tests must cover every classification row, new/same/partial MAP2, soft/hard
clear, scroll out/back, A-to-B-to-A, resize, teleport, reconnect, pause and
focus resume, linked-depth add/remove/shift, closed/open `blocksview`, colored
and negative radiance, zero and timed endpoints, alpha/transforms/double faces,
allocation and lock failure, and deterministic reset. The same fixtures must
exercise the production decoder and `map_draw_map()` path. The review gates are
the issue acceptance criteria: no implicit layer/lifecycle case, exact order
and vectors, hard resource bounds, one-pass ownership, preserved #185/#188/#271
authority/confidentiality, and an implementation graph that lets the remaining
children land without semantic drift.
