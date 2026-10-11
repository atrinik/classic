# Requested pull-request benchmarks

Required pull-request validation builds and tests Classic automatically, but
does not run timed performance probes. Maintainers request those probes by
adding the exact `ci: benchmark` label to an open pull request.

The `Requested PR benchmarks` workflow handles `opened`, `reopened`,
`synchronize`, `labeled`, and `unlabeled` activity. Its behavior is
deterministic:

- without the label it runs only request/path classification and records that
  benchmarks were not requested;
- adding or re-applying the label starts a run for the pull request's current
  head, even when required Check has already started or completed;
- a new commit cancels an older benchmark run and starts one for the new head
  while the label remains;
- removing the label cancels an older run and prevents later commits from
  launching timed probes;
- adding or removing any other label neither cancels nor restarts an active
  benchmark request;
- after opt-in, benchmark-sensitive paths select the client comparisons, the
  server content-loader comparison, both, or a clear not-applicable summary.

The workflow checks out and verifies the exact pull-request head and compares
it with the event's base commit. Client evidence contains the existing lighting
and movement JSON, a rendered movement summary, and compiler-cache statistics.
Server evidence contains verbose isolated base/head content-loader logs and a
short completion summary. Each selected suite uploads a head-qualified artifact
retained for 14 days. Same-repository pull requests receive one bounded updated
movement-summary comment; fork pull requests retain read-only workflow
permissions and publish evidence only through the run summary and artifact.

The candidate uses the same immutable dependency bundle and pinned LLVM Linux
image as required validation. The server comparison stages a clean detached base
and authenticates its committed benchmark producer contract. A baseline with the
approved original workflow and runner uses their frozen Classic build 1.16.0
image and GCC 15.2.0; a current LLVM baseline uses its own source toolchain in
Classic build 1.18.0. Unknown or modified contracts fail before container launch.
The host launches each probe in its own digest-pinned image with networking
disabled, separate compiler caches and build trees, and runs only the required
`server-content-benchmark` test. Evidence records revision, image, compiler and
lock digests for both completed probes; a missing or failed probe produces no
success summary. Historical source commits are selected by producer contract,
not by a fixed commit whitelist. Release recovery approval is separate.

The client retains its explicit GPU-qualification skip when the current source
has no qualified player-view benchmark. Applying the label never changes the
required `Classic validation` aggregate, and an absent, removed, or not-applicable
request cannot leave that aggregate pending or make it fail.

Push and merge-queue events run required correctness validation without timed
probes. They do not inherit a pull-request label. Scheduled and manual full
client performance monitoring remains owned by
[`Daily Classic client performance`](DAILY_CLIENT_PERFORMANCE.md), which runs
only trusted `main` code. To rerun a PR benchmark, remove and re-apply
`ci: benchmark`; to benchmark a later commit automatically, leave the label in
place and push the commit.

The server's `server-content-benchmark` is timed work: it records startup,
archetype initialization, cold/warm map loading, swap/reload, and memory data.
It is therefore labeled `performance` and excluded from ordinary coverage,
Release, and sanitizer CTest runs. Its input-validation unit suite remains
automatic correctness coverage in required Check.

## Local contract checks

The request, selection, transition, fork-safety, and aggregate-separation
contracts are network-free:

```sh
python3 -m unittest -v tools.tests.test_classify_changes
python3 -m unittest -v tools.tests.test_workflow_contracts
python3 -m unittest -v tools.tests.test_server_benchmark
bash -n tools/ci/run_linux_check.sh
shellcheck tools/ci/run_linux_check.sh
actionlint .github/workflows/check.yml .github/workflows/pr-benchmarks.yml
```

Native benchmark builds use the same explicit Clang 23.1.3, LLD and
compiler-rt toolchain as Check and retain their existing Release optimization.
LLVM and historical GCC observations belong to distinct environment cohorts;
compare timings only within the compatible compiler and image cohort.
