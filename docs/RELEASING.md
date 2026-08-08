# Unified classic releases

Classic uses one repository version, commit, tag, GitHub release, and artifact
set. The first post-consolidation release is `v5.6.0`; later versions stay on
the `v5.x.x` line and follow Conventional Commits through semantic-release. A
breaking marker or `feat` bumps minor, and every other accepted type bumps
patch.

## Historical boundary

The immutable historical sequence begins with `v5.0.19` at
`f2cdf68710d157d4fae44a0582972129e6c4db9e` and follows the classic server
release line through `v5.5.1`. `docs/history/release-tags.json` fixes those
names and targets, requires `v5.6.0` as the first new tag, and admits later
unprefixed `v5.x.x` versions only on the post-consolidation first-parent main
line. Version analysis and release notes select that same first-parent line by
full commit ID; commits reachable only through imported component parents are
provenance and never become unified release changes.
`docs/history/component-release-map.json` maps the last five independent
component releases to their imported commits and unified artifact names.

Never create a release tag by hand. The immutable-tag ruleset prevents moving
or deleting `v*` tags. Repository immutable releases are an externally governed
hard activation gate: an administrator must enable and live-audit the setting
before Semantic Release is enabled. The workflow token cannot read that
administration endpoint before publication. Its postcondition detects and stops
after GitHub fails to report the completed release as immutable, but it cannot
undo a mistakenly mutable publication; the external audit is therefore
mandatory. If Semantic Release fails before it creates a tag, rerun that
workflow. If it creates the tag but no release, dispatch
Recover Missing Release for that tag; the guarded workflow proves ancestry,
policy, checks, and release absence before it
regenerates the same first-parent notes with the pinned official formatter,
creates a draft, and queues packaging. If the draft already exists, rerun or
manually dispatch Package Release from the exact matching tag ref only when no
packaging run has produced a candidate or uploaded an asset. Otherwise use
**Re-run failed jobs** on the original Package Release run. Never delete or
recreate a tag or draft to recover.

If a release-automation defect is fixed after the tag and draft exist but
before a candidate is produced, the next successful Semantic Release run
detects the single reachable draft, dispatches Package Release from the
validated current `main` workflow definition, and skips version analysis. This
is the guarded escape hatch for a broken tag-bound workflow definition; normal
publication remains bound to the exact tag definition. Multiple drafts fail
closed for manual investigation.

If the failed run reached complete-candidate validation but a defect in its
tag-bound publication code makes a job rerun impossible, dispatch Package
Release from current `main` with both the release tag and the numeric failed
run ID. The recovery preflight requires the original run to have exactly one
successful complete-candidate finalizer, a failed publication job, and one
unexpired candidate artifact named for that tag. It skips every candidate
build, downloads that retained artifact by run ID, rejects any draft-asset
mismatch, and uses only the validated current-main verifier against the tagged
source tree. This is the sole exception to recovery within the original run.

```sh
gh workflow run package-release.yml --repo atrinik/classic --ref main \
  -f tag=v5.6.1 -f candidate_run_id=RUN_ID
```

## Publication flow

1. The root Check workflow validates import evidence and every module. Its
   aggregate result is `Classic validation`.
2. A successful Check run for the current `main` commit triggers Semantic
   Release. Pull-request, merge-group, failed, stale, and non-main Check runs
   cannot publish.
3. Semantic-release analyzes and formats only exact first-parent commits,
   creates the unprefixed tag and draft GitHub release notes, then dispatches
   Package Release from that exact immutable tag ref.
4. The non-publishing Build Release Candidate workflow revalidates the tag,
   draft, main ancestry, and successful aggregate check. GitHub exposes drafts
   only to tokens with push access, so production grants `contents: write` only
   to its metadata job; that job performs no mutations. Rehearsals remain
   read-only.
   Independent jobs build every artifact, install/import the wheel, consume the
   extracted same-version library archive, and build the root-context server
   image without publishing it.
5. Package Release rechecks all hashes, attests the candidate, and reconciles
   the draft assets: a matching partial upload is resumed, while any digest,
   size, state, name, or extra-asset mismatch fails without overwrite. It then
   publishes or verifies the same-version server image, locked-input labels,
   SLSA provenance, SPDX SBOM, and GitHub/Sigstore attestation.
6. With all twelve assets and the image complete, the workflow publishes the
   draft as its last release mutation and verifies that GitHub reports an
   immutable, non-prerelease release with the exact asset digests. A retry also
   accepts that exact published state and skips every immutable release write.
7. A separate job dispatches the globally serialized Promote Latest Release
   workflow. It recomputes GitHub's authoritative latest release, revalidates
   its closed asset set and exact versioned image, and reconciles only the
   mutable GHCR `latest` alias, verifies its resulting registry digest, and
   requires the GitHub/Sigstore attestation. GitHub assigns the release's Latest designation
   from semantic versions when the draft is published.

Publication uses the root executable `.releaserc.cjs` and its fail-closed
first-parent selector,
`.github/workflows/release.yml`, and
`.github/workflows/package-release.yml`; both production and rehearsal call the
non-publishing `.github/workflows/build-release-candidate.yml`, and
`.github/workflows/promote-latest.yml` owns only alias reconciliation. Nested component workflows and
release configurations remain inert migration evidence while the first root
release is reviewed and rehearsed; they do not define another version train.

### Initial activation

The pipeline was activated in two phases so merging its implementation could
not publish an untested first release. Semantic Release remained manual-only
until a complete post-merge rehearsal succeeded and an administrator
live-audited repository immutable releases as enabled through the governance
rollout. The separate activation change then added only the successful current
`main` Check trigger.

