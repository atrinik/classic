# Atrinik protocol

This directory is the canonical source for Atrinik's classic game wire command
IDs.
It generates matching C and Python bindings so the client, server, tests, and
future automation do not maintain independent numeric constants.

The game and metaserver protocols are separate contract families. This package
currently publishes only the classic game command registry. Add another family
only with its own namespace, specification, version, fixtures, and validation.

Protocol v1078 atomically replaces MAP2 scalar-byte and RGB888 lighting with
network-order Q5.11 scalar and RGB radiance words. The shared bounded preflight
validates the complete packet before client cache mutation; the v1077 lighting
layout is not retained as a compatibility path.

Protocol v1077 adds the server-authoritative `MAP2_FLAG2_EXIT` semantic for
visible type-66 map objects. Clients use it only for the main-map depth-zero
post-world cue; it does not disclose boundary-only, hidden, or unexplored
transitions.

Protocol v1076 adds a 32-bit keyboard movement epoch to `MOVE` and, after its
always-present 32-bit tag, `FIRE`. An empty client-to-server `CLEAR` payload
retains the historical broad queue/path clear used by Stay. A scoped payload
contains a `MOVE` or `FIRE` command ID followed by an epoch and cancels only
queued commands of that type and epoch. Epoch zero identifies ordinary direct
movement and is never replaceable. This lets a held direction change replace
stale movement without discarding standalone steps, ordered macros, mouse
actions, or unrelated commands.

Regenerate bindings after editing the schema:

```sh
python3 tools/generate.py
python3 -m unittest discover -s tests -p 'test_*.py'
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Unified releases publish the Python distribution as
`atrinik-classic-protocol` while preserving the `atrinik_protocol` import
package. Its wheel and scoped source archive use the repository-wide classic
version. Both include the root GPL license; the scoped source archive also
includes attributions and provenance evidence.
