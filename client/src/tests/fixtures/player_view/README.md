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

The colored scene retains neutral ambient cells and adds the normative subtle
warm and cool full-day vectors together with resolved red, blue, and magenta
overlap samples across ground, fog, ordinary and elevated objects, walls, a
roof-only cell, and physical depths zero, +1, and +2. Separate smooth and
discrete manifests freeze both rendering paths; discrete intentionally retains
the authoritative scalar projection while smooth lighting applies RGB.

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
primary-map pass reveals only the transformed yellow silhouette.

The non-primary outline scene replays the nearby-living snapshot onto a surface
other than the map widget. It models the dynamic minimap's `map_draw_map()` call
and freezes the retained legacy living cue without enabling the new outline.

The widget-state scene freezes `sans.ttf`, enables names and target UI, renders
through the real widget zoom/blit path at 125%, then applies a second validated
MAP update that scrolls the cache and redraws the unobscured local player at the
new center. The harness asserts that both UI paths executed and separately
hashes the UI-enabled pixels, including the name, target label, health bar,
placement, and ordering. It then disables the UI and hashes a second
deterministic reference for the moved, zoomed player without an outline.

`expected-pixels-sha256` and the widget scene's
`expected-ui-pixels-sha256` hash the viewport width and height as big-endian
32-bit integers followed by canonical RGBA bytes in row-major order. Except for
the explicit non-primary regression scene, these are pixel-exact references
from the same primary software map surface that `/screenshot map` saves; PNG
encoder metadata is deliberately excluded.
