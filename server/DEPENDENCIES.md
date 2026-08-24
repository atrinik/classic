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
  The repository-level `server/tools/dependencies.py` is the authoritative
  immutable acquisition boundary used by both client and server commands. It
  safely extracts and verifies those archives below ignored runtime paths.
- `cmake/immutable_sources.lock.json` records the third-party libpcpnatpmp
  source URL and its archive/tree digests. CMake invokes the same authoritative
  fetcher for this source. Use
  `FETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP` to provide an existing local source
  tree when network fetching is unavailable.

Every lock entry records an immutable semantic-version tag, the exact
40-character commit ID behind that tag, a canonical release URL, and the
expected archive digest. A tag is a human-readable release coordinate; the
digest is the integrity boundary. Git submodules and floating branches are
intentionally not used.

Release rehearsal, candidate packaging, and recovery do not acquire these
archives from their origin URLs. `dependencies.bundle.json` binds the complete
client/server lock set plus `libpcpnatpmp` material to an exact OCI manifest
digest. The trusted default-branch publisher builds that deterministic layout
through this same fetcher. Release staging verifies the GitHub OCI attestation,
OCI digest, closed manifest and materials statement, source-lock and material
digests, sizes, and all four archive hashes before installing the raw archives
into their ordinary cache locations. Component sync selects those staged
download directories and uses
the fetcher's explicit `--refresh --offline` boundary; server CMake also uses
the staged immutable-source cache. A missing or invalid entry fails without
opening a network connection.

During the Content history cutover, the workflows may also recover unchanged
archives from the attested prior bundle
`ghcr.io/atrinik/classic-dependencies@sha256:f71e7dce5893e3fa6734e067c02738925a3b5e31c201dc202c85eaaabd720685`.
`tools/release/recover_attested_dependency_bundle.sh` verifies that image's
GitHub attestation before extraction. The staging boundary then accepts only
manifest-listed regular files whose names and SHA-256 values match the current
lock; missing or changed material still goes through the normal bounded
network acquisition path. This permits the new Content archive to be acquired
while preserving exact Resources, Sound, and libpcpnatpmp inputs whose origin
release assets are temporarily unavailable. It is not a substitute for
verification or for publishing repaired upstream releases.

The repository-root `Update verified content lock` workflow is the only
automated writer for the content record. Daily and manual runs enumerate the
complete published `atrinik/content` release history and accept only the
`classic` target derived from `main`, after verifying its tag target, canonical
assets, SHA256SUMS, bounded archive structure, schema-v2 target and source
identity, Classic compatibility, complete file digests, and license
attributions. If the current coordinate is the authenticated legacy line or
its release is no longer published, the verified lock coordinate is retained
as historical evidence and the updater allows exactly one fully verified
history cutover; it never treats a draft or unpublished matching tag as a
cutover. Once a published coordinate is locked again, every later update must
be a strict descendant of the current `main` commit. It changes only `tag`,
`commit`, `url`, and `sha256`, then re-runs this server's existing dependency
loader before proposing a pull request.

Discovery uses the read-only workflow token. Only after verification and
automation-branch ownership checks does the workflow mint the repository-only
`atrinik-classic-dependency-updater` installation token. That token may update
the stable `automation/content-update` branch and create or edit its one pull
request; the workflow never approves, merges, tags, publishes, dispatches a
release, changes settings, or writes `main`. Unexpected branch, author, pull
request, pagination, network, release, archive, or lock state fails without
mutation. A human must review and merge the generated lock pull request.

CMake stores the digest-verified, read-only pcpnatpmp extraction in
`ATRINIK_DEPENDENCY_CACHE_DIR`, which defaults to a cache beside the active
server build root. Build roots sharing that parent reuse one extraction while
retaining separate binary directories. The cache is locked during population,
published only after verification, and revalidates the complete extracted tree
against its digest-bound marker on reuse. An incomplete or mismatched cache
fails closed. MinGW builds copy and patch the immutable source inside their own
binary directory; they never modify the shared extraction, and a changed local
copy is recreated from the verified source. A cache hit always re-hashes the
archive or source tree before reuse. A cache miss downloads each attempt to a
unique partial file, removes failed partials, verifies the digest and source
tree, then atomically publishes the result. Transport resets, timeouts, and
HTTP 408, 429, and 5xx responses retry at most four times with jittered
exponential backoff (honouring a bounded `Retry-After`); policy, TLS, URL,
digest, archive-safety, and extraction failures fail immediately. Diagnostics
identify the dependency, cache state, attempt, public URL, category, and
terminal classification without retaining URL query strings.

The Check workflow stages these immutable inputs once. Its bundle key covers
the downloader and bundle schemas plus every field in the client lock, server
lock, and libpcpnatpmp source metadata. A digest-keyed Actions cache contains
raw archives only. The staging job re-hashes every restore, replaces a corrupt
or missing entry only through the bounded acquisition boundary above, and
publishes a per-run manifest with exactly the verified archives. Linux client,
server, integrated, benchmark-baseline, and Windows consumers validate that
manifest against their checkout before use. They install or extract only from
the bundle with offline mode enabled, and every compile/test container runs
with networking disabled. A missing, stale, extra, or mismatched material fails
with its dependency name before configuration begins; extracted trees and
marker files are never accepted as a cross-job cache.

For coordinated local changes, configure CMake with
`FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL` or
`FETCHCONTENT_SOURCE_DIR_LIBATRINIK`. The protocol override must remain on the
same Classic revision; the libatrinik override is a development input and must
never weaken committed lock metadata.
