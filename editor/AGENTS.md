# Atrinik editor packaging repository guide

- This repository is a thin packaging utility for an external Gridarta
  checkout. Gridarta source and generated JARs do not belong here.
- Keep `build.sh` strict, portable Bash. Resolve the supplied checkout and
  output directory safely, quote paths, propagate Gradle failures, and verify
  the expected upstream `AtrinikEditor.jar` before copying it.
- Preserve the dated artifact plus stable symlink contract. Generated packages
  belong under an explicit ignored output directory and must not be committed.
- Do not download, mutate, or publish an upstream checkout unless the task
  explicitly authorizes that external action.
- Validate changes with `bash -n build.sh`, `shellcheck build.sh`, a disposable
  upstream fixture or checkout, and `git diff --check`.
- Commits and pull-request titles use Conventional Commits. Every squash merge
  is released by semantic-release.
- Update this `AGENTS.md` in the same change when major rework alters packaging
  ownership, the Gridarta contract, artifact layout, or validation.
