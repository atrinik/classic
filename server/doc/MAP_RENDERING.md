# Rendered map lighting and multi-level MAP2

This design contract applies when changing classic server lighting,
`draw_client_map2()`, linked-map depth, or structural camera visibility.

## Lighting

- `src/server/light.c` propagates source masks as spherical 3D volumes across
  horizontal and `TILED_UP`/`TILED_DOWN` links. Opaque cells stop rays after
  receiving light on their exposed face, and a floor on the upper level blocks
  a ray crossing that vertical boundary. Search depth follows `MAP2_MAX_DEPTH`,
  the maximum depth serialized to the client.
- Ambient/floor light remains in `MapSpace.light_value`; source contributions
  use `MapSpace.light_source_value` so masks can rebuild when an opaque object
  or floor changes. Apply lighting through `map_get_darkness()` rather than
  reading either component alone.
- Map loading defers local source masks until floors/blockers load, then restores
  sources from loaded neighboring levels. Keep load/unload symmetric.
- Test buildings from outside and inside. Upper floors may own lights, while an
  exterior facade still receives nearby base-map lighting through unobstructed
  3D rays when viewed outdoors.

## Multi-level serialization and visibility

- `src/socket/request.c:draw_client_map2()` serializes each physical linked map
  into its own length-delimited `CLIENT_CMD_MAP` level block. Each depth owns an
  independent `MapCell` delta cache in `socket_struct.lastmap`; never fold upper
  or lower objects into base-map sublayers.
- `src/server/los.c` remains the two-dimensional gameplay LOS/fog authority.
  Its base mask protects actors, items, effects, targeting, and unexplored cells
  across depths. Structural shell/roof visibility is a camera decision in
  `request.c`; do not turn gameplay LOS into a voxel volume.
- Send objects on authoritative layers. Walls/roofs remain `LAYER_WALL`; the
  client uses explicit depth for projection. Send both halves of every
  `draw_double` object regardless of player quadrant.
- Mark doors with `MAP2_FLAG2_DOOR` independently of the generic second-pass
  bit and cache that semantic per socket layer so a type-only change emits a
  delta. Door reveal must not broaden LOS or disclose interiors.
- Upper-level visibility is camera-top-down. A solid floor, gameplay-opaque
  cell, or hidden wall-layer roof limits enclosed storeys below to their
  structural boundary without removing a middle-storey exterior wall. A
  covered storey sends only that exterior wall, not its hidden floor/floor mask;
  lower structure must not cut holes in a roof above. Downward visibility is
  blocked by the player's current solid floor/opaque boundary.
- Derive building cutaways from the stack, not authored flags. When the player
  is beneath an upper solid surface/roof, flood-fill that connected overhead
  component in the client viewport and omit it. Include the adjacent one-cell
  wall boundary without recursively following unrelated wall chains. Send
  camera-occluded cells with `MAP2_MASK_HARD_CLEAR` so cached roofs/walls cannot
  reappear grey after re-entry.
- Base-map LOS limits gameplay content on positive depths but must not clear
  their complete cells. Downgrade to structural floor, floor-mask, and wall
  boundary so lower walls do not slice roofs. Stack occlusion decides whether
  positive-depth structure is disclosed.
- Base-depth blocked cells use ordinary fog clear: never-seen cells remain
  empty and seen contents remain grayscale unless a visible roof needs its
  structural column. Send base structural support elevation even when never
  seen so linked visible structure projects correctly, but do not disclose a
  floor face or gameplay content.
- A visible roof may send base floor, floor-mask, and wall layers in its
  structural column with explicit fog state to complete the silhouette while
  withholding actors, items, effects, and interiors.
- Connected UP/DOWN transitions include signed depth offsets so client/server
  shift existing caches rather than forcing a full refresh.
- Do not add authored building/balcony/overlook flags or make serialization
  borrow/zoom magic-mirror targets.

Tests must cover lighting rebuild/load/unload, inside/outside buildings, delta
cache semantic changes, fog versus hard clear, roof silhouettes, disclosure
boundaries, cutaway connectivity, and depth-transition cache shifts.
