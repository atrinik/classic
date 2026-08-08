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
