# Dependency model

The server has two independently synchronized dependency sets.

- `cmake/dependencies.lock.json` records the protocol and libatrinik source
  releases. CMake validates every lock field and uses `FetchContent` with an
  exact SHA-256 digest.
- `dependencies.lock.json` records install-time content and runtime resources.
  `tools/dependencies.py` validates, downloads, safely extracts, and verifies
  those archives below ignored runtime paths.

Every entry records an immutable semantic-version tag, the exact 40-character
commit ID behind that tag, a canonical release URL, and the expected archive
digest. A tag is a human-readable release coordinate; the digest is the
integrity boundary. Git submodules and floating branches are intentionally not
used.

For coordinated local changes, configure CMake with
`FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL` or
`FETCHCONTENT_SOURCE_DIR_LIBATRINIK`. These overrides are development inputs
and must never weaken committed lock metadata.
