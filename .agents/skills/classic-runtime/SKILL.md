---
name: classic-runtime
description: Prepare, run, diagnose, and validate the Atrinik classic client/server stack through the atrinik/atrinik workspace wrapper. Use for runtime smoke tests, deterministic scenarios, collected content or resources, mutable server state, topology supervision, client launches, or failures that only appear with the classic stack running.
---

# Classic runtime

## Use the workspace boundary

Runtime composition is owned by `atrinik/atrinik`, not this repository. Work
from that wrapper root and use a profile based on `classic`, which combines this
monorepo with `content@1.x`, resources, and sound. Do not reconstruct build,
collection, state, port, PID, log, or client-configuration paths manually.

Use a distinct profile/worktree when testing source changes. Use a distinct
topology name and state for every concurrent run. Never reuse another running
topology's mutable state or client configuration directory.

## Deterministic workflow

```sh
./atrinik topology show PROFILE --state STATE
./atrinik state add STATE
./atrinik scenario create SCENARIO --profile PROFILE --state STATE
./atrinik up --name TOPOLOGY --profile PROFILE --state STATE
./atrinik ps TOPOLOGY
./atrinik logs --tail 200 TOPOLOGY server
./atrinik logs --tail 200 TOPOLOGY client
```

Perform the feature-specific action with the provisioned account and record the
expected observable result. When a graphical client is needed, state the X11 or
Wayland and audio prerequisites. Diagnose through wrapper status and logs before
inspecting generated paths.

Always clean up the supervised processes:

```sh
./atrinik down TOPOLOGY
```

Retain a state only when the user needs it for follow-up debugging; otherwise
use the wrapper's explicit state cleanup command after the topology is down.
Never delete or rewrite accounts, players, keys, identities, or state directories
directly.

## Handoff

Report the exact profile, classic worktree, topology, state, scenario, assigned
endpoint, automated tests, log observations, manual actions, expected result,
display prerequisites, and cleanup commands. A successful process start alone
is not feature verification.
