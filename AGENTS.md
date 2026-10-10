# Atrinik classic monorepo guide

- This repository owns the maintained classic Atrinik implementation. The
  next-generation MIT server, client, editor, renderer, protocol, and toolkit
  remain in separate repositories.
- Logical ownership remains split across `client/`, `server/`, `editor/`,
  `libatrinik/`, and `protocol/`. Read this file and the nearest subtree
  `AGENTS.md` before editing. Module guides preserve their component-specific
  invariants while using monorepo ownership and release terminology.
- Content is not vendored here. Classic runtime content is the `classic`
  target derived from `atrinik/content@main`; sound and resources remain
  separate repositories.
- Root `.github/` and `.releaserc.cjs` are the only active GitHub/release
  configuration. Retired nested component copies remain recoverable from Git
  history; do not reintroduce an independent module release train.
- Use the `atrinik-development` plugin from the `atrinik` marketplace:
  `classic-native-change` for C17/CMake work, `classic-protocol-change` for
  wire contracts, and `classic-runtime` for integrated execution. The exact
  provider revision and required skills are recorded in
  [.agents/skill-provider.json](.agents/skill-provider.json). Cross-repository
  composition uses `atrinik-multi-repo-workspace`; repository policy uses
  `atrinik-github-governance` from the same plugin. Repository ownership,
  licensing and subtree instructions remain authoritative. Ordinary offline
  checks require neither an installed plugin nor network access.
- Keep coordinated protocol, library, client, and server changes in one
  monorepo worktree and pull request. Never copy protocol identifiers or shared
  code between subtrees.
- Prefer sibling `protocol/` and `libatrinik/` sources for integrated builds.
  Component release locks remain integrity fallbacks until deliberately
  migrated; update lock and fallback validation when their contract changes.
- The root CMake project is the integrated client/server graph and must add
  protocol and libatrinik exactly once. Keep component CMake entry points,
  presets, source packaging, and installed consumers independently functional.
- All Atrinik-authored source in this monorepo, including `protocol/`, is
  GPL-2.0-or-later under the single root `LICENSE.md`. Preserve every compatible
  third-party asset/code notice, attribution, imported commit map, retired-ref
  record, and the unprefixed release-tag policy. Original source graphs remain
  authoritative in the archived source repositories; do not recreate a live
  `history/*` branch namespace here.
- New classic code must be GPL-2.0-or-later compatible. That source license is
  not a blanket outbound reuse ban. Under the
  [canonical provenance policy](https://github.com/atrinik/atrinik/blob/main/docs/PROVENANCE.md),
  an MIT destination may inspect exact, independently separable material as
  source reference, copy it, migrate or port it, translate or adapt it, or
  relicense it only after a complete audit proves each selected contribution
  is the applicable named grantor's original work. Each contribution must be
  solely authored by that grantor and fall within the row's temporal scope.
  Distinct contributions may cite different rows only when each independently
  satisfies one row. Rows cannot be combined to cover jointly authored
  contributions, generated output, or inseparable mixed work. Later material
  needs contemporaneous compatible permission. The grants authorize only
  proven destination use. They neither change this repository's source license
  nor by themselves approve a GPL dependency, linked or combined binary,
  bundle, or surrounding material.
- On touch, refresh existing Atrinik-owned copyright terminal years and blanket
  holders per `CONTRIBUTING.md`; preserve precise attribution.
- Operational language is “classic.” Historical `legacy-*` coordinates may
  appear only in provenance, import maps, migrated issue history, or archival
  references.
- Build and run through the thin `atrinik/atrinik` wrapper with a profile based
  on `classic`. Every runtime handoff includes the exact `topology show`, `up`,
  `ps`, `logs`, and `down` lifecycle, an isolated state, prerequisites, expected
  results, and cleanup.
- Do not guess wrapper-generated paths or edit state used by a live process.
  If broken local metadata prevents an authorized launch and public
  inspect/retry/recovery cannot run, follow the wrapper's canonical
  [local recovery](https://github.com/atrinik/atrinik/blob/main/docs/LOCAL_RECOVERY.md)
  contract. Repair only the smallest owned coordinate after fresh path, object,
  user, generation, ownership and current-use checks under exclusive
  coordination; preserve the original evidence. Never rewrite a live
  generation, adopt another task's state, fabricate ownership, or bypass
  wrapper validation.
- Commits and pull-request titles use Conventional Commits. Preferred scopes are
  `client`, `server`, `editor`, `libatrinik`, `protocol`, `build`, `ci`, `docs`,
  and `release`.
- Root semantic-release owns one unprefixed repository version and first-parent
  release history. `main` advances each checked release-driving batch to the
  next minor line; a numeric `X.Y.x` branch is cut from `vX.Y.0` and publishes only
  later patches. Never hand-edit tags, images, drafts, or release assets; use
  the checked publication/recovery procedures in `docs/RELEASING.md`.
- Successful trusted main-push Check completions automatically wake Semantic
  Release for the exact checked current head. Package Release completion
  reconciles later checked main batches; semantic analysis, packaging, and
  recovery share the non-cancelling publication queue. Latest promotion keeps
  its separate alias lock. Each release includes the complete artifact
  contract, including the Linux Debian client. Numeric maintenance pushes and
  manual retries retain branch/check validation and guarded recovery. See
  `docs/RELEASING.md`; source publication does not authorize runtime deployment.
- Checked main source builds also publish `classic-server:source-COMMIT` and
  the `development` discovery alias through `publish-development-server.yml`,
  with version `0.0.0`, exact source identity, locked inputs, SBOM and signed
  provenance. Never substitute a source build for a semantic release.
- `tools/ci/classify_changes.py` is the single path-selection contract for
  native Check and CodeQL work. Pull requests are path-aware; protocol,
  libatrinik, and validation-contract changes select both client and server.
  Push, merge-group, schedule, and manual runs are full. Keep the stable
  `Classic validation` aggregate strict: it may accept a skipped component only
  when the successful classifier explicitly marked that component unnecessary.
- Run every affected module's tests, `python3 tools/verify_import_history.py`,
  and `git diff --check` before finishing. Keep generated files in ignored build
  directories and preserve unrelated work.
