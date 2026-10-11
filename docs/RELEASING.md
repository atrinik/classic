# Unified classic releases

Classic uses one repository version, commit, tag, GitHub release, and artifact
set. The first post-consolidation release was `v5.6.0`; later versions stay on
the `v5.x.x` line and follow a branch-aware Conventional Commits policy through
semantic-release. On `main`, each checked batch containing release-driving
commits starts the next minor line. A numeric `X.Y.x` branch is cut from its
published `vX.Y.0` tag and accepts only patch releases (`vX.Y.1`, then later point fixes);
feature/breaking transitions fail closed instead of leaving the range.

## Automatic main publication

Successful trusted `Check` workflow runs for protected `main` pushes wake
Semantic Release. Before changing release state, it verifies the exact current
main commit, the Check workflow identity and check suite, and the successful
`Classic validation` aggregate. Pull-request, merge-group, failed, stale,
non-main, or foreign-repository Check runs cannot authorize publication.

Semantic Release, Package Release, and Recover Missing Release share the
non-cancelling `classic-release-publication` queue with
`queue: max`. GitHub retains up to 100 pending runs; operators must investigate
queue-capacity failures rather than treating a missing run as success. The queue
serializes release writers while CI validates new merges. Stale or unvalidated
heads are skipped, and the next checked current head includes all unreleased
first-parent changes since the previous semantic release. This batches merges
without a fixed quiet-period delay or a release for every intermediate commit.
Only batches containing release-driving Conventional Commits create a version.
Promote Latest Release retains its separate `classic-promote-latest` lock and
recomputes the latest complete immutable release before reconciling aliases.
The expanded pending queue applies to the new workflow definitions. Existing
immutable tags retain their historical definitions, including the former
single-pending-run behavior and original asset schema; recover them through
the guarded procedures below rather than assuming the new queue contract.

Successful Package Release completion also wakes Semantic Release to reconcile
current main. This wake-up reselects the latest head and requires its own exact
trusted successful Check binding before mutation; the package run is not source
validation. It lets an older pending draft finish recovery before a later
checked batch is analyzed, so merges made during packaging are not lost. If
current main is still being checked, its later successful Check completion
supplies the next wake-up. Failed packaging retains the guarded recovery rules
below and does not authorize a new release over an incomplete draft.

Numeric maintenance branches retain push and manual dispatch triggers and
patch-only version policy. Manual dispatch on current `main` provides an
explicit retry when automatic analysis failed before tagging; it still requires
exact trusted source validation and the shared publication queue. Maintenance
pushes and manual release runs wait for the exact push Check result for up to
30 attempts at 30-second intervals; a timeout is a failure. Once a tag or
candidate exists, follow the matching recovery procedure below instead of
creating replacement release state.

Merging the automation change arms future successful main Check completions.
Preparing or reviewing its pull request publishes no release and dispatches no
workflow. Stable source publication and the existing development-server channel
do not deploy or upgrade a running public game; runtime acceptance, activation,
compatibility, and rollback remain separate operations.

### Development server channel

`Publish Development Server` runs on main pushes and manual current-main
dispatches. Its read-only preflight binds a successful `Classic validation`
aggregate to the exact repository/main/push Check workflow run and source
commit. Forks, other branches/events, stale revisions and dirty tracked source
fail closed. The publisher uses the existing root-context server Dockerfile,
the exact attested dependency bundle, and its offline dependency/build step.
Base-image and distribution-package acquisition retain the existing build
contract. No server compilation occurs on deployment hosts.

| Coordinate | Development contract |
| --- | --- |
| Image package | Existing public `ghcr.io/atrinik/classic-server` |
| Immutable image tag | `source-FULL_40_CHARACTER_MAIN_COMMIT` |
| Discovery alias | `development`; never a runtime/rollback identity |
| CMake and OCI package version | `0.0.0`, an honest non-release source build |
| Source identity | Full main commit, also compiled into native version metadata; clean source |
| Channel label | `org.atrinik.release-channel=development` |
| Signed builder | `atrinik/classic/.github/workflows/publish-development-server.yml` |
| Signed source ref | `refs/heads/main` |
| Runtime pin | Exact index, Linux/amd64 child and config digests |

The new tags are disjoint from stable numeric tags and `latest`. Development
publication creates no Git tag, GitHub release, unified artifact bundle or
public Windows-client release. Existing stable discovery continues to select
GitHub releases and numeric image tags. Development clients use qualified
clean-source exports matching their server protocol.

