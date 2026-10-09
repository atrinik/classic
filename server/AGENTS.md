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
- Region-map exploration is account-owned server state. Follow
  [`doc/REGION_EXPLORATION.md`](doc/REGION_EXPLORATION.md): only visible public
  base-level MAP2 cells grant discovery; retain account sidecars in backups and
  never import untrusted client `.tiles` files.
- Preserve object ownership, map activation/swap, lighting, plugin boundaries,
  and save transactionality. Test cleanup/rollback for lifecycle changes.
- Book writing uses server-held inventory/title/text snapshots and separate
  persistent `book_finalized`, signer, in-game date and UTC metadata. Preserve
  hidden/unpaid inventory checks, script/quest attributes, destination stack
  rejection, byte-based changed-text ink charging and atomic rejection before
  mutation. The offline `writing-books` scenario uses normal skill linking;
  provision and run it only through an isolated wrapper state. See
  [`doc/BOOK_WRITING.md`](doc/BOOK_WRITING.md) for persistence, protocol revision
  1083, refill rules and acceptance observations.
- While celestial-v1 runtime activation is inactive, preserve physical private-map
  and savebed paths through normal character saves, including saves on v1 maps.
  A map's schema alone does not select the process-wide persistence policy.
- With the active celestial-v1 runtime, player persistence uses owner-bound
  `unique-v1:` map/savebed tokens for private maps; physical datapath identities
  must fail closed and use the existing savebed/emergency fallback.
- Before celestial activation commits its forward-only marker, startup publishes
  a schema-2 snapshot plus a bounded, symlink-free archive at
  `celestial-activation-state` for the complete mutable datapath. Treat the
  archive and immutable Classic/content coordinates as one offline rollback
  cohort; never edit or restore either in place while a server is running.
- `install_data/` defines new-runtime defaults. Never handcraft, replace, or
  delete initialized account/player/key/identity state unless the task owns
  that mutable data.
- Local root-only update shutdown is opt-in; preserve the bounded protocol,
  idempotent countdown, and checked persistence/receipt contract in
  [`doc/LOCAL_ADMIN_SHUTDOWN.md`](doc/LOCAL_ADMIN_SHUTDOWN.md).
- Keep `--content_benchmark`, `--content_benchmark_route`, and
  `--provision_scenario` offline: no listeners,
  plugins, metaserver, or console. Benchmark canonical logical map IDs; scenario
  provisioning persists through normal account/password APIs. Use wrapper
  profiles/states/scenarios. Fixed `brynknot-v1` walking routes use initialized
  maps and normal collision checks; static export is planning evidence, while
  traversal requires the live client to acknowledge each normal movement step.
  See [walking-route export](doc/WALKING_ROUTE.md).

## Access admission and administration

- Protocol 1083 retains mandatory authenticated access policy before setup/account
  traffic. Retired SETUP subtype 3 stays reserved; no join-password fallback.
- `access_tokens.c` owns checked snapshot/audit/receipt/outbox transactions;
  `access_admin.c` owns strict bounded management JSON; `access_server.c` owns
  copied worker jobs and main-loop session checks. Keep socket/player pointers
  out of worker ownership, and propagate failed saves into shutdown receipts.
  Shutdown cancels in-flight access route IO before joining workers; ambiguous
  cancellation retains pending receipts/outbox entries for durable recovery.
- In-game access administration uses the current character’s existing
  `/cmd_permission` grant for `access`; `[OP]` grants it automatically. Local
  root administration remains available for bootstrap. Access policy/store
  settings are startup-only and cannot be changed through `/config`.
- See `doc/ACCESS_TOKEN_STORE.md` and `doc/LOCAL_ADMIN_SHUTDOWN.md` for durability,
  root bootstrap, private responses, exact online/offline status and backups.
  Never expose raw codes to command logs or ordinary packet dumps.

## Dependencies, protocols, and generated files

- Integrated builds use sibling `protocol/` and `libatrinik/`. Classic protocol
  must come from that sibling, the release's embedded `dependencies/protocol`
  tree, or an explicit source override so its wire revision cannot drift;
  libatrinik likewise requires matching sibling, embedded, or explicit source;
  a parent SDK may provide a compatible `Atrinik::Core` target. Historical
  standalone library releases are not a current API fallback.
  Content/resources also come from pinned releases; add no submodules.
- Packet-layout changes are coordinated protocol work. Generated IDs originate
  at `protocol/schema/game-commands.json`; never copy or renumber them locally.
- Edit Flex/CMake definition inputs rather than generated lexer/configured
  headers, and update `src/cmake.txt` for source additions/removals.
- For lighting, linked depths, MAP2 caches, cutaways, fog, or structural
  disclosure, read and preserve [`doc/MAP_RENDERING.md`](doc/MAP_RENDERING.md).
  The same document freezes the remembered/live client contract, player-field
  presentation, and unified-compositor boundaries. Gameplay LOS remains
  separate from camera structure and no visual change may disclose hidden
  gameplay state or radiance source identity.

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
- Development server images use the root publisher, honest source version
  `0.0.0`, full source revision and digest-pinned provenance. They do not advance
  stable releases or the public `latest` alias; see `../docs/RELEASING.md`.
- Commits/PR titles use Conventional Commits. Preserve unrelated work, keep
  generated output under `build/`, and finish with `git diff --check`.
- Update this guide when ownership, layout, commands, persistence/runtime, or
  validation changes; keep feature-specific algorithms in focused design docs.
