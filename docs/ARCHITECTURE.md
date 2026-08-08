# Classic monorepo architecture

The repository is one physical Git checkout with five logical modules. Module
boundaries remain explicit because they encode runtime ownership, focused
tests, and reusable APIs.

```text
protocol ──> libatrinik ──> client
    │              └──────> server
    ├─────────────────────> client
    └─────────────────────> server

content@1.x + resources ──> server
sound ────────────────────> client
Gridarta checkout ────────> editor packaging
```

The arrows are dependency direction. Protocol identifiers originate only in
`protocol/`. Reusable C runtime mechanisms belong in `libatrinik/`; gameplay
and presentation policy remain in server and client respectively. Editor is a
packaging boundary for external Gridarta rather than a copy of its source.

The `atrinik/atrinik` wrapper owns multi-repository composition: physical
checkout initialization, classic worktrees, profiles, content/resource
collection, builds, mutable state, deterministic scenarios, and supervised
topologies. This repository must not reconstruct those paths or vendor the
external inputs.

Root formatting, ignore, contribution, GitHub, dependency-update, and future
release metadata are authoritative for the whole monorepo. Nested component
workflows and semantic-release configurations are inert migration inputs: they
remain in the current tree until root automation faithfully restores their
build, packaging, automatic tagging, publication, and dependency-update
capabilities, then they are retired atomically. Module-specific build presets,
locks, documentation, agent guidance, release packaging tools, provenance, and
attribution remain with their owning modules. Future classic releases use one
repository-wide version and commit; only the unprefixed unified tag sequence
drives release calculation.
