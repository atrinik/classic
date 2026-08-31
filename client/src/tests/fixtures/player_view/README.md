# Frozen renderer migration fixtures

These XML manifests preserve the pre-cutover renderer's viewport, logical map
size, lighting mode, zoom behavior, clock, settings defaults, multipart geometry,
MAP command, and every image by SHA-256. They are immutable inputs for schema
checks and qualified GPU conformance jobs; no client executable replays them
through the removed CPU renderer. The same bounded MAP command covers
ordinary and stretched terrain, a multipart sprite, a protocol animation,
fog, roof/cutaway data, smooth and discrete lighting, and physical depths
zero, +1, and +2.

The generated `gpu-qualification-town-25x25` snapshot supplies seven active
depths, nearly three thousand ordered sprite layers, mixed owner depths,
roof/door/exit/FOW and transform semantics, plus exactly 64 animated live
actors through the normal MAP2 decoder. Its zero expected hash is an explicit
pending-hardware marker: qualified runs record `golden_verified:false` until
reviewers approve and pin the cross-backend rendering contract.

Every GPU manifest pins the shared `data/interface.cfg` defaults and
`gpu-interface.cfg` saved layout in addition to its scene inputs. The
`gpu-ui-closure` manifest runs a named nineteen-state production-screen sweep
over the same renderer. It separately warms and checkpoints the intro server
browser, ready account/password login form, character selection, populated
gameplay widgets and text windows, a context menu, tooltip, opaque and fading
notification, generic input controls, book, settings, color picker, connection
preference, join-password, credits, painting and region-map popups, the
minimap, an observable zoomed region-map FOW update followed by an unchanged
retained frame, and the real `/screenshot` and `/screenshot map` commands.
Every non-readback state requires zero source upload, fallback, and
resource creation/destruction after warmup. Blink and notification-fade clocks
are frozen by the fixture, while inventory, party-list, text-window,
music-player, and render-profiler models are populated deterministically. The
fixture disables the normal user-data hierarchy, so no account, buddy, media,
or blacklist state is read or written. The screenshot states require one
asynchronously enqueued completed-frame readback with exact dimensions. The
map screenshot must equal the exact centered 75%-viewport rectangle of the
immediately preceding window screenshot, including its nonzero crop origin.
Their named backend hashes are part of the human-approved GPU golden contract.
The display-dependent fullscreen checkpoint is nested under
`fullscreen.<hardware-tier>.<pixel-width>x<pixel-height>` so reference and
minimum runners approve the exact 1:1 logical/output pixel mode they actually
exercised.

The separate player-sub-layer fixture leaves a positive-depth roof cell
without floor geometry and gives only its canonical object sub-layer a light
sample. It guards against shading such roofs with an unrelated empty player
sub-layer.

The colored scene retains neutral ambient cells and adds the normative subtle
warm and cool full-day vectors together with resolved red, blue, and magenta
overlap samples across ground, fog, ordinary and elevated objects, walls, a
roof-only cell, and physical depths zero, +1, and +2. Separate smooth and
discrete manifests freeze both rendering paths; discrete intentionally retains
the authoritative scalar projection while smooth lighting applies RGB.

