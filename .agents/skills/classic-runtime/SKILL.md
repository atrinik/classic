---
name: classic-runtime
description: Run or diagnose the classic client/server stack through wrapper profiles, scenarios, isolated state, and supervised topologies.
---

# Classic runtime

Run from the `atrinik/atrinik` wrapper root. The wrapper owns builds,
collection, state locks, ports, PIDs, logs, supervision, and client config; do
not reconstruct or edit its generated paths.

Use a classic-derived profile selecting the full classic worktree. Give every
concurrent topology a distinct name and state. For a deterministic account and
character, let the scenario own its dedicated `scenario-NAME` state:

```sh
./atrinik profile show PROFILE
./atrinik scenario create NAME --profile PROFILE --preset basic-player
./atrinik scenario show NAME --json
./atrinik scenario credentials NAME
./atrinik topology show PROFILE --state scenario-NAME --json
./atrinik up --name NAME --profile PROFILE --state scenario-NAME
./atrinik ps NAME --json
./atrinik logs NAME server --tail 200
./atrinik logs NAME client --tail 200
```

State display/audio prerequisites and perform the feature-specific login/actions
with an exact expected result. Diagnose wrapper status/logs before generated
paths. Never expose credentials or handcraft accounts, players, keys, or
identities.

Always stop the topology. Reset only scenario-owned state when a clean repeat
is needed; there is no generic state-removal command:

```sh
./atrinik down NAME
./atrinik scenario reset NAME
```

Report the profile, classic worktree, topology/scenario/state, automated tests,
log observations, actions/results, prerequisites, and cleanup. Process startup
alone is not feature verification.
