# Dependency boundaries

The client consumes one revision-coupled source and two released inputs:

- The Classic protocol comes from the sibling `protocol/` tree, the scoped
  release's embedded `dependencies/protocol` tree, or an explicit
  `FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL` override. CMake verifies that its
  generated wire version matches the client.
- `cmake/dependencies.lock.json` pins the `libatrinik` source archive by release
  tag, commit, URL, and SHA-256.
- `dependencies.lock.json` pins the sound archive with the same metadata.
- `tools/dependencies.py` delegates to the repository's authoritative
  immutable fetcher, which installs sound into the ignored `sound/` runtime
  directory and refuses unmanaged destinations or unsafe archive members.
  It retries only transient transport failures (up to four attempts, with
  jittered exponential backoff and bounded `Retry-After`) and atomically
  publishes only SHA-256-verified archives.

Check acquires the client and server material set in one staging job. Consumer
jobs verify the staged raw-archive manifest against all current locks, install
sound from that bundle in offline mode, and run their build/test containers
without networking. The comparison worktree uses the current authoritative
fetcher against its own lock and the same staged bundle; a missing or mismatched
baseline archive fails before configuration instead of contacting an external
host. Extracted media directories and their markers are not shared between
jobs.

CMake's standard `FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL` and
`FETCHCONTENT_SOURCE_DIR_LIBATRINIK` overrides are the supported local
development seam. Integrated builds use sibling sources; scoped release builds
select their embedded protocol and libatrinik trees without network access.
