# libatrinik

`libatrinik` is the reusable C17 networking and utility library shared by the
Atrinik classic client, server, tests, and tooling. Its public CMake target is
`Atrinik::Core`; the installed static library is named `libatrinik`.

The library directly depends on the separately released `atrinik/protocol`
package. By default CMake downloads the exact release archive and verifies the
SHA-256 recorded in `dependencies.lock.json`. For local protocol development,
override that source explicitly:

```sh
cmake -S . -B build \
  -DATRINIK_PROTOCOL_SOURCE_DIR=/path/to/protocol
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Set `LIBATRINIK_USE_INSTALLED_PROTOCOL=ON` to use an installed
`AtrinikProtocol` CMake package instead of the locked source release.

Parent CMake projects that consume this source archive with `add_subdirectory`
must provide `Atrinik::Protocol` first and set `LIBATRINIK_PACKAGE_LAYOUT=ON`.
That selects the archive's namespaced public-header layout without enabling
standalone install or test targets.

The first standalone release preserves the inherited GPL-2.0-or-later license.
Extraction into an independent repository does not relicense the code.

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
package. Consumers use `find_package(AtrinikPathfinding 1 CONFIG REQUIRED)`,
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