The movement-colored fixture expands the same validated colored MAP2 geometry
to all five physical depths and covers every cell of the 17-by-17 visible look
area with varied scalar illumination. Its base depth has dense ordinary floor
geometry while the two lower and two upper depths have representative sparse
floor, wall, item, and effect objects. The generated field adds three explicit
colored-light accents per depth around the preserved source scene, modeling the
distribution of colored sources in ordinary play instead of stacking five
fully furnished, uniquely colored maps. The fixture pairs that scene with a
closed, length-prefixed MAP2 stream. Its four active
`MAP_UPDATE_CMD_SAME` packets move east, return to the origin, turn south, and
return again; a fifth `SAME` packet holds the origin without map data for the
idle phase. Every active step carries per-depth clear, floor-object,
scalar-light, and colored-light deltas, so the smooth and discrete manifests
exercise ordinary scroll/cache work without using `MAP_UPDATE_CMD_NEW` during
sustained or resumed movement.
The initial and same-process-repeat `NEW` packets are explicit resets, and each
whole active stream deterministically restores the origin. The `PVM1` envelope
is a fixture-only framing layer; each enclosed packet still goes through the
normal MAP validator and decoder. `generate_movement_five_depth.py` and
`generate_movement_delta.py` recreate the pinned hex inputs. Passing
`--transition` to the five-depth generator preserves the same bounded geometry
and assets while assigning a distinct map name and path to a second validated
`MAP_UPDATE_CMD_NEW` packet. Both movement manifests pin that transition and a
32-by-24-pixel resize delta so reset, resized, restored, and map-transition
checkpoints remain deterministic. The captured movement evidence cleared its
offscreen frame target before every map draw, matching the retired reference
compositor's per-frame contract; historical frame timing therefore includes
that clear and the full primary map draw. It also rendered the map core into the production
1700-by-1200 local-minimap view whenever the real 250-millisecond dynamic
minimap cadence is due. The current renderer builds that auxiliary world into
its retained GPU map target and applies the centered crop, scale, mask, and
border in the GPU widget path; it does not paint or scale a CPU minimap image.
Main-map and local-minimap calls and timings remain separate, while the fenced
complete-client frame includes the production widget tree, popup/tooltip
layers, and both map views. Current GPU qualification reuses the pinned inputs
and compares explicit readbacks on supported hardware; the historical
executable oracle is no longer built.

The `brynknot-movement` manifest is the roof-heavy companion workload for
dense Brynknot-style movement. It uses the same sanitized 17-by-17 MAP2
geometry and packet route as the colored fixture, but keeps furnished physical
depths `+1` and `+2` with both roof and effect layers so the ordinary painter
and roof/cutaway paths stay active at the 1024-by-780 viewport. Recreate its
snapshot with `generate_movement_five_depth.py --roof-heavy`; the manifest's
`movement_route` record reports the four accepted MAP2 packet coordinates with
deterministic simulated receive/apply timestamps. Those timestamps document
queue ordering and application timing only; they are not character-speed
measurements. The manifest is archival input for the qualified GPU conformance
harness. Test builds replay ordinary immutable MAP/MAP2 snapshots through
`--gpu-player-view`; the former CPU `--player-view-movement-benchmark`
executable path no longer exists.

The `dense-cursor` manifests reuse the frozen five-depth roof stack as a
Brynknot-style dense multi-depth fixture. The cursor replay records stationary,
world-pointer, UI-pointer, animation-only, and movement phases, including
frame/wait timings, redraw reasons, projected/structural lighting-cache
telemetry, sprite-cache counters, and pixel checkpoints. Stationary and both
pointer phases must perform zero full map draws; animation may perform only the
object pass; movement retains explicit packet-plus-scroll reasons. Pointer
motion restores only the bounded union of its old and new dirty regions over a
retained completed world.

Fixture schema 3 also defines a same-contract lighting reconstruction A/B over
the smooth five-depth snapshot and a separately generated movement-only SAME
stream. Its active packets carry all five depths with empty payloads, preserving
the snapshot's scalar and colored radiance while the camera alternates one tile
horizontally and vertically. Each candidate sample alternates the production
translated-field path with a benchmark-only full-field control. Both modes use
the same packets, stateful cache lifetime, viewport, checkpoints, Release
build, and fresh-process rules. Benchmark-only scopes attribute field
translation and dirty clearing, light rasterization and extrapolation,
destination tone-map/multiply, and transformed-sprite lookup, construction,
and invalidation. The isolated lighting-work duration is their non-overlapping
sum accumulated from before queued MAP decode through the primary map draw, so
it includes the reconstruction selected for that tick and excludes the
separately reported local minimap; total update work remains the production-like
end-to-end guard.

Each replay injects MAP state at a simulated 125-millisecond (8 Hz) update
cadence without sleeping. This is not a display-frame-rate target: reports show
measured replay-work capacity against a separate, informational 144 FPS
(6.944-millisecond) display reference. Hosted CI is not expected to attain that
rate; candidate-only runs establish its baseline, and schema-compatible changes
alternate base and candidate Release processes on the same runner before
reporting their timing deltas. Its phases are one cold `NEW` tick, 480 sustained
ticks (60 simulated seconds, one active packet per tick), 16 idle ticks (eight
unchanged `SAME` packets alternating with eight animation-only ticks), and 80
resumed ticks (two packets on each of the first eight ticks, no packets on the
next eight, then one packet on each of the remaining 64). That resumed overrun
intentionally creates and drains a bounded production-command-queue backlog.
The process then records resize, restored-size, reset, and the distinct map
transition checkpoints and repeats the complete replay in the same process. A
fresh-process verifier runs the selected viewport twice.

