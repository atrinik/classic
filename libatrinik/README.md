# libatrinik

[![Coverage](https://codecov.io/gh/atrinik/classic/graph/badge.svg?branch=main&flag=libatrinik)](https://codecov.io/gh/atrinik/classic)

`libatrinik` is the reusable C17 networking and utility library shared by the
Atrinik classic client, server, tests, and tooling. Its public CMake target is
`Atrinik::Core`; the installed static library is named `libatrinik`.

Integrated classic builds use the sibling `protocol/` source from the same
commit. Standalone builds retain a checksum-pinned protocol archive fallback
from `dependencies.lock.json`. Override that source explicitly when composing
another checkout:

```sh
cmake -S . -B build \
  -DATRINIK_PROTOCOL_SOURCE_DIR=/path/to/protocol
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

To collect line, function, and branch coverage from the native tests:

```sh
cmake --preset linux-coverage
cmake --build --preset linux-coverage
ctest --preset linux-coverage
gcovr --root . --filter '.*\.c$' --exclude '.*/tests/.*' \
  --exclude '.*/build/.*' --print-summary
```

To run the standalone suite with address and undefined-behavior sanitizers:

```sh
cmake --preset linux-sanitizers
cmake --build --preset linux-sanitizers
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
ctest --preset linux-sanitizers
```

Set `LIBATRINIK_USE_INSTALLED_PROTOCOL=ON` to use an installed
`AtrinikProtocol` CMake package instead of the locked source release.

Parent CMake projects that consume this source archive with `add_subdirectory`
must provide `Atrinik::Protocol` first and set `LIBATRINIK_PACKAGE_LAYOUT=ON`.
That selects the archive's namespaced public-header layout without enabling
standalone install or test targets.

Unified releases publish a scoped
`atrinik-classic-libatrinik-VERSION.tar.gz` development archive under the root
GPL-2.0-or-later license. The archive version matches the complete monorepo and
includes matching protocol source under `dependencies/protocol`; a standalone
CMake build selects that source automatically.

## Direct connection and rendezvous API

`socket_quic_client_create` owns the complete client connection attempt. It
pins any rendezvous invite's server ID to the expected QUIC certificate
fingerprint before discovery, and applies one absolute 15-second deadline to
address resolution, STUN, WebSocket authorization, candidate collection, and
the QUIC candidate race. Its optional `socket_connect_failure_t` output is a
bounded, credential-free value owned by the caller. The function borrows all
string and invite inputs only for the duration of the call.

Lower-level rendezvous consumers allocate a `socket_rendezvous_attempt_t` with
`socket_rendezvous_attempt_create`. The object copies its server ID, ticket,
invite, deadline, protocol state, and counters. It is mutable, must be released
with `socket_rendezvous_attempt_destroy`, and is not safe to share between
threads; independent attempts may run concurrently. Server candidate output is
transactional and remains empty if any frame is malformed, out of order,
over-budget, or belongs to another ticket. Invite and proof operations use
OpenSSL's compatible EVP signing interface; QUIC transport itself is enabled
only when building with OpenSSL 3.5 or newer.

Secret files use the shared `path_read_secret` primitive, which reads from the
same verified file handle, rejects another owner and symbolic-link/reparse
indirection, and reports non-owner permissions through its explicit
`permissive_mode` output. `path_secret_create_atomic` publishes an owner-only
file only after its contents are flushed, never replaces an existing path, and
returns a distinct collision result so concurrent creators can securely reread
the winner. Input and output buffers remain caller-owned; callers must cleanse
their own secret material after use.

## Metaserver publisher API

`metaserver_publisher_classic_body` and `metaserver_publisher_build` construct
the exact bounded JSON, RFC 9530 digest, and RFC 9421 signature input owned by
the shared metaserver-publisher fixture. Publisher identities are immutable,
caller-owned P-256 certificate/private-key pairs; signing borrows them and
returns only the public request signature. The matching verifier checks the
certificate's exact DER fingerprint and raw P1363 signature without retaining
input.

`metaserver_publish_sequence_reserve` persists a crash-safe uint64 high-water
mark before a caller attempts network I/O. Its two owner-only files are named
for the exact certificate-derived server ID, tolerate a crash between
publication and cleanup, allow gaps, never move backwards, and fail closed on
ambiguous or corrupt state. Recovery consumes the Worker's non-secret minimum
without weakening monotonic ordering. The shared publisher test exercises the
protocol-owned positive/negative vectors and sequence-file lifecycle on both
Linux and the native Windows CI runner.

## Pathfinding core

`Atrinik::Pathfinding` is a separate dependency-free C17 target for reusable
graph search. It can be configured without the networking toolkit or protocol
package:

```sh
cmake -S pathfinding -B build/pathfinding -DCMAKE_BUILD_TYPE=Release
cmake --build build/pathfinding --parallel
ctest --test-dir build/pathfinding --output-on-failure
```

Installing that build provides an independent `AtrinikPathfinding` CMake
package. Release consumers request the exact unified package version with
`find_package(AtrinikPathfinding 5.6.0 EXACT CONFIG REQUIRED)`,
link `Atrinik::Pathfinding`, and include `<atrinik/pathfinding.h>`. A normal
top-level libatrinik installation also installs this package, but it does not
make the pathfinding target link against `Atrinik::Core` or its dependencies.

The adapter maps application-owned 64-bit state IDs to deterministic neighbor
enumeration, a goal predicate, and optional heuristic, partial-ranking, and
cancellation callbacks. The core provides A*, Dijkstra, breadth-first, greedy
best-first, and reachability traversal. Each context owns its heap, hash index,
visited nodes, and result storage; independent contexts can be searched
concurrently or nested without global state.

Search result pointers remain valid until the context's next operation or its
destruction. A* returns an optimal route when the supplied heuristic is
admissible. Exact routes are the default. Best-effort paths require both the
`return_partial` option and an explicit `partial_rank` callback, and report
`ATRINIK_PF_PARTIAL` while retaining the underlying exhaustion, limit, or
cancellation reason. Generated-state, expanded-state, examined-transition, and
frontier budgets are per search; zero selects no limit. Adapters retain
ownership of their world state and may encode transition facts such as an exit
or seam coordinate in the metadata copied into each reconstructed step.

Run `build/pathfinding/tests/atrinik-pathfinding-benchmark` after the standalone
build to record cost, path length, expanded/generated states, examined
transitions, peak frontier, and wall time for the included 256-by-256 grid
fixture.
