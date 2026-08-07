# Atrinik server repository guide

- This repository owns the C17 classic game server, its persistence and runtime
  contracts, and offline scenario provisioning. Authored world data and media
  remain in `content` and `resources`.
- Read `CONTRIBUTING.md`, the affected subsystem, and its tests before editing.
  Use precise component and protocol names; do not reference confidential or
  unreleased work.
- Protocol and libatrinik sources must come from immutable, checksum-pinned
  releases in `cmake/dependencies.lock.json`.
- Content and runtime resources must come from immutable, checksum-pinned
  releases in `dependencies.lock.json`; do not introduce Git submodules.
- Treat a packet-layout change as coordinated protocol work. Generated command
  identifiers originate in `protocol/schema/game-commands.json`; do not copy or
  renumber them locally.
- Persist spell and skill identities as stable `spell_id` and `skill_id`
  strings. Numeric `stats.sp` table positions and array order are runtime-only,
  not durable identities.
- Preserve object ownership, map activation and swap invariants, lighting
  propagation, plugin boundaries, and save-file transactionality. Add tests for
  cleanup and rollback paths when changing lifecycle code.
- Keep `--provision_scenario` offline: it must not start listeners, plugins,
  metaserver registration, or an interactive console, and it must persist via
  normal password/account APIs. Use `./atrinik scenario` from the workspace;
  never handcraft account or player files.
- Follow `.clang-format`, update CMake source lists, and treat warnings and
  sanitizer findings as defects. Validate dependency locks when they change.
- Build and test through `./atrinik build server --profile PROFILE --test`.
  For runtime behavior use the wrapper's exact
  `topology show`/`up`/`ps`/`logs`/`down` lifecycle with an isolated state and,
  when useful, a test scenario.
- For substantial native logic changes, also run the `linux-coverage` preset
  and gcovr summary documented in `README.md`; keep source/test exclusions
  intentional.
- Pull request titles and commits use Conventional Commits style.
- Every squash merge is released by semantic-release; preserve the source,
  Windows server package, checksum, and server-image release jobs together.
- Preserve unrelated work, keep generated output under `build/`, and finish
  with `git diff --check`.
- Update this `AGENTS.md` in the same change when major rework alters ownership,
  layout, commands, persistence/runtime invariants, or validation expectations.