`expected-standard-checkpoint-sha256` identifies the archived software
renderer's ordered visual lifecycle for the standard viewport. GPU evidence
records and cohorts this immutable identity, while the separately
human-approved per-backend exact-RGBA8 contract governs completed GPU frames.
The archived digest is SHA-256 over the ASCII prefix
`pvm-checkpoints-v1\n`, followed by one line per checkpoint in replay order:
`name<TAB>pixels-sha256<TAB>map-x<TAB>map-y<TAB>viewport-width<TAB>viewport-height<NL>`.
Internal state digests are deliberately excluded, so implementation-only state
may evolve while any intermediate visual, position, ordering, or resize change
still fails the golden proof.

From the client directory, recreate the pinned movement inputs with:

```sh
python3 tools/generate_movement_five_depth.py \
  src/tests/fixtures/player_view/colored-scene.map2.hex \
  src/tests/fixtures/player_view/movement-colored-five-depth.map2.hex
python3 tools/generate_movement_five_depth.py \
  src/tests/fixtures/player_view/colored-scene.map2.hex \
  src/tests/fixtures/player_view/movement-colored-transition.map2.hex \
  --transition
python3 tools/generate_movement_delta.py \
  src/tests/fixtures/player_view/movement-colored-delta.map2.hex
python3 tools/generate_movement_delta.py \
  src/tests/fixtures/player_view/movement-lighting-static-delta.map2.hex \
  --static-radiance
python3 -m unittest -v tools.tests.test_movement_fixture
```

The bounded standard smooth and discrete determinism tests are separate from
the `long-performance` large-viewport tests. Run them explicitly with:

```sh
ctest --test-dir build/linux-release -L standard-performance --output-on-failure
ctest --test-dir build/linux-release -L long-performance --output-on-failure
```

The complete candidate-only reporting matrix is also explicit:

```sh
python3 tools/benchmark_movement_regression.py candidate-only \
  --candidate-client build/linux-release/atrinik \
  --candidate-manifest src/tests/fixtures/player_view/movement-colored.xml \
  --discrete-manifest src/tests/fixtures/player_view/movement-colored-discrete.xml \
  --lighting-manifest src/tests/fixtures/player_view/movement-lighting-isolated.xml \
  --full-matrix --output build/movement-full-matrix.json
```

Large smooth rendering is intentionally not part of the fast PR subset. It is
a multi-minute, hardware-dependent stress context: a 1920-by-1080 Linux Release
probe on the development runner exceeded five minutes and reached about 315 MiB
peak RSS before its 300-second probe ceiling. The long verifier therefore
allows up to 900 seconds per fresh process; reserve the full matrix for a runner
and job with an aggregate budget for all eight processes.

The radial-light scene freezes the default profiles for the applied portable
torch (strength 3) and wall sconce (strength 5) on a 13-by-13 floor. The
sconce's exposed boundary column receives light while the cells beyond it are
occluded. Smooth and discrete manifests use identical coordinates, revision
`atrinik/content@bf460d92ce8e42cb169cdff57c99638df2fd4d95`, camera, zoom,
assets, and MAP2 samples.

The exit-cue scene marks unobscured and later-occluded base-level objects as
EXIT-only positive cases; the occluded case also exercises `draw_double`. A
separate cached base-level exit is under fog of war and must not receive the
bright cue, while a positive-depth object is also marked as an exit to prove
that only visible physical depth zero receives the post-world outline. The
occluded sprite interior remains governed by the normal painter order; only its
outline is replayed after the world pass. Before the final visible state, the
snapshot also caches an exit on a secondary layer, soft-clears that cell, and
then reuses the tile for fresh non-exit data. This reaches the soft-FOW reset
that discards stale per-layer exit semantics before the new layer is decoded.