A single non-cancelling workflow lock covers every development image and alias
write. An absent source tag can be built once. Before attesting this run's
new index, the publisher verifies its digest, source/channel/version/platform,
locked-input labels, BuildKit SLSA provenance and SPDX SBOM. GitHub attests the
exact built index; the publisher verifies the repository, workflow, source
commit and main ref before promotion. An existing source tag must already
pass those same checks, including its GitHub attestation; it is never rebuilt,
overwritten or retroactively attested. Missing packages, malformed API data,
authentication failures and inventory limits are failures, not permission to
recreate a package or overwrite a coordinate.

Promotion rechecks current main and verifies any existing development alias
and its source tag. A new source must descend from that previous source; the
same source must retain exactly the same digest. Only the verified index is
copied to `development`, and the resulting digest is checked. If main advances
during a build, the old immutable source image may finish publishing, but the
stale run cannot promote it after observing that advance. GitHub/registry
cannot atomically compare a branch ref and write an alias; the shared writer
lock and ancestry check prevent a delayed run from replacing a newer published
source. Consumers independently enforce accepted-source ancestry.

A failure after image push but before GitHub attestation leaves an incomplete
immutable source tag that is never eligible for promotion. A new run must not
reattest or replace those bytes. Investigate the failed run; publication of a
later reviewed main commit uses a fresh source coordinate. A fully attested
image whose promotion failed may be verified and promoted by rerunning the
workflow while that source is still current main. If main has advanced,
publish its new source instead. Never restore runtime state from a mutable
alias; retain the exact prior image and complete state backup.

The generic service updater's reviewed Classic development channel must select
`development`, verify its immutable source tag and exact signed source identity,
and pin the digest graph before activation. It orders accepted versions by
source ancestry, not by `0.0.0`; the existing durable-save, compatibility,
backup, fencing and explicit activation gates still apply. Source publication
is not runtime acceptance. Verify anonymous pulls, signed provenance, image
capabilities and actual isolated private-map/access behavior before enabling
a deployment or its update timer. Public game and metaserver changes remain
separate operations.

## Historical boundary

The retired historical sequence begins with `v5.0.19` at
`97d9960ec87313bbbf5412910ca4a71a49de8832` and follows the classic server
release line through `v5.5.1`. `docs/history/release-tags.json` records those
retired names and targets, plus the retained live tags `v5.34.4` and
`v5.46.2`. New unprefixed `v5.x.x` versions remain on the
post-consolidation first-parent main line. Version analysis and release notes
select that same first-parent line by full commit ID; commits reachable only
through imported component parents are provenance and never become unified
release changes.
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
publication remains bound to the exact tag definition. Candidate metadata in
this recovery path is validated by the exact checked current-main revision, so
an older pending tag is checked against the complete current release history
rather than running validation code and history policy frozen at that tag.
Multiple drafts fail closed for manual investigation.

The retired `v5.8.1` and `v5.10.0` coordinates are the recorded exceptions to that
retry rule. Both `v5.8.1` Package Release attempts failed in the Release-only
server build before finalizing a candidate, publishing an image, or uploading
a draft asset. `docs/history/release-tags.json` records each exact tag commit,
empty draft ID, failed run IDs, expected Windows-server and server-image job
conclusions, and
`delete-empty-draft` disposition. On current validated `main`, Semantic Release
rechecks those immutable coordinates and the failed job conclusions, then
re-lists the complete draft inventory and every page of failed-run jobs, reads
the exact release once more, deletes only that exact zero-asset draft, and
continues version analysis. Semantic Release, Package Release, and manual
recovery share one non-cancelling publication queue. Any
changed draft ID or tag target, uploaded asset, successful
candidate/publication job, unrecognized run, or additional draft fails closed.
GitHub does not provide a conditional release DELETE, so operators must not
manually mutate a policy-listed draft while Semantic Release is running; the
single guarded helper minimizes the remaining read/delete interval.
Their commit targets are evidence only; they are never published, downloadable,
eligible for Latest, or used as image aliases. The next semantic version
contains the server correction and all first-parent fixes after `v5.8.1`.

The `v5.10.0` Package Release failed while cross-compiling the Windows server:
the tagged source called POSIX-only `lstat` and `S_ISLNK` from a shared server
source file. Its server image build succeeded, but its Windows server build
failed before candidate finalization, publication, or any draft asset upload.
The policy records exact draft ID `368181077`, tag commit
`0fe7cd14e89f1a59466bfe6173d4a956da0d7143`, and failed run `31429488922`.
The commit remains unpublished historical evidence and is never used as an
image alias. The next semantic version includes the portable staging-path
correction.

