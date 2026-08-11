# Offline player-view fixture

The XML manifests freeze the software-renderer viewport, logical map size,
lighting mode, zoom behavior, clock, settings defaults, multipart geometry,
MAP command, and every image by SHA-256. The same bounded MAP command covers
ordinary and stretched terrain, a multipart sprite, a protocol animation,
fog, roof/cutaway data, smooth and discrete lighting, and physical depths
zero, +1, and +2.

The separate player-sub-layer fixture leaves a positive-depth roof cell
without floor geometry and gives only its canonical object sub-layer a light
sample. It guards against shading such roofs with an unrelated empty player
sub-layer.

The colored scene retains neutral ambient cells and adds resolved red, blue,
and magenta overlap samples across ground, fog, ordinary objects, walls, a
roof-only cell, and physical depths zero, +1, and +2. Separate smooth and
discrete manifests freeze both rendering paths; discrete intentionally retains
the authoritative scalar projection while smooth lighting applies RGB.

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
and a nearby living object that must retain ordinary rendering. The structural
occluders continue to hide the player's interior while the final primary-map
pass reveals only the transformed yellow silhouette.

The non-primary outline scene replays the nearby-living snapshot onto a surface
other than the map widget. It models the dynamic minimap's `map_draw_map()` call
and freezes the retained legacy living cue without enabling the new outline.

`expected-pixels-sha256` hashes the viewport width and height as big-endian
32-bit integers followed by canonical RGBA bytes in row-major order. Except for
the explicit non-primary regression scene, this is the pixel-exact reference
from the same primary software map surface that `/screenshot map` saves; PNG
encoder metadata is deliberately excluded.
