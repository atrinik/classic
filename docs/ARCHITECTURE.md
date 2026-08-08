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

Nested component workflows and release metadata are retained for historical
audit but are inert. Root GitHub workflows are authoritative. Future classic
releases use one repository-wide version and commit; component-prefixed imported
tags remain immutable history and never drive the unified release calculation.
