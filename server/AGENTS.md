# Atrinik server repository guide

- Do not describe any Atrinik component as "legacy".
- Do not add references to confidential or unreleased Atrinik projects.
- Preserve unrelated work and keep generated output under `build/`.
- Protocol and libatrinik sources must come from immutable, checksum-pinned
  releases in `cmake/dependencies.lock.json`.
- Content and runtime resources must come from immutable, checksum-pinned
  releases in `dependencies.lock.json`; do not introduce Git submodules.
- Pull request titles and commits use Conventional Commits style.
- For C changes, build and run the relevant CTest suites. For dependency or
  runtime changes, also run the Python dependency tests and lock verification.