The `v5.33.1` Package Release failed after version metadata moved into the
authoritative root `cmake/AtrinikVersion.cmake`: the tagged server Dockerfile
did not copy that directory into its build context. Every recorded attempt
successfully built the Windows server package, then failed the server-image
job before complete-candidate validation, image publication, or draft asset
upload. A later source fix added the missing Dockerfile copy, but recovery
continued to build the immutable tagged source and therefore could not repair
that draft. The policy records exact draft ID `370711804`, tag commit
`c470f85abc9827b90004ff6129f938ec8b321a5a`, every failed Package Release run,
and both job conclusions. The commit remains unpublished historical evidence.
Ordinary semantic analysis selects the next version containing the Dockerfile
correction and all later first-parent changes.

The `v5.34.0` Package Release failed because the root `.dockerignore` still
excluded `cmake/`, so BuildKit could not satisfy the server Dockerfile's
`COPY cmake ./cmake` instruction. The Windows server package succeeded, but
the server-image job failed before complete-candidate validation, image
publication, or draft asset upload. The policy records exact draft ID
`371272859`, tag commit `e850d809a202181069634a64494533792cff28ad`,
failed run `31936701583`, and both job conclusions. The commit remains
unpublished historical evidence. Ordinary semantic analysis selects the next
version containing the Docker context correction and all later first-parent
changes.

The `v5.80.0` Package Release run `38092568098` failed while installing the
server image's runtime provider packages. The base image retained
`util-linux=2.41.3-3ubuntu2.2`, which requires the same `libuuid1` version,
while the builder-derived provider lock required `libuuid1=2.41.3-3ubuntu2`.
The Windows server, Windows client, Debian client, and source package jobs
succeeded; complete-candidate validation and publication were skipped. The
2026-10-10 audit found nine run artifacts, no complete release candidate,
zero draft assets, and no `classic-server:5.80.0` registry manifest. The
published `5.79.0` manifest remained readable as an independent registry
access check.

The policy binds only draft `409250858`, tag source
`24bee6c8a830aa7372a30be6b842210b07639f73`, and failed run `38092568098`.
Fixing the Dockerfile on main cannot repair the immutable tagged source.
After the correction and this policy are reviewed and merged, successful main
validation permits the existing guarded Semantic Release procedure to
revalidate and remove that exact empty draft, retaining the immutable tag as
unpublished historical evidence, then select the next semantic version.
Preparing this policy performs no live draft, tag, image, or asset mutation.

This incident opts into an additional retirement guard without changing the
older recorded exceptions. On both disposition selection and the guarded
DELETE path, it re-fetches the exact failed run and attempt, verifies its source
and skipped candidate/publication jobs, rejects any complete candidate artifact,
and requires complete bounded Package Release inventories for the exact tag and
source without date filters, plus later main-lineage runs since that run's
creation, to contain exactly the listed relevant run IDs. Queued, rerun, or
additional tag/main-lineage runs stop retirement. The current registry inventory
must prove the versioned server image absent; permission errors, missing pages,
malformed metadata, or an exhausted bound fail closed. The helper rechecks current
main before returning, and the existing DELETE helper re-reads the exact empty
draft immediately before deletion. These checks run under the publication queue;
they do not make GitHub's release DELETE conditional or permit concurrent manual
mutations. Local fixtures and this historical audit do not authorize deletion.

If a failed run reached complete-candidate validation but a defect in its
publication code makes a job rerun impossible, the semantic-release guard
searches a bounded, paginated window of failed Package Release runs. It
accepts exactly one run only when the run belongs to this repository, its tag
and source commit are ancestors of current `main`, its complete-candidate
finalizer succeeded, its publication job failed only at `Publish the complete
draft release`, its image and attestation checks succeeded, and it has one
unexpired `complete-release-candidate-TAG` artifact with a SHA-256 digest. The
guard then dispatches Package Release from current `main` with the exact tag
and numeric failed run ID. Package Release skips every candidate build,
downloads that retained artifact by run ID, rejects any draft-asset mismatch,
and uses only the validated current-main verifier against the tagged source
tree. This is the sole exception to recovery within the original run; it never
rebuilds or edits a tag, release, asset, image, or attestation identity.

