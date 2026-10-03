# Offline Brynknot walking-route export

`--content_benchmark_route=brynknot-v1` reuses the offline content-benchmark
initialization and shutdown. Run it through the workspace's managed scenario
route preparation, with disposable data and the selected Classic content
artifact. It opens no listener, starts no plugins, and invokes no account or
player-save API. It is exclusive with other offline operating modes.

The normal server loader initializes the twenty authored 24-by-24 maps
`/shattered_islands/world_X_Y`, with X=0..3 and Y=66..70. A temporary
`human_male` archetype object and its pool-owned controller supply ordinary
player collision semantics; normal object destruction releases them. The
candidate is never inserted into a map or registered as a connected player.

A bounded breadth-first search uses `object_blocked`, the collision check used
by `move_ob`. Every seam is verified through `get_map_from_coord` against the
loaded destination; filename adjacency alone cannot admit an edge. Tiles without floors, closed
doors, exits and walk-on/off callbacks are excluded because they may require an
extra action or change the expected destination. Authored NPCs remain blockers.
Spawn-point cells are reserved even before their first live tick, so an
unspawned guard cannot make its authored position look safely traversable.
The exact initial scenario position overlaps the dock captain's spawn cell:
its ordinary floor/collision checks still apply, but the route may depart
that already-occupied initial cell. The reservation prevents all later entry.
The planner rejects missing maps, non-24-by-24 dimensions, unreachable targets,
an invalid start, missing required coverage, or more than 8192 checkpoints.

The route starts at scenario dock `world_0_70 (20,8)`, visits all twelve city
chunks X=0..2/Y=67..70 and two wilderness chunks `world_0_66`, `world_1_66`,
passes reachable land within four Manhattan tiles of seven city landmarks, and
finishes exactly at `world_2_67 (7,23)`. Targets are selected in fixed order;
nearest reachable target ties use row-major tile order. Search direction ties
use the server's north-clockwise compass order.

Only after a complete route passes validation does stdout contain:

```xml
ATRINIK_WALKING_ROUTE_BEGIN
<live-movement-route version="1" timeout-ms="1800000" step-timeout-ms="10000">
  <checkpoint map="/shattered_islands/world_0_70" x="20" y="8" direction="0"/>
  <!-- Further checkpoints each describe one numeric-keypad movement. -->
</live-movement-route>
ATRINIK_WALKING_ROUTE_END
```

The real output has no XML declaration or comments. Subsequent directions are
1..9 excluding 5; 8 is north, 6 east, 2 south and 4 west. Each checkpoint names
the expected destination of exactly one normal movement command. The wrapper
extracts the bounded framed payload into an exclusive output artifact and
records its SHA-256 together with actual producer/content provenance in
companion evidence; provenance is not embedded in this strict client schema.

Static reachability is planning evidence only. The live client must fail on a
blocked movement, an unexpected map/coordinate, or a deadline; it cannot retry,
teleport, or mark traversal complete from this export alone. Dynamic NPCs,
doors or other live state may invalidate the static plan.
