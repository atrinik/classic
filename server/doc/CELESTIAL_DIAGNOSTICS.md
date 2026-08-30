# Classic celestial lighting diagnostics

The operator-only `/celestial` command controls one bounded, process-local
lunar override for Classic celestial-v1 maps. It is a test instrument, not a
second light-source system: the existing regional lunar evaluator, structural
transport, authoritative map field, and normal MAP2/cache path remain the only
lighting implementation.

## Commands

The command is available to `[OP]` and `[DEV]` permission groups. A custom
permission group may grant it explicitly.

```text
/celestial status
/celestial phase new|waxing-crescent|first-quarter|waxing-gibbous|full|waning-gibbous|last-quarter|waning-crescent
/celestial age HOURS
/celestial clear
```

`age` is checked against the effective lunar period of the operator's current
map (`0..L-1`, where `L` is the authored regional period). Named phases select
the exact anchor `phase * L / 8`. An explicit age is retained with its source
period and is scaled with integer floor arithmetic if a client later views a
map whose effective period differs. This keeps one global diagnostic state
deterministic across regional profiles.

`status` reports whether the override is active, the request mode/value, the
calendar-derived lunar age, the effective age and named phase, illumination,
moon orbit/elevation/visibility, moon contribution, starlight contribution,
and the process-local revision. It reports no hidden object, light-source, or
structural identity.

## Lifecycle and clock boundaries

The state is process-wide so every connected client sees the same controlled
celestial sample. Applying, replacing, or clearing it changes the celestial
cache identity, invalidates loaded keyframes, rebuilds through the existing
authoritative field, and refreshes each active client with `draw_client_map2`.
Reconnects and map changes use the current state through the ordinary map
update path. A server restart clears it; it is never written to a character,
map, world, or save file. The state is intentionally not a per-player hidden
light source.

`todtick` remains authoritative for the calendar, solar phase, season, spawns,
scripts, weather, schedules, persistence, and `/settime`. `/settime` can still
change ordinary calendar lighting while an override is active; it does not
change or clear the override. `status` exposes both the calendar-derived age
and the effective overridden age so this boundary can be checked directly.

## Isolated verification

Build and run a dedicated Classic-derived topology through the workspace
wrapper. Keep state temporary and use the exact profile/topology names reported
by the wrapper:

```text
./atrinik build classic-server --profile PROFILE --test
./atrinik up --name TOPOLOGY --profile PROFILE --temporary-state --json
./atrinik ps TOPOLOGY --json
./atrinik logs TOPOLOGY server --tail 100
```

With an authorized operator/debug client, hold `/settime` constant and run
`status`, all eight named phases, a valid age, a replacement, `clear`, and
`status` again. Repeat the same phase to verify deterministic replacement,
change maps, reconnect a second client, and compare the normal MAP2 lighting
updates. Check full, quarter, crescent, new, below-horizon, moonlit, moonless,
and starlight-only samples. Try malformed, extra-token, negative, and
out-of-range input; permission denial must occur before the handler. Confirm
that `/time`/`/settime`, saves, and restart state remain ordinary.

Stop the exact topology after the run:

```text
./atrinik down TOPOLOGY
```

Do not hand-edit server data or use a primary checkout for this procedure.
