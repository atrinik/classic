# Daily Classic client performance

The `Daily Classic client performance` workflow runs at 03:17 UTC each day and
can be dispatched manually from `main`. It runs the frozen player-view tests
and the full movement matrix on the pinned Classic Linux image. The job summary
is the quickest human view; the run artifact contains the complete versioned
movement JSON and any checkpoint images.

## Trend history and cohorts

The publisher keeps generated data on the dedicated `benchmark-data` ref, not
on `main`. `trend.json` retains the latest 90 points for each compatible
cohort, and `reports/` keeps the corresponding readable reports. A cohort is
identified by the benchmark instrumentation/schema, fixture digests, compiler
and SDL implementation identity, runner identity, viewport/mode, and pinned
runner image. Compare p50/p95/p99 and first-to-last sustained-window values
only inside one cohort. A toolchain, fixture, schema, runner, or viewport change
therefore starts a separate history instead of creating a false trend.

The report separates cold, sustained, idle, and resumed phases. Sustained
telemetry includes changed/no-op MAP traffic, queue peak depth and oldest age,
lighting rebuild/reuse, sprite-cache hits/misses/evictions, memory/resource
data, redraw reasons, and correctness checks. Large-viewport phases are shown
separately when present. The 144 FPS value in the benchmark contract is an
informational display reference. Hosted-runner timing, including the existing
standard and large sustained p95 limits in
`client/tools/benchmark_movement_regression.py`, remains informational in this
reporting phase. The workflow can record a compatible regression and open an
alert after two observations, but timing alone does not fail the benchmark job.
Enabling an absolute or relative timing gate requires a separate reviewed
policy change backed by pinned-runner variance evidence. Correctness,
determinism, queue recovery, redraw isolation, and resource bounds remain
enforced.

## Rerun and backfill

Use **Run workflow** on `main` for an investigation or backfill. The workflow
is serialized with `classic-daily-client-performance`; it never runs from a
pull-request ref and never executes untrusted pull-request code in its
publishing job. The run records its source SHA, workflow URL, artifact, and
cohort. Re-running the same run identity replaces its point rather than
duplicating it, while each attempt retains an attempt-qualified raw artifact.
Both benchmark and publisher check out and verify the exact triggering `main`
commit, so a moving branch cannot mislabel the measured source.

Before publication, the reporter revalidates the closed movement-evidence
schema and recomputes its summaries and checks from the raw records. It requires
two clean, commit-matched runs for each standard/large and smooth/discrete
context. Missing contexts, contradictory status, or a mismatched implementation
revision fail as infrastructure errors rather than entering the trusted trend.
The workflow run ID is the durable observation identity, including when an
attempt crosses a UTC date boundary.

## Alerts and response

A monitored check failure is retained as a report result. An enforced
correctness or resource failure fails the benchmark; an informational timing
failure does not. A regression issue is keyed by cohort and metric and is
opened only after two consecutive compatible failures. Subsequent failures
update that one issue. Two consecutive compatible passes close the alert and
document recovery. Missing, corrupt, or incomplete evidence is an
infrastructure failure and must be investigated from the job logs/artifact; it
is never treated as a green point.
Only transitions produced by the current observation are reconciled; a cohort
change cannot replay an older cohort's alert transition.

Before entering the pinned build container, the workflow acquires and verifies
the complete immutable dependency input bundle. The benchmark itself then runs
with container networking disabled, so a missing or stale input fails closed
instead of silently changing the measured cohort.

When responding, first verify the cohort and source SHA, then compare the
sustained p95 and first-to-last window, redraw reasons, queue recovery,
lighting/cache counters, RSS, and correctness checks. Include the workflow run,
raw artifact, benchmark schema, and first bad SHA in any follow-up issue. Update
budgets only with a reviewed change to the benchmark contract and its tests;
do not tune a threshold to hide a single noisy run.

## Local contract checks

The report projection and alert state machine are deterministic and network
free:

```sh
python3 -m unittest -v tools.tests.test_daily_performance_report
python3 -m unittest discover -s tools/tests -p 'test_*.py'
actionlint .github/workflows/daily-client-performance.yml
```
