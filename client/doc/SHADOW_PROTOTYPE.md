# Bounded dynamic world-shadow prototype

This document records the issue #390 research result. The prototype is an
offline raster model used to compare bounded work and cache behavior. It is
not linked into the Classic renderer, does not alter production pixels, and
does not change the protocol.

## Authority boundary

Classic already receives one viewer-authorized aggregate scalar/RGB radiance
field. That field includes the server's celestial shadow/spill result, but it
does not identify the source, blocker, sky exposure, local-light component, or
sun/moon direction. The prototype therefore treats the aggregate field as the
only production-compatible lighting input. The directional technique receives
an oracle direction only to show the visual/temporal dependency; that input is
not available to a production client.

## Reproduction

From the Classic checkout root:

```sh
python3 client/tools/benchmark_shadow_prototypes.py \
  client/src/tests/fixtures/player_view/shadow-prototypes.json \
  --output build/shadow-prototype-evidence.json
python3 -m unittest client.tools.tests.test_benchmark_shadow_prototypes -v
```

The fixture is a closed 64x48 player view plus a roof-heavy dense-movement
scene. All three techniques consume the same two scenes, caster set, aggregate
field, blocked cells, eight-point movement route, and 480 movement + 16 idle
frame workload. It includes dawn, noon, dusk, night, full/new moon,
starlight-only, map-local colored light, negative light, and timed-keyframe
cases. The output contains SHA-256 checkpoints of the raster masks; these are
deterministic image checkpoints, not screenshots. Negative scalar input is
clamped to zero for the mask while both requested and effective scalars are
recorded; RGB cases record their peak channel while the prototype mask stays
monochrome. Timed oracle directions are evidence-only and are never treated as
production inputs.

The model bounds the viewport to 12,288 pixels, casters to 64, caster radius
to eight cells, and the incremental-work gate to 12,000 work units. Work units
are a stable proxy for processed pixels plus twice the emitted spans; they are
not a claim about wall-clock milliseconds. Caster inputs are composited in
ascending physical depth and then stable name order; opacity, fade, visibility,
and closed `blocksview` cells gate every technique. A production follow-up must
repeat the same matrix in the real SDL renderer before adopting any visual pass.

## Measured noon matrix

| Technique | Player-view p50 / p95 work | Roof-heavy p50 / p95 work | Player spans | Roof spans | Idle non-zero frames |
| --- | ---: | ---: | ---: | ---: | ---: |
| Contact/blob | 131 / 133 | 495 / 495 | 14,820 | 61,920 | 0 |
| Aggregate-field screen-space | 186 / 189 | 603 / 613 | 27,846 | 87,852 | 0 |
| Directional projected silhouette (oracle direction) | 149 / 161 | 385 / 411 | 19,493 | 36,338 | 0 |

Each technique performed 480 bounded updates, retained the same 16 idle cache
hits, and allocated one fixed 64x48 mask per update in the model. Hidden actors,
closed `blocksview` cells, zero-opacity/faded casters, and zero-radiance cells
cannot emit a mask. Elevation expands only the caster's bounded receiver
radius; transformed sprites and effects remain eligible only when visible.

## Decision

No candidate is approved for production cast shadows from this issue alone.
The directional result is the most visually expressive, but it changes with
the dawn/noon/dusk/timed oracle direction and therefore cannot be made
temporally correct from the current aggregate field. Sending a direction,
source identity, blocker map, or hidden-light component would violate the
#271 authority boundary and requires a separate bounded,
viewer-authorized protocol design.

The best compatible visual alternative is the aggregate-field screen-space
candidate as a separately scoped client-only follow-up: it can emphasize
already-authorized field transitions without claiming a source direction. A
contact/blob cue is the cheaper fallback for local depth, but it must not be
described as a solar/lunar cast shadow. Both alternatives need a real-renderer
visual review, cache invalidation design, and measured p50/p95 comparison with
the parent Standard and sustained-drift gates before production work begins.

The production fallback remains unchanged aggregate field lighting plus the
existing actor/effect fades. Settled idle performs no shadow work, and no
server, protocol, or production shadow implementation lands from this
research issue.
