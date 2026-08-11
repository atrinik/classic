# Classic Linux Check image and compiler-cache evidence

This ledger records the candidate and released-image evidence for consuming the
task-focused Classic Linux image from `atrinik/devcontainer#24`. Candidate
coordinates below remain historical review evidence; Check consumes only the
verified semantic-release image.

## Immutable coordinates

- Classic pull request: [#98](https://github.com/atrinik/classic/pull/98)
- Released image tag: `1.2.3`
- Released index digest:
  `sha256:d0ec0a31f97fa1d699f62b81bbe697d95b335f44f1c99fde8704dfc528e2102f`
- Released amd64 manifest:
  `sha256:3c6fb2cefc907776cc4ea2f544d2d0756321b2b83bd44da3fe25b29cec837f0e`
- Devcontainer release:
  [`v1.2.3`](https://github.com/atrinik/devcontainer/releases/tag/v1.2.3)
- Publisher release commit: `cfd1afd4088f76f6cd327159b0d58b20a6a6b0dd`
- Release publication:
  [run 31440069022](https://github.com/atrinik/devcontainer/actions/runs/31440069022)
- OCI source label: `https://github.com/atrinik/devcontainer`
- OCI revision label: `cfd1afd4088f76f6cd327159b0d58b20a6a6b0dd`
- Embedded inventory SHA-256:
  `cbeb59d410f138631e1b997d68df98c24c881d13f2a99fea3cd64711185b82c0`
- Embedded consumer validation: `atrinik/classic` at
  `2d3ecad2117733b1262f5195c0dd414fef4b45f3`
- Candidate consumer source head:
  `934af663a1f0d5892026a366f12b907459f3cd50`
- Pull-request merge ref commit used by Check:
  `2f90965bc25d41edc3ad46561836042185b43a5c`
- Publisher pull request:
  [`atrinik/devcontainer#28`](https://github.com/atrinik/devcontainer/pull/28)
- Publisher source: `b9a86c4f52205c927373caa7583d3a43989cfca7`
- Candidate publication:
  [run 31427897129](https://github.com/atrinik/devcontainer/actions/runs/31427897129)
- Candidate index digest:
  `sha256:e117b858d5aecdb8eb39dc56451378b6e6bd72dd5e042ab96fee5b6154000043`
- Candidate amd64 manifest:
  `sha256:cdba4bfd40f288e577842b3308e88ccfb7252623ac7b13d08074fc45cc305b8e`
- Candidate tag:
  `candidate-sha-b9a86c4f52205c927373caa7583d3a43989cfca7`
- Runner: GitHub-hosted `ubuntu24`/X64 with 4 logical CPUs, image
  `20260720.247.2`, runner `2.336.0`, kernel
  `Linux 6.17.0-1020-azure x86_64 GNU/Linux`, and Docker client/server
  `28.0.4`/`28.0.4`. The allocation, kernel, and Docker versions were captured
  by [final evidence run 31432805795](https://github.com/atrinik/classic/actions/runs/31432805795)
  on the same runner image.
- Baseline:
  [successful apt-based run 31405779774](https://github.com/atrinik/classic/actions/runs/31405779774)
- Candidate cold and warm evidence:
  [run 31430896836](https://github.com/atrinik/classic/actions/runs/31430896836),
  attempts 3 and 4
- Released-image cold and restored-cache evidence:
  [run 31443750204](https://github.com/atrinik/classic/actions/runs/31443750204),
  attempts 1 and 2 at Classic head
  `1d0e802e2d154052f08ed758b69633c3e9262706`

The package is public and remains linked to `atrinik/devcontainer`. Anonymous
manifest inspection resolved the released tag to the index digest above. The
obsolete private Actions grants were removed by
[`atrinik/github-settings#62`](https://github.com/atrinik/github-settings/pull/62),
so pull-request workflows neither request package permission nor authenticate
to GHCR to consume `classic-build` or `windows-build`.

## Released-image hosted validation

The implementation-head Check run above passed core, client, server,
integrated-graph, native Windows, security, coverage, and the stable
`Classic validation`
aggregate on both attempts. Attempt 1 populated the four isolated compiler
caches; attempt 2 ran on fresh hosted runners and restored every cacheable
compilation with zero misses:

| Component | Direct hits | Misses | Uncacheable PCH calls |
| --- | ---: | ---: | ---: |
| Core | 87 | 0 | 0 |
| Client | 352 | 0 | 238 |
| Server | 504 | 0 | 494 |
| Integrated graph | 50 | 0 | 366 |

The raw attempt-2 TSV artifacts are named
`linux-{core,client,server,integrated}-*-2`. Precompiled-header calls are
reported separately rather than counted as cache failures. Hosted job times
were 93/96 seconds for core, 94/61 for client, 190/182 for server, and 56/51
for the integrated graph across attempts 1/2. These are individual samples;
the raw zero-miss results, not elapsed-time variation, prove restoration.

The released-image measurement artifact recorded 401,281,809 compressed bytes,
1,136,279,102 Docker content bytes, a 19,208 ms first pull, a 178 ms immediate
repeat pull, a 265 ms cold start, and warm starts of 191, 203, 254, 199, and
206 ms on the same hosted runner image described above.

## Registry image measurements

Each run removed the exact local image reference before measuring its first
pull, immediately repeated the pull with local layers present, then measured
one cold and five warm `docker run --rm IMAGE true` samples. Compressed size is
the sum of the amd64 manifest's registry-layer sizes. Docker content size is
reported separately and is not compared with the compressed transfer size.

| Measurement | Cold-cache attempt 3 | Restored-cache attempt 4 |
| --- | ---: | ---: |
| Compressed amd64 layers | 401,282,166 B | 401,282,166 B |
| Docker content size | 1,136,279,102 B | 1,136,279,102 B |
| First pull | 18,226 ms | 21,643 ms |
| Immediate repeat pull | 210 ms | 176 ms |
| Cold startup | 269 ms | 272 ms |
| Warm startup samples | 184, 173, 178, 171, 187 ms | 180, 193, 186, 189, 197 ms |
| Warm startup mean | 178.6 ms | 189.0 ms |

The first-pull difference is normal hosted-network variation. Both immediate
repeat pulls prove the local-layer case and both preserve all raw startup
samples rather than only an aggregate.

## End-to-end hosted job timing

Times below are the GitHub Actions job `started_at` to `completed_at`
differences. The baseline and both candidate attempts used the same hosted
runner image version. The cold attempt started with no matching Actions cache;
the unchanged warm attempt restored the three entries created by it.

| Job | Apt-based baseline | Candidate cold | Candidate warm | Warm vs baseline |
| --- | ---: | ---: | ---: | ---: |
| Core validation | 206 s | 116 s | 84 s | -122 s (-59.2%) |
| Client validation | 84 s | 67 s | 40 s | -44 s (-52.4%) |
| Server validation | 202 s | 235 s | 141 s | -61 s (-30.2%) |

The cold server result pays a 33-second first image pull and a fully cold
compiler cache, so it is 33 seconds slower than the apt-based sample. The warm
server run reduces its build/test step from 181 to 100 seconds and its whole
job by 94 seconds. Client build/test falls from 19 to 9 seconds between the
candidate attempts. These are individual hosted samples, not guarantees.

For reference, exact baseline step timestamps report 28 seconds for core
dependency installation, 67 seconds for the complete client validation step,
and 185 seconds for the complete server validation step. The issue's original
log-level sampling further attributes roughly 50 seconds of each client/server
path to repeated dependency installation.

## Raw ccache results

The workflow zeroes statistics immediately before each component workload and
uploads the complete machine-readable `ccache --print-stats` output. Cacheable
calls equal direct hits, preprocessed hits, and misses.

| Component | Cold direct hits | Cold misses | Warm direct hits | Warm misses |
| --- | ---: | ---: | ---: | ---: |
| Core | 0 | 87 | 87 | 0 |
| Client | 0 | 193 | 193 | 0 |
| Server | 10 | 986 | 996 | 0 |

The ten server hits in the cold attempt arise inside the same job as repeated
configurations compile common inputs. The unchanged new-runner attempt restored
every output: 1,276 direct hits across all three components and zero misses.

GitHub recorded exactly three keys under `refs/pull/98/merge`, each prefixed
with `classic-Linux-X64-linux-pr-98-` and separated by component. A lookup for
the `classic-Linux-X64-linux-trusted-main` prefix returned no entries, proving
that this pull request did not write the trusted-main namespace.

## Invalidation and validation evidence

- `tools/tests/test_linux_cache_key.py` changes every hashed material, compiler
  identity, image digest, component, and manual epoch and requires a different
  restore prefix.
- `tools/tests/test_workflow_contracts.py` requires the core, client, server,
  and integrated-graph jobs to hash the shared validation driver and their
  component CMake configuration, mount only the component ccache directory,
  and activate both C and C++ launchers.
- Pull request, merge-group, and trusted-main scopes have distinct keys; unsafe
  material paths, missing files, symlinks, invalid digests, and unsupported
  events fail closed.
- Attempts 3 and 4 both passed core, client, server, native Windows, and the
  stable `Classic validation` aggregate. Attempt 4 restored all compiler
  outputs without preserving any CMake build tree.

## Reproduction

Use `gh` outside a sandbox with read access to the repository. The image itself
is public and can be inspected with an empty Docker credential directory:

```sh
gh run view 31430896836 --repo atrinik/classic
gh run view 31430896836 --repo atrinik/classic --json jobs
gh run download 31430896836 --repo atrinik/classic --dir RUN_ARTIFACTS
gh run view 31443750204 --repo atrinik/classic
gh run download 31443750204 --repo atrinik/classic --dir RELEASED_RUN_ARTIFACTS
gh cache list --repo atrinik/classic --ref refs/pull/98/merge \
  --json id,key,ref,sizeInBytes,createdAt,lastAccessedAt,version
gh cache list --repo atrinik/classic \
  --key classic-Linux-X64-linux-trusted-main
DOCKER_CONFIG="$(mktemp -d)" docker buildx imagetools inspect \
  ghcr.io/atrinik/classic-build:1.2.3
```

Artifact names contain the run attempt. Attempt 3 provides
`linux-{core,client,server}-*-3`; attempt 4 provides the corresponding `-4`
artifacts. Preserve their complete TSV files when recalculating this ledger.
