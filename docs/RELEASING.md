# Unified classic releases

Classic uses one repository version, commit, tag, GitHub release, and artifact
set. The first post-consolidation release is `v6.0.0`; later versions follow
Conventional Commits through semantic-release. Breaking changes bump major,
`feat` bumps minor, and every other accepted type bumps patch.

## Historical boundary

The immutable historical sequence begins with `v5.0.19` at
`f2cdf68710d157d4fae44a0582972129e6c4db9e` and follows the classic server
release line through `v5.5.1`. `docs/history/release-tags.json` fixes those
names and targets, requires `v6.0.0` as the first new tag, and admits later
unprefixed semantic versions only on the post-consolidation first-parent main
line. `docs/history/component-release-map.json` maps the last five independent
component releases to their imported commits and unified artifact names.

Never create a release tag by hand. The immutable-tag ruleset prevents moving
or deleting `v*` tags. After an Actions outage, rerun the failed Semantic
Release workflow or dispatch that standard workflow without a version input.

## Publication flow

1. The root Check workflow validates import evidence and every module. Its
   aggregate result is `Classic validation`.
2. A successful Check run for the current `main` commit triggers Semantic
   Release. A stale successful run cannot release a newer, unvalidated commit.
3. Semantic-release creates the unprefixed tag and GitHub release notes, then
   dispatches Package Release with that exact tag.
4. Package Release revalidates the tag, main ancestry, GitHub release, and
   successful aggregate check. Independent jobs build all downloadable
   artifacts and rehearse the root-context server image.
5. The final job refuses an existing versioned image or any existing release
   asset, validates the closed artifact set, emits checksums and SPDX, creates
   GitHub artifact attestations, uploads assets without `--clobber`, then
   publishes the server image with BuildKit SBOM and provenance attestations.
   Only the highest semantic version updates `latest`.

Publication uses the root `.releaserc.json`,
`.github/workflows/release.yml`, and
`.github/workflows/package-release.yml`. Nested component workflows and
release configurations remain inert migration evidence while the first root
release is reviewed and rehearsed; they do not define another version train.

## Artifact contract

Every downloadable file has one source revision and version.

| Artifact | Purpose |
| --- | --- |
| `atrinik-classic-VERSION.tar.gz` | Complete source, governance, and provenance |
| `atrinik-classic-{client,server,editor,libatrinik,protocol}-VERSION.tar.gz` | Scoped source with root license, attributions, provenance, and `VERSION` |
| `atrinik_classic_protocol-VERSION-py3-none-any.whl` | Python bindings; distribution name `atrinik-classic-protocol` |
| `atrinik-classic-{client,server}-VERSION-windows-x86_64.zip` | Portable Windows packages built against sibling protocol and libatrinik |
| `atrinik-classic-VERSION.spdx.json` | SPDX 2.3 manifest for downloadable artifacts |
| `release-manifest.json` | Machine-readable commit, epoch, sizes, and hashes |
| `SHA256SUMS` | SHA-256 for every preceding release file |
| `ghcr.io/atrinik/classic-server:VERSION` | Root-context server image with embedded SBOM/provenance |

The editor archive contains the maintained Gridarta packaging utility. It does
not claim to contain a Gridarta JAR: Gridarta remains an operator-supplied,
separately reviewed GPL checkout. Automating that JAR requires an independently
verified immutable upstream revision and dependency contract. Native Linux
client and editor binary bundles likewise require a separately defined runtime
and packaging contract; the complete/scoped source archives and Linux server
container are the supported Linux release inputs in this pipeline.

## Rehearsal and verification

Dispatch Release Rehearsal on the candidate branch. It invokes the same source,
wheel, Windows, image, and closed-set validation jobs with version `0.0.0`,
retains the candidate assets for seven days, and has no publishing job. The
versioned image build uses `push: false`.

From the `atrinik/atrinik` wrapper root, verify the candidate source and runtime
with an isolated classic profile and state:

```sh
./atrinik profile create classic-release-review --from classic
./atrinik profile show classic-release-review
./atrinik build all --profile classic-release-review --test
./atrinik supply-chain audit --profile classic-release-review
./atrinik state add classic-release-v6
./atrinik topology show classic-release-review --json
./atrinik up --name classic-release-v6 --profile classic-release-review \
  --state classic-release-v6
./atrinik ps classic-release-v6 --json
./atrinik logs classic-release-v6 server --follow
./atrinik down classic-release-v6
```

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

Release assets and versioned images are write-once. A duplicate rehearsal must
fail before upload; the package workflow never uses `--clobber`. If publication
partially succeeds, stop rather than deleting, replacing, or retagging public
material. Record the incident, preserve logs and digests, fix the pipeline, and
publish the correction as the next semantic version. The mutable `latest`
alias is convenience only and is never a rollback source.

Before upgrading a production server, snapshot its state and retain the exact
`content@1.x` revision. Roll back by stopping the topology, restoring that
snapshot, selecting the prior immutable classic commit/image and compatible
content revision in a classic-derived wrapper profile, then running the full
supervised lifecycle above. A unified packaging version alone does not change
the wire protocol or save format; protocol, persistence, or content migrations
must document their own compatibility and rollback requirements in the change
that introduces them. Pre-consolidation releases and assets remain available
from the archived component repositories recorded in the release map.
