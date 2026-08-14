# Daily Classic client performance

The `Daily Classic client performance` workflow runs at 03:17 UTC each day and
can be dispatched manually from `main`. It runs the frozen player-view tests
and complete movement matrix through `tools/ci/run_linux_check.sh
client-benchmark` on the pinned Classic Linux image. This trusted monitor is
separate from the explicitly labeled
[pull-request benchmark workflow](PR_BENCHMARKS.md); required pull-request,
push, and merge-queue validation never runs timed probes.

Every attempt uploads `daily-client-performance-RUN-ATTEMPT` for 90 days. That
raw artifact contains the complete movement JSON, checkpoint images, source
SHA, repository, workflow, run, attempt, and any explicit early-failure
diagnostic. A successful benchmark that produces no movement JSON is an
infrastructure failure, not a successful empty observation.

## Public report and durable checkpoint

Successful trusted `main` observations are published at
`https://atrinik.github.io/classic/` through a custom GitHub Pages workflow.
The site is responsive, accessible, and usable without JavaScript. Its stable
surfaces are:

- `index.html`: latest status and the compact retained history;
- `trend.json` and `v1/trend.json`: byte-identical compact chart projections;
- `v1/state.json`: bounded cohort membership, watermarks, alert state,
  generation, and predecessor identity;
- `v1/manifest.json`: producer/run/source identity and SHA-256 plus size for
  every deployed file;
- `points/run-RUN.json`: one immutable detailed point per retained logical run;
- `reports/run-RUN/index.html`: one readable detailed report per point.

The normal predecessor is the last successfully deployed Pages tree. The
publisher downloads only the paths named by its manifest, rejects redirects,
validates every digest and byte count, verifies the repository/workflow
identity, and then constructs a new full tree. A benchmark, predecessor,
projection, tree, or deployment failure leaves the prior site live. No job
reads or writes a mutable generated Git ref, and no report job has
`contents: write`.

The manifest and state bind the exact source SHA, workflow run and highest
attempt, schema/environment cohort, generation, predecessor run/digest,
retention version, and every retained point/report. Rebuilding with the same
validated checkpoint, evidence, run metadata, and attempt produces byte-stable
JSON and equivalent HTML. `daily-client-performance-checkpoint-RUN-ATTEMPT`
retains the current detailed point, compact state, alerts, and manifest for 90
days as routine recovery evidence; it is not a substitute for the complete
bounded Pages history.

## Retention, reruns, and cohorts

Each compatible cohort retains at most 90 successful detailed points. The site
retains at most eight cohorts, 1,500 files, and 512 MiB. When those global
bounds remove an inactive cohort, its detailed point and report paths are
absent from the next full deployment, its watermark advances, and any active
alert receives one final recovery reconciliation. The compact trend never
embeds detailed points.

A cohort is identified by the benchmark instrumentation/schema, fixture
digests, compiler and SDL implementation identity, runner identity,
viewport/mode, and pinned runner image. Compare p50/p95/p99 and first-to-last
sustained-window values only within one cohort. An incompatible environment is
therefore visibly separate rather than silently compared with old data.

The logical observation identity is the workflow run ID. Rerunning it replaces
that point only with a higher attempt; a lower attempt fails closed. A backfill
is inserted in stable numeric run order only above every applicable retention
watermark. Once an observation has aged out, it cannot be reintroduced as if it
were new.

## Interpretation and alerts

Reports separate cold, sustained, idle, and resumed phases and show Standard,
Large, smooth, discrete, translated-lighting, and forced-full contexts. They
include p50/p95/p99 and first-to-last behavior, exact source and environment,
correctness/resource checks, alert state, and links to the raw artifact.
Lighting work is the sum of non-overlapping instrumented lighting scopes and
is not additive with overlapping parent profiler stages. Hosted-runner timing
remains informational until a separately reviewed policy change calibrates an
enforced budget; correctness, determinism, queue recovery, redraw isolation,
and resource bounds remain enforced.

