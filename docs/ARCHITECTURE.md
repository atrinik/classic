# Classic monorepo architecture

The repository is one physical Git checkout with five logical modules. Module
boundaries remain explicit because they encode runtime ownership, focused
tests, and reusable APIs.

```text
protocol ──> libatrinik ──> client
    │              └──────> server
    ├─────────────────────> client
    └─────────────────────> server

content@main (Classic target) + resources ──> server
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

Root formatting, ignore, contribution, GitHub, dependency-update, validation,
and release metadata are authoritative for the whole monorepo. The root release
pipeline builds every module from one commit and one unprefixed version. Nested
component workflow/release copies were retired after the root rehearsal and
first unified releases proved equivalence; Git history preserves the migration
evidence. Module-specific build presets, fallback locks, documentation, agent
guidance, packaging tools, provenance, and attribution remain with their owning
modules.
