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

The workflow uses the same immutable dependency bundle and pinned Linux image
as required validation, then disables container networking while building and
running pull-request code. Applying the label never changes the required
`Classic validation` aggregate, and an absent, removed, or not-applicable
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
bash -n tools/ci/run_linux_check.sh
shellcheck tools/ci/run_linux_check.sh
actionlint .github/workflows/check.yml .github/workflows/pr-benchmarks.yml
```
