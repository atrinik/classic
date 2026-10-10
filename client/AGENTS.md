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
  revision cannot drift. libatrinik likewise requires matching sibling, embedded,
  or explicit source; a parent SDK may provide a compatible `Atrinik::Core` target.
  Historical standalone library releases are not a current API fallback.
  Update dependency selection and its configure tests together. Do not add
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
  Opaque fragments retain an exact owner and painter rank; translucent
  contributors use their own light before ordered source-over blending. Damage
  and lighting-only updates must replay transparency over the resolved opaque
  target, with opaque-rank occlusion and the existing encoded-RGBA convention.
- Keep offline player-view proofs on the normal MAP decoder and
  `map_draw_map()` path. Their closed manifests must pin every immutable input
  and renderer choice, remain bounded and network-free, and never read or
  write the normal user configuration/cache hierarchy.
- Focused text inputs own their key-down, key-up, text-input, and text-editing
  events. Do not let gameplay bindings observe an event already consumed by a
  focused widget.
- Book writing uses a separate `BOOK_EDIT` popup and bounded protocol snapshots.
  Preserve drafts on server errors and keep destination/source identity explicit.
  Cancel, Escape and X-close suspend the draft without saving or spending ink;
  same-book reopen uses a fresh session and retains draft title, text and base.
  Confirm discard for a new destination and rebase for changed persisted fields.
  Disconnect clears connection-local destination identity and drafts.
  Copy replacement and irreversible signing each require their own confirmation;
  signing submits persisted fields only. Never inherit signature metadata on copy.
  Multiline text editing is opt-in; default single-line inputs retain their behavior.
  Reset editor session identity on disconnect. Read-only books render optional
  authenticated signer/date metadata separately without interpreting its markup.
- Region-map exploration uses a hashed cache of at most 10,000 server-received
  map bitfields, scoped to the pinned connection certificate and authenticated
  account. Retain initial dimensions and OR incoming snapshots/patches. Request
  only region-definition paths, deduplicated and paced with socket backpressure.
  Bitmap allocations total at most 1 MiB; reject excess new records without
  changing the cache. Replay uses one shared 65,536-unit map/byte/set-bit budget
  per frame, resumes across frames, and coalesces packets by cache revision.
  RESET selects the account once per connection; duplicate active RESETs are
  idempotent and account switches require disconnect. Disconnect hides cached
  bits and flushes optional private disk storage. Never import character `.tiles` files or
  persist client-renderer visibility in the account cache.
- Live movement diagnostics use normal authenticated connections and movement
  commands in an isolated wrapper scenario. Their closed route owns gameplay
  input, accepts only complete server map publications, and fails on divergence
  or deadlines. Arrival and presentation are separate evidence; offline snapshot
  replays do not prove live travel or movement performance.
- Access-code popup resolution uses `access_resolver` jobs with copied secret
  storage. Closing a popup cancels without joining; the main loop reaps finished
  jobs and metaserver teardown joins all workers before endpoint/toolkit cleanup.
  Retained workers share a bounded job budget, including cancelled requests.
- Capture privacy is owned by `capture_privacy`: private UI retention and frame
  composition deny new screenshots, video frames, and diagnostic captures until
  a successfully presented clean frame. UI close/reset and renderer recovery
  must not clear this latch; already submitted safe copies may complete.
- Linux video recording owns `video_recording`, `video_encoder`, and `video_avi`.
  Capture only completed gameplay frames with bounded asynchronous readbacks;
  transport and process-isolated JPEG encoding must not block the render loop.
  Preserve the codec process's closed input grammar, private exclusive outputs,
  minimized environment, descriptor closure and bounded lifecycle. Other platforms
  must reject recording until process handle isolation is qualified. Validate
  timing, backpressure, cancellation, failure and actual AVI decoding separately.
- Client user data lives below `.atrinik/<major>.x/`. When that stable directory
  is first created, the client may migrate the highest valid same-major legacy
  directory; the migration is collision-safe and marker-backed, leaves other
  major lines untouched, and resumes after interruption or a user-file conflict.
- Intro provider switches are session-only: dismiss access-code input, cancel
  pending access resolution without waiting, clear selection and directory rows,
  and replace the complete trusted directory/rendezvous/access endpoint set.
  Directory and access workers own endpoint snapshots until completion; publish
  only current directory generations and never adopt cancelled access results.
  Only an uncovered provider-button click may bypass the active access-code
  modal; other popups and focused input retain their normal event ownership.
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
- Linux amd64 release packaging uses `tools/build-linux-package.sh`, staged
  verified sound and shader inputs, and APT-managed system libraries with SDL
  >= 3.4. Root Release Candidate builds and installation-tests the Debian package
  on pinned Debian testing inputs; retain the unified version and release contract.
- Keep generated output under `build/`, preserve unrelated work, and finish
  with `git diff --check`.
- Update this `AGENTS.md` in the same change when major rework alters ownership,
  layout, commands, UI/runtime invariants, or validation expectations.