Two consecutive compatible failures activate an alert; two compatible passes
recover it. The post-deployment alert job alone has `issues: write`. It
reconciles the complete desired alert state through bot-owned markers, exact
titles, and run/attempt transition markers. Creation, reopen, comment, and close
operations are idempotent across retries, and ambiguous owned issues fail
closed. Missing or corrupt evidence never mutates alerts.

When responding, verify the cohort and source SHA, then inspect sustained p95,
first-to-last behavior, redraw reasons, queue recovery, lighting/cache
counters, RSS, and correctness checks. Include the workflow run, raw artifact,
benchmark schema, and first bad SHA in follow-up work. Change budgets only with
a reviewed contract and tests.

## Activation and first bootstrap

GitHub Pages and the `github-pages` environment are manual governance state.
The environment allows only `main`, has no reviewers/secrets/variables, and the
Pages source must be GitHub Actions. The desired state and exact owner runbook
live in `atrinik/github-settings`; do not make the workflow configure its own
repository settings.

After both reviewed pull requests merge, an organization owner follows that
runbook to switch the Pages source to Actions. Then bootstrap exactly once from
the recorded final generated-data commit
`bab40ecefefa5b6052d42eab6390c504b9482e81`:

```sh
gh workflow run daily-client-performance.yml --repo atrinik/classic \
  --ref main -f checkpoint_source=final-benchmark-data
gh run list --repo atrinik/classic --workflow daily-client-performance.yml \
  --limit 1
```

The bootstrap reads that immutable commit, validates its detailed point and
trend, adds the fresh complete benchmark, and deploys the first digest-bound
tree. It never fetches the mutable `benchmark-data` ref and never pushes any
generated branch. Confirm the raw and checkpoint artifacts, all four jobs,
the provider URL, stable/versioned JSON, exact source/run/attempt, equivalent
legacy projection for the retained old point, and one fresh cohort. The final
generated-data commit remains untouched; branch deletion is separately
reviewed destructive work.

All later manual runs use the Pages predecessor (the default, shown
explicitly here):

```sh
gh workflow run daily-client-performance.yml --repo atrinik/classic \
  --ref main -f checkpoint_source=pages
```

Do not use `final-benchmark-data` again after successful activation. A second
bootstrap would intentionally start from the frozen historical checkpoint and
would fail or replace newer history rather than silently discarding it.

## Recovery and provider outage

For ordinary recovery, download and validate the live snapshot before applying
new evidence:

```sh
python3 tools/ci/daily_performance_site.py fetch \
  --base-url https://atrinik.github.io/classic/ \
  --output build/performance-checkpoint
python3 tools/ci/daily_performance_site.py validate \
  --site build/performance-checkpoint
```

Rerun a failed publication only after identifying whether its evidence,
checkpoint, projection, Pages deployment, or alert reconciliation failed. A
projection retry with the same run automatically uses a higher attempt and
replaces the logical point. If Pages is temporarily unavailable, do not
bootstrap empty state, update the generated ref, or reconcile alerts. Preserve
the last validated local/static snapshot and attempt-qualified artifacts, wait
for the provider, and redeploy by rerunning the exact failed workflow attempt
or a reviewed manual `pages` run. The last known-good deployment remains the
rollback target.

If the Pages state is missing or corrupt and no complete validated snapshot is
recoverable, stop. The frozen final generated-data commit plus retained
attempt-qualified point/state artifacts are recovery evidence, but ambiguous
or incomplete history must not be silently presented as continuous. Document
the gap and use a separately reviewed recovery plan.

## Local contract checks

The evidence projection, checkpoint builder, tree validation, and alert state
machine are deterministic and network-free in tests:

```sh
python3 -m unittest -v \
  tools.tests.test_daily_performance_report \
  tools.tests.test_daily_performance_site \
  tools.tests.test_reconcile_performance_alerts
python3 -m unittest discover -s tools/tests -p 'test_*.py'
actionlint .github/workflows/daily-client-performance.yml
git diff --check
```