For the `v5.37.0` incident, run `32038894588` is not a retained publication
candidate because its publisher failed while inspecting the versioned server
image. Run `32048332566` is the candidate disposition when its exact release,
tag, artifact, image, and attestation evidence still revalidates: it failed at
the release-publication boundary after the complete candidate was finalized.
If that exact proof is no longer present, the guard fails closed with the
recovery disposition instead of guessing or mutating the draft.

```sh
gh workflow run package-release.yml --repo atrinik/classic --ref main \
  -f tag=v5.6.1 -f candidate_run_id=RUN_ID
```

## Publication flow

### Verified content lock updates

`.github/workflows/update-content.yml` checks for a newer compatible Classic
runtime derived from `content@main` on manual dispatch and contains the
reviewed daily schedule. Scheduled events remain inert until the repository variable
`DEPENDENCY_UPDATE_SCHEDULE_ENABLED` is exactly `true`; enable it only after the
first generated lock pull request, rehearsal, normal release, artifact audit,
and runtime check all succeed. Its read-only phase runs
`tools/release/update_content_lock.py`, which evaluates every newer canonical
semantic release and emits machine-readable old/new evidence only after the
release tag, checksums, archive, complete manifest, explicit `classic` target,
`main` source identity, compatibility range, consumer contract, and license
attributions all agree. Celestial-v1 manifests additionally bind the migration
index and complete file-list digest when present. If the current coordinate is
the authenticated legacy line or its release has disappeared from the complete
published history, the updater uses that historical evidence as a one-time
cross-line cutover and still requires the replacement's complete release
evidence before changing the lock. Before writing tracked inputs, both the
content and sound updaters generate the dependency bundle descriptor from the
updated locks and verified archives in a temporary staging tree. Content updates
also derive the selected GPU fixture provenance from the verified runtime
manifest, preserving the original fixture and observed-issue coordinates.
Archive acquisition or generation failure leaves the tracked inputs untouched.
Routine CI and publication acquire checksum-verified canonical archives directly;
the optional attested recovery helper is reserved for unavailable historical inputs.
Once a published coordinate is restored,
later candidates must be strict descendants of it. Draft, unpublished,
incomplete, or invalid candidates are recorded and skipped; incomplete
discovery or an invalid current coordinate stops the run.

When a verified update exists, the workflow mints the narrowly installed
GitHub App credential and creates or refreshes at most one App-owned pull
request from `automation/content-update`. It refuses an unexpected branch,
author, changed path, pull request, or concurrent ref change. The App credential
is operationally limited to that branch/PR step; although GitHub couples tag,
release, and review APIs to the same repository permission families, this
workflow contains no approval, merge, tag, release, dispatch, settings, or
default-branch mutation. Review the evidence and ordinary required checks, then
merge the lock pull request manually.

After a lock update merges, manually dispatch `Release Rehearsal` on current
`main`. Do not enable the daily schedule until that rehearsal and the next
normal Classic release both show the new content
tag, commit, URL, and digest in `release-manifest.json`, the SPDX relationships,
and the server image labels, and the released runtime starts successfully.

### Durable dependency bundle

`dependencies.bundle.json` binds the complete `sound`, `content`, `resources`,
and `libpcpnatpmp` raw-archive set to the SHA-256 digests of all three source
locks and both acquisition tools, issue #203's verified input-bundle digest,
a deterministic material digest, and the exact OCI manifest digest at
`ghcr.io/atrinik/classic-dependencies`. The material tag is derived from the
complete material digest and is immutable; release consumers use the manifest
digest, never the tag, as their trust boundary.

The embedded `materials.json` is a deterministic unsigned statement of the
closed archive set and acquisition contract; it does not claim a builder
identity. The authoritative publication provenance is the GitHub-signed OCI
attestation. Every release, rehearsal, and recovery consumer verifies that
attestation for the exact digest and repository before accepting the image.

Any reviewed lock or acquisition-contract change must rebuild the descriptor
before merge:

```sh
python3 tools/release/dependency_bundle.py build \
  --cache build/dependency-bundle-cache \
  --output build/dependency-bundle-oci \
  --write-descriptor
python3 tools/release/dependency_bundle.py show
```

