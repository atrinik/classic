# Atrinik classic library module guide

- The `libatrinik/` module owns reusable C17 libraries. `Atrinik::Core` contains shared
  runtime utilities; `Atrinik::Pathfinding` is a separate dependency-free core
  that must remain independently buildable and installable from `pathfinding/`.
- Keep client/server policy and world-specific behavior in their consumers.
  Public APIs must document allocation, ownership, lifetime, mutability, error,
  and thread-safety contracts.
- Integrated builds use command bindings from sibling `protocol/`. Its
  immutable, checksum-pinned lock remains a release and standalone-build
  fallback. Update the lock, generated dependency, and consumer builds
  together; do not copy identifiers into library code.
- Pathfinding adapters own map and traversal state. A context owns its search
  heap and returned storage; results remain valid only until the next operation
  on that context or its destruction. Preserve deterministic tie-breaking, exact
  search by default, explicit partial results, and enforced work budgets.
- Keep pathfinding free of global state and external dependencies. Add tests for
  unreachable goals, budgets, partial paths, overflow, allocation failure, and
  independent contexts when changing the core.
- Follow the root `.clang-format` and CMake target/export conventions. Validate the
  top-level CMake/CTest suite, the standalone `pathfinding/` build and tests,
  install/package consumption, and downstream client/server builds through the
  workspace when a public API changes.
- For substantial native logic changes, also run the `linux-coverage` preset
  and gcovr summary documented in `README.md`; include both core and standalone
  pathfinding coverage without counting test or build output as source.
- Commits and pull-request titles use Conventional Commits. Classic uses one
  repository-wide release line.
- Keep generated output under `build/`, preserve unrelated work, and finish
  with `git diff --check`.
- Update this `AGENTS.md` in the same change when major rework alters ownership,
  targets, public API/lifetime contracts, build layout, or validation.
