# Classic Linux Check image and compiler-cache evidence

This ledger records the pre-merge evidence for consuming the task-focused
Classic Linux image from `atrinik/devcontainer#24`. The candidate digest is a
review coordinate only. After the devcontainer pull request is released,
Classic must verify and pin the versioned release digest before this pull
request becomes ready.

## Immutable coordinates

- Classic pull request: [#98](https://github.com/atrinik/classic/pull/98)
- Classic source head: `934af663a1f0d5892026a366f12b907459f3cd50`
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

The package remains private and linked to `atrinik/devcontainer`. Its manual
Actions-access contract grants `atrinik/classic` read only and is inventoried
by [`atrinik/github-settings#59`](https://github.com/atrinik/github-settings/pull/59).

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
- `tools/tests/test_workflow_contracts.py` requires all three jobs to hash the
  shared validation driver and their component CMake configuration, mount only
  the component ccache directory, and activate both C and C++ launchers.
- Pull request, merge-group, and trusted-main scopes have distinct keys; unsafe
  material paths, missing files, symlinks, invalid digests, and unsupported
  events fail closed.
- Attempts 3 and 4 both passed core, client, server, native Windows, and the
  stable `Classic validation` aggregate. Attempt 4 restored all compiler
  outputs without preserving any CMake build tree.

## Reproduction

Use `gh` outside a sandbox with read access to the private package and
repository:

```sh
gh run view 31430896836 --repo atrinik/classic
gh run view 31430896836 --repo atrinik/classic --json jobs
gh run download 31430896836 --repo atrinik/classic --dir RUN_ARTIFACTS
gh cache list --repo atrinik/classic --ref refs/pull/98/merge \
  --json id,key,ref,sizeInBytes,createdAt,lastAccessedAt,version
gh cache list --repo atrinik/classic \
  --key classic-Linux-X64-linux-trusted-main
```

Artifact names contain the run attempt. Attempt 3 provides
`linux-{core,client,server}-*-3`; attempt 4 provides the corresponding `-4`
artifacts. Preserve their complete TSV files when recalculating this ledger.