The build invokes issue #203's canonical staging and closed-bundle verifier,
reuses only rehashed archives, and otherwise acquires them through the
authoritative bounded-retry fetcher. Review and commit the descriptor, not the
ignored OCI layout or archives. A push of the relevant files to current
`main`, or a manual current-`main` dispatch, runs `Publish Dependency Bundle`.
That workflow rebuilds the layout, refuses a descriptor mismatch, downloads
and verifies the exact pinned ORAS client, and publishes the layout with its
digest preserved. It has package write access only in its publication job.
Pull-request and other untrusted workflows have no publication trigger or
credential. An existing material tag at a different digest fails closed.

If a locked origin is temporarily unavailable, an operator can supply a
previously attested bundle through `--trusted-bundle`. The Python staging
boundary reuses only manifest-listed archives whose names and SHA-256 values
still match the current locks; missing or mismatched files fail closed.
`tools/release/recover_attested_dependency_bundle.sh` records the historical
Content-cutover bundle and requires its exact OCI attestation. Its historical
digest may no longer exist, so routine CI and publication do not depend on it.
Use an available, independently attested bundle for any manual recovery.

The first merge containing this contract is the bootstrap: wait for `Publish
Dependency Bundle` to publish and attest the checked descriptor before
dispatching a rehearsal. Semantic Release also waits for and fully verifies
that exact bundle before it may create a tag or draft. A missing bundle therefore
blocks versioning rather than allowing a release to race its input publication.
The initial publisher audits the material tag through GitHub's structured
package API and treats only its exact package-not-found response as an absent
repository. Ordinary pushes do not recreate a missing package. For the first
publication only, manually dispatch `Publish Dependency Bundle` from exact
current `main` with `bootstrap_missing_package` enabled; that path is bound to
the initial checked material tag and OCI digest. Once the package exists,
ordinary current-`main` runs may add new material tags but still refuse any
existing tag at a different digest. Authentication, transport,
malformed-response, and other API errors remain terminal and cannot trigger a
blind tag write. Package deletion or loss is a recovery incident: do not reuse
the bootstrap input; follow the exact-digest restoration procedure below.
Build Release Candidate verifies the GitHub OCI attestation, pulls the durable
image once, verifies the outer OCI digest, closed materials statement,
source-lock digests, archive set, sizes,
and every inner SHA-256, then passes a one-day per-run artifact to the Windows
and server-image jobs. Those consumers run dependency acquisition and build
steps with networking disabled; a missing, stale, corrupt, duplicated, or extra
input fails before compilation.

Material images and attestations are retained for as long as any supported
release or recovery path references their digest. No workflow deletes them or
promotes a mutable alias. A lock update publishes a new material-digest tag;
rollback selects the descriptor embedded in the exact prior Classic source
revision and pulls its prior OCI digest. Never repoint an existing material tag.

If publication fails while origins are available, re-run only the failed
`Publish Dependency Bundle` job or manually dispatch it on exact current
`main`; the operation is digest-idempotent. If a required digest is absent
during an origin outage, stop release or recovery. Do not substitute an
Actions cache, a newer tag, or live downloads. Restore the exact previously
exported OCI layout with its recorded digest, or wait for the locked origins
and re-run the trusted publisher. After any emergency restoration, verify the
registry digest and GitHub attestation, run Release Rehearsal, and audit the
complete `classic` supply-chain profile before resuming publication.

1. The root Check workflow validates import evidence and every module. Its
   aggregate result is `Classic validation`.
2. A successful trusted main-push Check run wakes Semantic Release. It skips
   stale heads and requires exact current-head workflow/check-suite validation
   before mutation. Successful Package Release completion supplies a second
   reconciliation wake-up for a later checked main batch. Maintenance pushes
   and supported manual dispatches retain exact branch validation.
3. Semantic-release first pulls and verifies the exact checked dependency
   bundle. It then analyzes and formats only exact first-parent commits,
   creates the unprefixed tag and draft GitHub release notes, then dispatches
   Package Release from that exact immutable tag ref.
4. Package Release stages the public Discord Application ID from the
   `discord-release` environment before invoking the reusable candidate
   workflow. Environment secrets remain scoped to that top-level release job;
   a one-day artifact carries only the validated public ID into the official
   Windows client package. The environment deployment policy admits immutable
   `v*` tag refs and `main` only for checked recovery.
5. The non-publishing Build Release Candidate workflow revalidates the tag,
   draft, main ancestry, and successful aggregate check. GitHub exposes drafts
   only to tokens with push access, so production grants `contents: write` only
   to its metadata job; that job performs no mutations. Rehearsals remain
   read-only.
   It stages the digest-pinned durable dependency bundle once. Independent jobs
   build every artifact offline from those verified archives, install/import
   the wheel, consume the extracted same-version library archive, and build the
   root-context server image without publishing it.
