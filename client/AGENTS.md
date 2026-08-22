# Atrinik classic client module guide

- The `client/` module owns the SDL3 C17 game client. Keep server, protocol,
  libatrinik, sound, and content implementation in their owning sibling modules
  or external repositories.
- Read the root `CONTRIBUTING.md`, `INSTALL`, the affected subsystem, and its
  tests before editing. Use precise component and protocol names; do not
  reference confidential or unreleased work.
- Integrated builds use sibling `protocol/` and `libatrinik/` sources. Classic
  protocol must come from that sibling, the release's embedded
  `dependencies/protocol` tree, or an explicit source override so its wire
  revision cannot drift; libatrinik retains an immutable checksum-pinned
  release fallback. Update lock files and dependency tests together. Do not add
  Git submodules or copied protocol constants.
- The scoped client source release embeds matching protocol and libatrinik
  trees under `dependencies/`; standalone CMake configuration must select them
  without network access when ordinary monorepo siblings are absent.
- Command identifiers are generated from the sibling `protocol/` module. Treat
  packet-layout changes as coordinated monorepo protocol work and update both
  producer and consumer in one pull request and profile.
- Preserve SDL3 surface ownership, color keys, alpha/blend modes, clipping,
  texture invalidation, and mutable-backbuffer semantics. Exact black remains
  the transparency key when image decoding falls back, and mutable chat
  backbuffers must not be RLE encoded.
- For remembered-world visibility, lighting, fades, invalidation, and unified
  composition, [`server/doc/MAP_RENDERING.md`](../server/doc/MAP_RENDERING.md)
  is the shared normative contract. Keep remembered static geometry separate
  from current/live MAP2 records, treat server clears and Q5.11 radiance as
  authoritative, and keep player light presentation-only.
- Keep offline player-view proofs on the normal MAP decoder and
  `map_draw_map()` path. Their closed manifests must pin every immutable input
  and renderer choice, remain bounded and network-free, and never read or
  write the normal user configuration/cache hierarchy.
- Focused text inputs own their key-down, key-up, text-input, and text-editing
  events. Do not let gameplay bindings observe an event already consumed by a
  focused widget.
- Client user data lives below `.atrinik/<major>.x/`. When that stable directory
  is first created, the client may migrate the highest valid same-major legacy
  directory; the migration is collision-safe and marker-backed, leaves other
  major lines untouched, and resumes after interruption or a user-file conflict.
- Follow the root `.clang-format`, existing allocation/error conventions, and CMake
  source lists. Add focused tests for renderer, input, parser, and lifecycle
  regressions.
- Validate dependency changes with `python3 tools/dependencies.py verify` and
  all native changes with `./atrinik build client --profile PROFILE --test`
  from the workspace. Use the workspace topology lifecycle for live checks.
- For substantial native logic changes, also run the `linux-coverage` preset
  and gcovr summary documented in `README.md`; keep source/test exclusions
  intentional.
- Commits and pull-request titles use Conventional Commits. Classic uses one
  repository-wide release line; keep client release assets coherent with it.
- Keep generated output under `build/`, preserve unrelated work, and finish
  with `git diff --check`.
- Update this `AGENTS.md` in the same change when major rework alters ownership,
  layout, commands, UI/runtime invariants, or validation expectations.
