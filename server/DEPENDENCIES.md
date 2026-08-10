# Dependency model

The server has a revision-coupled protocol source plus two independently
synchronized dependency sets.

- The Classic protocol comes from the sibling `protocol/` tree, the scoped
  release's embedded `dependencies/protocol` tree, or an explicit
  `FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL` override. CMake verifies that its
  generated wire version matches the server.
- `cmake/dependencies.lock.json` records the libatrinik source release. CMake
  validates every lock field and uses `FetchContent` with an exact SHA-256
  digest.
- `dependencies.lock.json` records install-time content and runtime resources.
  `tools/dependencies.py` validates, downloads, safely extracts, and verifies
  those archives below ignored runtime paths.

Every lock entry records an immutable semantic-version tag, the exact
40-character commit ID behind that tag, a canonical release URL, and the
expected archive digest. A tag is a human-readable release coordinate; the
digest is the integrity boundary. Git submodules and floating branches are
intentionally not used.

For coordinated local changes, configure CMake with
`FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL` or
`FETCHCONTENT_SOURCE_DIR_LIBATRINIK`. The protocol override must remain on the
same Classic revision; the libatrinik override is a development input and must
never weaken committed lock metadata.