6. Package Release rechecks all hashes, attests the candidate, and reconciles
   the draft assets: a matching partial upload is resumed, while any digest,
   size, state, name, or extra-asset mismatch fails without overwrite. It then
   publishes or verifies the same-version server image, locked-input labels,
   SLSA provenance, SPDX SBOM, and GitHub/Sigstore attestation.
7. With all required assets and the image complete, the workflow publishes the
   draft as its last release mutation and verifies that GitHub reports an
   immutable, non-prerelease release with the exact asset digests. A retry also
   accepts that exact published state and skips every immutable release write.
   Schema 3 requires thirteen assets including the AppImage client; historical
   schema-2 recovery retains thirteen assets including the Debian client, and
   schema-1 recovery retains its original twelve-asset contract.
8. A separate job dispatches the globally serialized Promote Latest Release
   workflow after successful publication. It selects the highest published
   unified semantic version regardless of publication order, revalidates its
   immutable closed asset set and exact versioned image, and reconciles both
   GitHub's Latest designation and the mutable GHCR `latest` alias. It verifies
   the resulting registry digest and requires the GitHub/Sigstore attestation.
   The explicit successful-publication guard ensures that an intentionally
   skipped build in retained-candidate recovery cannot suppress reconciliation.
   Image policy is
   checked by the exact current-main workflow verifier against the immutable
   tagged image and the dependency locks in its tagged source checkout,
   allowing a verifier-only recovery without changing release inputs. GitHub
   may initially assign Latest by publication order when an older draft is
   recovered after a newer release; the promoter corrects that designation.

Publication uses the root executable `.releaserc.cjs` and its fail-closed
first-parent selector,
`.github/workflows/release.yml`, and
`.github/workflows/package-release.yml`; both production and rehearsal call the
non-publishing `.github/workflows/build-release-candidate.yml`, and
`.github/workflows/promote-latest.yml` owns only alias reconciliation. Nested
component workflow/release copies were retired after the root rehearsal and
unified releases proved equivalence; Git history preserves their migration
evidence.

### Activation and operational retries

The original unified pipeline was activated after a complete post-merge
rehearsal and an administrator's live audit of immutable releases. That initial
manual-only rollout and the later main publication hold are historical; current
main publication uses the automatic validation and reconciliation chain above.
The first post-consolidation version was `v5.6.0`. Imported component features,
breaking markers, and old issue numbers remain excluded from unified version
analysis and notes.

Repository immutable releases remain an externally governed prerequisite.
A live audit on 2026-10-10 for this automation change observed immutable-release
settings `enabled` and `enforced` as `true`, with no in-progress stable release
runs. Those observations describe that audit, not a permanent guarantee. Audit
fresh live settings and queued/running release writers before operational policy
changes; workflow credentials cannot substitute for the administration audit.
No settings change, workflow dispatch, tag, release, or deployment is part of
preparing the automation pull request.

Use Release Rehearsal after material pipeline changes. A failed pre-tag Semantic
Release run may be retried on current checked main. A failed Package Release run
must follow its exact draft/candidate recovery boundary. The shared queue does
not waive asset integrity, immutable release requirements, dependency-bundle
verification, or maintenance-branch version policy.

## Artifact contract

Every downloadable file has one source revision and version.

Release candidate and final packaging pass that version to every integrated,
standalone, Windows, and server-image CMake configuration through the single
`-DATRINIK_PACKAGE_VERSION=MAJOR.MINOR.PATCH` interface. Source archives retain
their generated `VERSION` file as the deterministic offline fallback.

| Artifact | Purpose |
| --- | --- |
| `atrinik-classic-VERSION.tar.gz` | Complete source, governance, and provenance |
| `atrinik-classic-{client,server,editor,libatrinik,protocol}-VERSION.tar.gz` | Scoped source with root license, attributions, provenance, and `VERSION`; native consumers include matching sibling dependency source |
| `atrinik_classic_protocol-VERSION-py3-none-any.whl` | Python bindings; distribution name `atrinik-classic-protocol` |
| `atrinik-classic-{client,server}-VERSION-windows-x86_64.zip` | Portable Windows packages built against sibling protocol and libatrinik |
| `atrinik-classic-client-VERSION-linux-x86_64.AppImage` | Linux x86_64 client with bundled application libraries; Ubuntu 24.04+ / glibc 2.39 baseline |
| `atrinik-classic-VERSION.spdx.json` | SPDX 2.3 manifest for downloadable artifacts |
| `release-manifest.json` | Machine-readable commit, epoch, sizes, hashes, and locked sound/content/resource inputs with affected artifacts |
| `SHA256SUMS` | SHA-256 for every preceding release file |
| `ghcr.io/atrinik/classic-server:VERSION` | Root-context server image with embedded SBOM/provenance |

