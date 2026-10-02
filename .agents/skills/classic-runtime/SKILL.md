---
name: classic-runtime
description: Run or diagnose the classic client/server stack through wrapper profiles, scenarios, isolated state, and supervised topologies.
---

# Classic runtime

Run from the `atrinik/atrinik` wrapper root. The wrapper owns builds,
collection, state locks, ports, PIDs, logs, supervision, and client config; do
not reconstruct its generated paths from guesses or edit state used by a live
process. If broken local metadata prevents an authorized launch and the public
inspect/retry/recovery operation cannot run, follow the wrapper's canonical
[local recovery](https://github.com/atrinik/atrinik/blob/main/docs/LOCAL_RECOVERY.md)
contract. Repair only the smallest owned coordinate after fresh path, object,
user, generation, ownership and current-use checks under exclusive coordination;
preserve the original evidence. The repair cannot rewrite a live generation,
adopt another task's state, fabricate ownership, or bypass wrapper validation.

The wrapper supplies a disposable transport-neutral `assets` staging view.
Generated `data/*`, exact-profile `client-maps/*`, and resources use
authenticated QUIC delivery by default; `http_url` only advertises an optional
operator-managed HTTP(S) origin. Never place generated assets in persistent
state or restore a bundled HTTP listener.

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
