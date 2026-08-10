# Atrinik protocol

This directory is the canonical source for Atrinik's classic game wire command
IDs.
It generates matching C and Python bindings so the client, server, tests, and
future automation do not maintain independent numeric constants.

The game and metaserver protocols are separate contract families. This package
currently publishes only the classic game command registry. Add another family
only with its own namespace, specification, version, fixtures, and validation.

Protocol v1076 extends the client-to-server `CLEAR` payload. An empty payload
retains the historical broad queue/path clear used by Stay. A one-byte `MOVE`
or `FIRE` command ID selectively removes queued movement-stream commands of
that type; `FIRE` selects only the untagged directional form. This lets a held
direction change replace stale movement without discarding unrelated actions.

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
