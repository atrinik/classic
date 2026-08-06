# Atrinik server repository guide

- Use precise component names and avoid age-based labels.
- Do not add references to confidential or unreleased Atrinik projects.
- Preserve unrelated work and keep generated output under `build/`.
- Protocol and libatrinik sources must come from immutable, checksum-pinned
  releases in `cmake/dependencies.lock.json`.
- Content and runtime resources must come from immutable, checksum-pinned
  releases in `dependencies.lock.json`; do not introduce Git submodules.
- Pull request titles and commits use Conventional Commits style.
- Every squash merge is released by semantic-release; preserve the source,
  Windows server package, checksum, and server-image release jobs together.
- For C changes, build and run the relevant CTest suites. For dependency or
  runtime changes, also run the Python dependency tests and lock verification.
