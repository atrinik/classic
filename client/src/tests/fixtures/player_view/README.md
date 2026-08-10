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

`expected-pixels-sha256` hashes the viewport width and height as big-endian
32-bit integers followed by canonical RGBA bytes in row-major order. This is
the pixel-exact reference from the same primary software map surface that
`/screenshot map` saves; PNG encoder metadata is deliberately excluded.
