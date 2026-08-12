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

`expected-pixels-sha256` hashes the viewport width and height as big-endian
32-bit integers followed by canonical RGBA bytes in row-major order. This is
the pixel-exact reference from the same primary software map surface that
`/screenshot map` saves; PNG encoder metadata is deliberately excluded.
