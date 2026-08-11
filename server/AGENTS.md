# Atrinik classic server module guide

## Ownership and orientation

- `server/` owns the C17 classic server, persistence/runtime contracts, and
  offline scenario provisioning. Content/media remain external.
- Read root `CONTRIBUTING.md`, the affected subsystem, and tests. Main startup
  is `src/server/main.c`; packet dispatch is `src/socket/server.c`; gameplay,
  objects, maps, simulation, and persistence live under neighboring `src/`
  modules. Fixtures/unit tests live under `src/tests/`.
- `plugin_arena` and required `plugin_python` are loadable modules. The server
  owns transport-neutral asset staging and immutable size/digest metadata;
  QUIC is the default delivery path. `http_url` is an operator-managed origin;
  never start or supervise an HTTP listener here.

## Change and persistence rules

- Persist spell/skill identities as stable strings, never table positions.
  Review `doc/METRICS.md` and the metric registry/hooks for gameplay changes;
  metric names, subjects, and event semantics are durable save contracts.
- Preserve object ownership, map activation/swap, lighting, plugin boundaries,
  and save transactionality. Test cleanup/rollback for lifecycle changes.
- `install_data/` defines new-runtime defaults. Never handcraft, replace, or
  delete initialized account/player/key/identity state unless the task owns
  that mutable data.
- Keep `--content_benchmark` and `--provision_scenario` offline: no listeners,
  plugins, metaserver, or console. Benchmark canonical logical map IDs; scenario
  provisioning persists through normal account/password APIs. Use wrapper
  profiles/states/scenarios.

## Dependencies, protocols, and generated files

- Integrated builds use sibling `protocol/` and `libatrinik/`. Classic protocol
  must come from that sibling, the release's embedded `dependencies/protocol`
  tree, or an explicit source override so its wire revision cannot drift;
  libatrinik remains an immutable checksum-pinned release fallback.
  Content/resources also come from pinned releases; add no submodules.
- Packet-layout changes are coordinated protocol work. Generated IDs originate
  at `protocol/schema/game-commands.json`; never copy or renumber them locally.
- Edit Flex/CMake definition inputs rather than generated lexer/configured
  headers, and update `src/cmake.txt` for source additions/removals.
- For lighting, linked depths, MAP2 caches, cutaways, fog, or structural
  disclosure, read and preserve [`doc/MAP_RENDERING.md`](doc/MAP_RENDERING.md).
  Gameplay LOS remains separate from camera structure and no visual change may
  disclose hidden gameplay state.

## Validation and release

- Follow root formatting and treat warnings/sanitizers as defects. Build/test
  through `./atrinik build server --profile PROFILE --test`; runtime checks use
  exact `topology show`/`up`/`ps`/`logs`/`down` with isolated state/scenario.
- For substantial native logic, run the documented `linux-coverage` preset and
  gcovr summary; pull-request CI also runs `linux-sanitizers`.
- Classic has one release line. Preserve source, Windows package, checksum, and
  server-image contracts. Scoped source packages embed matching protocol/
  libatrinik under `dependencies/` and select those repository-owned inputs
  without network access. Independently pinned third-party FetchContent sources
  retain their checksum-verified fallback unless a release contract explicitly
  bundles them.
- Commits/PR titles use Conventional Commits. Preserve unrelated work, keep
  generated output under `build/`, and finish with `git diff --check`.
- Update this guide when ownership, layout, commands, persistence/runtime, or
  validation changes; keep feature-specific algorithms in focused design docs.
