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

The exit-cue scene marks a base-level object covered later in the unified
painter order as a visible exit. A separate cached base-level exit is under fog
of war and must not receive the bright cue, while a positive-depth object is
also marked as an exit to prove that only visible physical depth zero receives
the post-world outline. The occluded sprite interior remains governed by the
normal painter order; only its outline is replayed after the world pass.

`expected-pixels-sha256` hashes the viewport width and height as big-endian
32-bit integers followed by canonical RGBA bytes in row-major order. This is
the pixel-exact reference from the same primary software map surface that
`/screenshot map` saves; PNG encoder metadata is deliberately excluded.