The four local-player outline scenes identify only the depth-zero center-cell
living object at the MAP header's player sub-layer. They freeze an unobscured
animated, rotated, non-uniformly zoomed and aligned player; a later-painted
same-level wall; an authorized positive-depth roof projected across the player;
and a nearby living object that must retain ordinary rendering. The unobscured
and nearby-living scenes receive no outline. In the other two scenes, the
structural occluders continue to hide the player's interior while the final
primary-map pass outlines only the transformed pixels that those structures
cover. Fully visible parts of the player retain their ordinary rendering.

The non-primary outline scene replays the nearby-living snapshot onto a surface
other than the map widget. It models the dynamic minimap's `map_draw_map()` call
and freezes the retained legacy living cue without enabling the new outline.

The living-outline scenes generalize that exact partial-occlusion contract to
non-local `LAYER_LIVING` commands. They freeze an unobscured actor, a same-level
wall, a positive-depth roof, a nearby but non-overlapping wall, a wall below the
established alpha threshold, a doubled actor, fully dark actors and walls, and
three independently masked actors where only one overlaps later structural
geometry. The local actor remains yellow while an occluded non-local actor
receives a cyan outline. A generated crowded scene
with 24 non-local actors and 24 later walls supplies bounded Release benchmark
input. A matching 24-actor scene without walls proves that projected-bounds
rejection avoids mask allocation when no actor is occluded.
`tools/generate_living_outline_fixtures.py` recreates every snapshot.

The centered visibility-fade scene places authoritative item, living, and
effect records on the MAP2 player cell. Its normal 320-by-240 player-view run
advances the presentation clock and asserts that current records remain at
full alpha, while the local player cannot enter a presentation fade. It then
expires one revoked item to its zero-alpha generation tombstone and verifies
that authoritative re-entry interpolates from zero instead of snapping opaque.

The remembered-floor smooth and discrete scenes first authorize a zero-radiance
floor beside a zero-radiance local actor, then soft-clear only that floor cell.
The normal MAP2 decoder must retain the authorized static floor while removing
its light state, and both renderer modes must show the remembered tile above a
clear black field at the fixed memory floor. The paired snapshots make the
fully-dark FOW transition a deterministic render fixture without adding a
wire field or live-object authorization.

The widget-state scene freezes `sans.ttf`, enables names and target UI, renders
through the real widget zoom/blit path at 125%, then applies a second validated
MAP update that scrolls the cache and redraws the unobscured local player at the
new center. The harness asserts that both UI paths executed and separately
hashes the UI-enabled pixels, including the name, target label, health bar,
placement, and ordering. It then disables the UI and hashes a second
deterministic reference for the moved, zoomed player without an outline.

The map-overlay widget scene uses a nonzero `(17, 23)` widget origin and 125%
zoom. It keeps damage and kill animations active across the validated MAP
scroll while names and target UI are drawn through the map surface. The
linked-depth companion uses a two-depth snapshot, an animation on non-player
living sub-layer 1 at depth +1, a post-scroll update, and an odd 321-by-241
source whose 125% displayed dimensions round independently. Overlay anchors
follow the exact source-to-displayed-surface transform; their font/icon
dimensions and 25-pixel rise over 850 ms remain screen-sized. Health/food
warnings are centered on the exact displayed surface without scaling their
textures. MAPSTATS text is horizontally centered there but retains its
historical fixed 300-pixel offset from the effective top edge; its font and
trajectory are also screen-sized.

The elevated-overlay companion gives both the player floor and the animation's
source sub-layer a pinned 24-pixel elevation. Test-only draw counters require
both damage and kill branches to execute in every final overlay frame, so a
digest without the overlays cannot be accepted. The manifests also pin the
production mono font and death texture used by those branches; out-of-domain
linked depths are rejected before replay.

`expected-pixels-sha256` and the widget scene's
`expected-ui-pixels-sha256` hash the viewport width and height as big-endian
32-bit integers followed by canonical RGBA bytes in row-major order. Except for
the explicit non-primary regression scene, these are pixel-exact references for
the completed map output. `/screenshot map` asynchronously enqueues an explicit
GPU readback of that map-widget rectangle and polls its fence on later client
iterations; PNG encoder metadata is deliberately excluded.
