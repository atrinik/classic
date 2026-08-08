# Atrinik classic monorepo guide

- This repository owns the maintained classic Atrinik implementation. The new
  MIT server, client, editor, renderer, protocol, and toolkit remain in their
  separate replacement repositories.
- Logical ownership remains split across `client/`, `server/`, `editor/`,
  `libatrinik/`, and `protocol/`. Read this file and the nearest subtree
  `AGENTS.md` before editing. The imported guides deliberately retain their
  original wording; “standalone repository” there means the logical component
  boundary inside this monorepo.
- Content is not vendored here. Classic runtime content comes from
  `atrinik/content@1.x`; sound and resources remain separate repositories.
- Root `.github/` is the only active GitHub configuration. Nested component
  `.github/` directories are preserved historical inputs and are inert.
- Use `.agents/skills/classic-native-change` for C17/CMake work,
  `.agents/skills/classic-protocol-change` for wire contracts, and
  `.agents/skills/classic-runtime` for integrated execution. Cross-repository
  composition still follows the wrapper's `atrinik-multi-repo-workspace` skill;
  repository policy follows `atrinik-github-governance`.
- Keep coordinated protocol, library, client, and server changes in one
  monorepo worktree and pull request. Never copy protocol identifiers or shared
  code between subtrees.
- Prefer sibling `protocol/` and `libatrinik/` sources for integrated builds.
  Component release locks remain integrity fallbacks until deliberately
  migrated; update lock and fallback validation when their contract changes.
- All Atrinik-authored source in this monorepo, including `protocol/`, is
  GPL-2.0-or-later under the single root `LICENSE.md`. Preserve every compatible
  third-party asset/code notice, attribution, imported commit map,
  original-history branch, and the unprefixed release-tag policy.
- New classic code must be GPL-2.0-or-later compatible. Never use provenance
  grants intended for clean-room MIT replacements to infer a different license
  here.
- Operational language is “classic.” Historical `legacy-*` coordinates may
  appear only in provenance, import maps, migrated issue history, or archival
  references.
- Build and run through the thin `atrinik/atrinik` wrapper with a profile based
  on `classic`. Every runtime handoff includes the exact `topology show`, `up`,
  `ps`, `logs`, and `down` lifecycle, an isolated state, prerequisites, expected
  results, and cleanup.
- Commits and pull-request titles use Conventional Commits. Preferred scopes are
  `client`, `server`, `editor`, `libatrinik`, `protocol`, `build`, `ci`, `docs`,
  and `release`.
- Run every affected module's tests, `python3 tools/verify_import_history.py`,
  and `git diff --check` before finishing. Keep generated files in ignored build
  directories and preserve unrelated work.
