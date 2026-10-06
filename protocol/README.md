# Atrinik protocol

This directory is the canonical source for Atrinik's classic game wire command
IDs.
It generates matching C and Python bindings so the client, server, tests, and
future automation do not maintain independent numeric constants.

The game and metaserver protocols are separate contract families. This package
currently publishes only the classic game command registry. Add another family
only with its own namespace, specification, version, fixtures, and validation.

Protocol v1082 adds the paired `BOOK_EDIT` commands for bounded book editing,
copying and permanent signing. Existing v1081 access command IDs are preserved;
BOOK_EDIT is appended at client-to-server ID 25 and server-to-client ID 32. The authored
[`book_edit.h`](include/atrinik/protocol/book_edit.h) specifies field order,
limits, actions and draft-preserving responses. Both endpoints validate a full
packet before publishing state. Editor sessions belong to the current playing
connection; reconnect requires applying a pen again. The existing protocol
version handshake rejects earlier peers before gameplay.
Signed `BOOK` responses also append two bounded NUL-terminated UTF-8 fields,
signer and in-game date (127 bytes each), after the existing book-message
terminator. Unsigned responses have no suffix. The client accepts either the
complete pair or no suffix and renders authenticated metadata outside editable
book markup.

Protocol v1081 replaces invitations and shared join passwords with independent
access codes. Authenticated `ACCESS_POLICY` precedes setup/account traffic;
protected connections require `ACCESS_AUTH` before proceeding. `ACCESS_ADMIN`
uses the active character's existing `/cmd_permission` grants, including
`[OP]`, and rechecks authorization before private result delivery. Retired
SETUP subtype 3 remains reserved. Admission and use history do not retain
network addresses or address-derived identifiers.

The shared access-authority contract is
[`atrinik/protocol` v2.9.0](https://github.com/atrinik/protocol/blob/6a725ca46d4ccea3f7c50d2d225abd4545265945/spec/access-tokens.md),
at commit `6a725ca46d4ccea3f7c50d2d225abd4545265945`. Its specification SHA-256 is
`bee74f5c4c7875c3d4ce81f97107a8e695472d28a58b36ce049889fb1b1e20ce`.
The released `atrinik-protocol-2.9.0.tar.gz` source archive has SHA-256
`e1388e317df69ec02639831553bb696e5104da1dde2ff4c7e244f163fff103af`;
`atrinik-protocol-contracts-2.9.0.tar.gz` has SHA-256
`dc2c94541e8e7b899125dd00bd7344f26cdd1612a4a4599fde51385c6c9ddc0a`.
These are contract provenance references, not Classic CMake dependencies.
Classic command IDs and C/Python bindings continue to come from this directory
and are included in the unified Classic source packages.

Protocol v1080 adds timed celestial aggregate-light keyframes to MAP2. Its
payload carries a bounded absolute game-time interval and next-endpoint samples;
the client interpolates locally without periodic MAP traffic.

Protocol v1079 added the server-to-client `PLAYER_STATUS` command. Its payload is
one bounded operation byte: a snapshot contains a 16-bit count and complete
entries, an upsert contains one complete entry, and a removal contains one
status key. Each entry contains a NUL-terminated stable key, a 16-bit face, a
NUL-terminated display name and tooltip, and a signed 32-bit remaining-seconds
value (`-1` means indefinite). Snapshots replace the client model
transactionally; upserts replace matching keys and removals are idempotent.
The generated limits bound status counts and every string field.

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