The editor archive contains the maintained Gridarta packaging utility. It does
not claim to contain a Gridarta JAR: Gridarta remains an operator-supplied,
separately reviewed GPL checkout. Automating that JAR requires an independently
verified immutable upstream revision and dependency contract. The editor has no
Linux binary bundle.

The Linux client AppImage producer uses pinned Ubuntu 24.04 x86_64 inputs and
rebuilds the qualified native dependency versions for the glibc 2.39 baseline.
The packaging tools are linuxdeploy `1-alpha-20251107-1`, appimagetool `1.9.1`,
and the static-FUSE type-2 runtime `20251108`. Application dependencies include
SDL >= 3.4, image/ttf/mixer, libcurl built with c-ares and OpenSSL, and their
required library closure. Preserve the mixer decoder set exactly: WAV,
STBVORBIS, OPUS, VOC, AIFF, AU, DRMP3, SINEWAVE and RAW; MIDI and module
decoders remain absent. The producer retains dependency and tool identities,
licenses and checked sound/shader inputs with its packaging evidence. Retain
the source/license appendix for the bundled application libraries and the
static runtime, including its musl, FUSE and squashfuse components.

`client/tools/build-appimage.sh [OUTPUT_DIRECTORY]` requires
`ATRINIK_PACKAGE_VERSION=MAJOR.MINOR.PATCH` and emits the exact versioned
AppImage. The trusted caller validates it using
`python3 tools/release/appimage.py PATH VERSION --revision FULL_SHA
--source-root TRUSTED_CHECKOUT`. Release callers bind both the exact source
commit and its checkout; standalone inspection may omit these flags and use the
current checkout's trusted inputs. Candidate code cannot replace that caller's
validator or redefine the supported host-library boundary.
The trusted caller supplies `tools/ci/appimage/prepare.py`, its Dockerfile
and `smoke.sh`. The exact candidate checkout and staged inputs are read-only
container inputs; candidate code runs from a private copy as a non-root user,
with networking disabled, dropped capabilities and no-new-privileges. Only the
output directory is a writable host mount. The output must be the expected
regular file. Validation checks the packaged dependency closure and baseline
rather than relying on the producer's newer host libraries. The AppImage
replaces the Debian package in new candidates and remains covered by the hash manifest, SPDX and attestation
checks of the closed candidate/publication pipeline.

The client and server source archives include the matching protocol and
libatrinik trees under `dependencies/`; the libatrinik archive includes the
matching protocol tree. Their CMake configuration selects those packaged
sources automatically, so an exported scope never follows the replacement
repositories or depends on a separately mutable classic release.

The Windows and AppImage clients consume the checksum-pinned sound release.
The portable server and server image consume the checksum-pinned classic content and
resources releases. Their lock path, repository, tag, commit, URL, SHA-256,
destination, and affected artifacts are recorded in `release-manifest.json`
and the SPDX relationships; the server image repeats its applicable coordinates
as machine-readable OCI labels. The release manifest also records the exact
durable dependency-bundle image, manifest digest, material digest, inner
manifest digest, and source-lock digests used to acquire those inputs.

### Installing the Linux client

Download `atrinik-classic-client-VERSION-linux-x86_64.AppImage` from the release
and verify it against `SHA256SUMS`. Make the file executable and run it directly:

```sh
chmod +x atrinik-classic-client-VERSION-linux-x86_64.AppImage
./atrinik-classic-client-VERSION-linux-x86_64.AppImage
```

The supported baseline is Ubuntu 24.04 or newer on x86_64 with glibc >= 2.39.
Other distributions meeting that boundary need their own qualification; the
package does not promise arbitrary glibc compatibility, Alpine/musl or ARM.
SDL and application libraries are bundled, so no APT SDL installation is needed.
The host supplies the kernel, hardware GPU drivers, Vulkan loader, display
server and audio services. Production play still requires the supported
hardware SDL_GPU contract.