The first automatic analysis must produce `v5.6.0` from the already-merged
unified release work. Imported component features, breaking markers, and old
issue numbers must not affect its version or appear in its notes.

## Artifact contract

Every downloadable file has one source revision and version.

| Artifact | Purpose |
| --- | --- |
| `atrinik-classic-VERSION.tar.gz` | Complete source, governance, and provenance |
| `atrinik-classic-{client,server,editor,libatrinik,protocol}-VERSION.tar.gz` | Scoped source with root license, attributions, provenance, and `VERSION`; native consumers include matching sibling dependency source |
| `atrinik_classic_protocol-VERSION-py3-none-any.whl` | Python bindings; distribution name `atrinik-classic-protocol` |
| `atrinik-classic-{client,server}-VERSION-windows-x86_64.zip` | Portable Windows packages built against sibling protocol and libatrinik |
| `atrinik-classic-VERSION.spdx.json` | SPDX 2.3 manifest for downloadable artifacts |
| `release-manifest.json` | Machine-readable commit, epoch, sizes, hashes, and locked sound/content/resource inputs with affected artifacts |
| `SHA256SUMS` | SHA-256 for every preceding release file |
| `ghcr.io/atrinik/classic-server:VERSION` | Root-context server image with embedded SBOM/provenance |

The editor archive contains the maintained Gridarta packaging utility. It does
not claim to contain a Gridarta JAR: Gridarta remains an operator-supplied,
separately reviewed GPL checkout. Automating that JAR requires an independently
verified immutable upstream revision and dependency contract. Native Linux
client and editor binary bundles likewise require a separately defined runtime
and packaging contract; the complete/scoped source archives and Linux server
container are the supported Linux release inputs in this pipeline.

The client and server source archives include the matching protocol and
libatrinik trees under `dependencies/`; the libatrinik archive includes the
matching protocol tree. Their CMake configuration selects those packaged
sources automatically, so an exported scope never follows the replacement
repositories or depends on a separately mutable classic release.

The portable client consumes the checksum-pinned sound release. The portable
server and server image consume the checksum-pinned classic content and
resources releases. Their lock path, repository, tag, commit, URL, SHA-256,
destination, and affected artifacts are recorded in `release-manifest.json`
and the SPDX relationships; the server image repeats its applicable coordinates
as machine-readable OCI labels.

## Rehearsal and verification

Release Rehearsal invokes the same source, wheel, Windows, image, and closed-set
validation jobs with version `0.0.0`, retains the candidate assets for 30 days,
and has no publishing job or write permissions. Run it before initial
activation and after material release-pipeline changes. The versioned image
build uses `push: false`.

From the `atrinik/atrinik` wrapper root, verify the candidate source and runtime
with an isolated classic profile and state:

```sh
./atrinik profile create classic-release-review --from classic
./atrinik profile show classic-release-review
./atrinik build protocol --profile classic-release-review --test
./atrinik build libatrinik --profile classic-release-review --test
./atrinik build server --profile classic-release-review --test
./atrinik build client --profile classic-release-review --test
./atrinik supply-chain audit --profile classic-release-review
./atrinik state add classic-release-v5-6
./atrinik topology show classic-release-review --json
./atrinik up --name classic-release-v5-6 --profile classic-release-review \
  --state classic-release-v5-6
./atrinik ps classic-release-v5-6 --json
./atrinik logs classic-release-v5-6 server --follow
./atrinik down classic-release-v5-6
```

A classic editor wrapper build contract has not landed yet. Until it does,
validate the maintained packaging helper from the wrapper root with
`bash -n classic/editor/build.sh` and `shellcheck classic/editor/build.sh`;
the root Check and Release Rehearsal workflows enforce the same contract.

A display is required when the client is included. Confirm that the client can
log in, load its map, and exchange commands with the paired server before
stopping the topology. Verify downloaded files and attestations with:

```sh
sha256sum --check SHA256SUMS
gh attestation verify atrinik-classic-VERSION.tar.gz \
  --repo atrinik/classic
gh attestation verify oci://ghcr.io/atrinik/classic-server:VERSION \
  --repo atrinik/classic
```

## Failure, recovery, and rollback

Published release assets and versioned images are write-once. Before a draft is
published, Package Release may resume from matching assets and a matching image
only; it never uses `--clobber`, and every mismatch fails closed. The same run
may be retried after publication: it verifies the exact immutable state, skips
publication, and requeues alias reconciliation. Once a Package Release run has
produced its finalized candidate or uploaded any asset, use **Re-run failed
jobs** on that original run. Never use Re-run all jobs or start a fresh Package
Release candidate to reconcile a partial draft: retained candidate artifacts
are the only permitted byte set, and Windows packages are not assumed to be
reproducible. If the original publication code itself is defective, use the
guarded current-main retained-candidate recovery described above with that
exact failed run ID. A tag with no release uses Recover Missing Release. A
draft with no packaging run may start Package Release from its exact tag ref;
after that, recovery stays with the original run or its retained-candidate
continuation. Never edit an already published
immutable release for recovery; publish a correction as the next semantic
version. The mutable `latest` alias is convenience only. Its globally serialized
promoter recomputes GitHub's latest complete immutable release on every run, and
the alias is never a rollback source.

Before upgrading a production server, snapshot its state and retain the exact
`content@1.x` revision. Roll back by stopping the topology, restoring that
snapshot, selecting the prior immutable classic commit/image and compatible
content revision in a classic-derived wrapper profile, then running the full
supervised lifecycle above. A unified packaging version alone does not change
the wire protocol or save format; protocol, persistence, or content migrations
must document their own compatibility and rollback requirements in the change
that introduces them. Pre-consolidation releases and assets remain available
from the archived component repositories recorded in the release map.
