# Dependency model

The server has four dependency classes:

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
- `cmake/pcpnatpmp.cmake` pins the third-party libpcpnatpmp source URL and
  SHA-256 digest directly at its single build integration point. Use
  `FETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP` to provide an existing local source
  tree when network fetching is unavailable.

Every lock entry records an immutable semantic-version tag, the exact
40-character commit ID behind that tag, a canonical release URL, and the
expected archive digest. A tag is a human-readable release coordinate; the
digest is the integrity boundary. Git submodules and floating branches are
intentionally not used.

CMake stores the digest-verified, read-only pcpnatpmp extraction in
`ATRINIK_DEPENDENCY_CACHE_DIR`, which defaults to a cache beside the active
server build root. Build roots sharing that parent reuse one extraction while
retaining separate binary directories. The cache is locked during population,
published only after verification, and revalidates the complete extracted tree
against its digest-bound marker on reuse. An incomplete or mismatched cache
fails closed. MinGW builds copy and patch the immutable source inside their own
binary directory; they never modify the shared extraction, and a changed local
copy is recreated from the verified source.

For coordinated local changes, configure CMake with
`FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL` or
`FETCHCONTENT_SOURCE_DIR_LIBATRINIK`. The protocol override must remain on the
same Classic revision; the libatrinik override is a development input and must
never weaken committed lock metadata.