The pinned static-FUSE type-2 runtime needs no host libfuse2. If `/dev/fuse`
is unavailable or mounting is denied, use extraction mode:

```sh
./atrinik-classic-client-VERSION-linux-x86_64.AppImage --appimage-extract-and-run
```

Upgrade by downloading and verifying the next release, then replacing the old
AppImage. User settings and cached data remain in `.atrinik/<major>.x/`;
`ATRINIK_CONFIG_DIR` still selects an isolated configuration base. Removing the
AppImage leaves that user data in place. There is no automatic updater or store
integration.

Release-manifest schema 3 has an exact 13-file contract with the AppImage in
place of the Debian package. Historical schema 2 retains its 13 files including
`atrinik-classic-client-VERSION-linux-amd64.deb`; schema 1 retains its original
12 files. Publication and recovery select the schema of the original source
revision, validate the manifest, locked inputs and every artifact hash, and
never append or replace a platform artifact in an already published release.
Automatic main releases use the complete schema-3 contract; the checked-batch
queue, immutable publication and recovery safeguards remain unchanged.

### Linux client qualification

A candidate needs the builder, trusted structural/dependency/baseline validator
and headless runtime checks on the pinned Ubuntu 24.04, Ubuntu 26.04 and Debian
13 smoke images. Complete candidate and rehearsal validation requires all three
smoke jobs; Check's fixtures provide narrower packaging regressions. Normal
mounting and extraction-mode startup are separate paths; test extraction with `/dev/fuse` unavailable. Loader resolution,
CLI/version output, decoder availability, repeated runs, and replacement/removal
preserving isolated user data are headless checks. A configured check or a
successful `--version` command is not evidence of graphical or audible behavior.
Record actual results for the exact candidate; this contract alone does not
assert that a new AppImage has passed qualification.

On Lyra's Ubuntu 26.04 desktop, record the candidate SHA-256, host/driver identity
and an isolated `ATRINIK_CONFIG_DIR`, then complete this acceptance checklist:

- Start the downloaded AppImage normally and confirm the intended hardware GPU.
- Observe intro/UI and gameplay rendering, including normal map presentation.
- Hear music and sound effects through the normal desktop audio service.
- Connect to an authorized isolated local Classic server and enter play.
- Replace the AppImage and confirm settings remain usable; remove only the
  task-owned executable and retain user data.

Desktop acceptance requires those observations and must be reported separately
from headless evidence. It authorizes no live game deployment or upgrade. Use
the isolated wrapper topology lifecycle below for local server acceptance.

## Rehearsal and verification

Release Rehearsal invokes the same source, wheel, Windows, AppImage client
build/validation, image, and closed-set validation jobs with version `0.0.0`,
retains the candidate assets for 30 days, pulls the exact durable dependency
bundle before its build fan-out, and has no
publishing job or write permissions. Run it before initial
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
gh attestation verify \
  oci://ghcr.io/atrinik/classic-dependencies@BUNDLE_DIGEST \
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
version. The recorded pre-candidate exceptions above may each delete only
their exact empty draft so Semantic Release can advance; they do not generalize
to another tag or any draft containing an asset. The mutable `latest` alias is
convenience only. Its globally serialized
promoter recomputes GitHub's latest complete immutable release on every run, and
the alias is never a rollback source.

The pending-release guard classifies failed Package Release boundaries as
`server-image-build`, `server-image-inspection`, `windows-server-build`,
`release-publication`, or `candidate-validation`. An assetless ordinary draft
may receive at most two automatic retries after consecutive
`server-image-build` failures; the third matching failure returns a successful
`blocked` disposition with the class and count in the workflow summary, so
successful main validation does not waste another release run. Other classes
are surfaced in the summary and continue through their existing exact
disposition; tag-bound runs and current-main recovery runs count only when their
commits are on the draft tag's lineage to current `main`, and only the server-
image build class receives this automatic retry cap. Operators must inspect the
recorded failed job boundary before changing any retry policy.

Before upgrading a production server, snapshot its state and retain the exact
prior `content@main` Classic artifact coordinate. Roll back by stopping the
topology, restoring that snapshot, selecting the prior immutable Classic
commit/image and compatible content artifact in a classic-derived wrapper
profile, then running the full supervised lifecycle above. A unified packaging version alone does not change
the wire protocol or save format; protocol, persistence, or content migrations
must document their own compatibility and rollback requirements in the change
that introduces them. Pre-consolidation releases and assets remain available
from the archived component repositories recorded in the release map.
