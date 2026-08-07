# Atrinik server repository guide

## Ownership and orientation

- This repository owns the C17 classic game server, its persistence and runtime
  contracts, and offline scenario provisioning. Authored world data and media
  remain in `content` and `resources`.
- Read `CONTRIBUTING.md`, the affected subsystem, and its tests before editing.
  Use precise component and protocol names; do not reference confidential or
  unreleased work.
- `src/server/main.c` initializes the authoritative server and main loop.
  `src/socket/server.c` dispatches incoming numeric commands, with handlers
  under `src/socket/`.
- `src/types/`, `src/server/`, and neighboring modules implement game objects,
  simulation, commands, maps, and persistence.
- `src/plugins/plugin_arena/` and `src/plugins/plugin_python/` build loadable
  modules. The Python plugin is required by normal scripted content.
- Runtime fixtures and C unit tests live under `src/tests/`.

## Change and persistence rules

- Persist spell and skill identities as stable `spell_id` and `skill_id`
  strings. Numeric `stats.sp` table positions and array order are runtime-only,
  not durable identities.
- Consider player metrics whenever adding or changing server gameplay. Review
  `doc/METRICS.md`, the registry, and authoritative event hooks to decide
  whether character/account telemetry should be added, removed, or revised.
  Persisted metric names, subject IDs, and event semantics are durable save
  contracts; document and migrate intentional changes instead of silently
  repurposing them.
- Preserve object ownership, map activation and swap invariants, lighting
  propagation, plugin boundaries, and save-file transactionality. Add tests for
  cleanup and rollback paths when changing lifecycle code.
- `install_data/` defines new-runtime defaults. Preserve initialized workspace
  state and never handcraft, replace, or delete player, account, key, or
  identity files unless the task explicitly concerns that mutable state.
- Keep `--content_benchmark` offline and development-only: it must not start
  listeners, asset serving, plugins, metaserver registration, or the console.
  Run it through an isolated workspace profile/state and supply canonical
  logical map IDs rather than collected filesystem paths.
- Keep `--provision_scenario` offline: it must not start listeners, plugins,
  metaserver registration, or an interactive console, and it must persist via
  normal password/account APIs. Use `./atrinik scenario` from the workspace.

## Dependencies, protocols, and generated files

- Protocol and libatrinik sources must come from immutable, checksum-pinned
  releases in `cmake/dependencies.lock.json`.
- Content and runtime resources must come from immutable, checksum-pinned
  releases in `dependencies.lock.json`; do not introduce Git submodules.
- Treat a packet-layout change as coordinated protocol work. Generated command
  identifiers originate in `protocol/schema/game-commands.json`; do not copy or
  renumber them locally.
- Modify Flex `.l` and CMake `.def` inputs instead of generated lexer C or
  configured headers. Update `src/cmake.txt` for compiled source additions or
  removals.

## Rendered map lighting

- `src/server/light.c` propagates source masks as spherical 3D volumes across
  horizontal and `TILED_UP`/`TILED_DOWN` links. Opaque cells stop rays after
  receiving light on their exposed face, and a floor on the upper level blocks
  a ray crossing that vertical boundary. The linked-map search depth follows
  `MAP2_MAX_DEPTH`, the maximum depth serialized to the client.
- Ambient/floor light remains in `MapSpace.light_value`; source contributions
  use `MapSpace.light_source_value` so the server can rebuild masks when an
  opaque object or floor changes. Apply map-light calculations through
  `map_get_darkness()` rather than reading either component in isolation.
- Map loading defers local source masks until all floors and blockers have
  loaded, then restores sources from already loaded neighboring levels. Keep
  this lifecycle symmetric with map unloads when changing lighting.
- Test building-lighting changes from both outside and inside. An upper floor
  may have its own lights, while an exterior facade must still receive nearby
  base-map lighting through unobstructed 3D rays when viewed outdoors.

## Multi-level map updates

- `src/socket/request.c:draw_client_map2()` serializes each physical linked map
  into its own length-delimited `CLIENT_CMD_MAP` level block. Each depth has an
  independent `MapCell` delta cache in `socket_struct.lastmap`; do not fold
  upper or lower objects into the base map's sub-layers.
- `src/server/los.c` remains the ordinary two-dimensional gameplay LOS and fog
  authority. The same base-map mask protects actors, items, effects, targeting,
  and unexplored cells across serialized depths. Structural shell and roof
  visibility are separate camera decisions in `src/socket/request.c`; do not
  turn gameplay LOS into a per-level voxel ray volume to complete graphics.
- Send objects on their authoritative layers. Walls and roofs remain on
  `LAYER_WALL`; the client uses explicit depth for projection.
- Send both copies of every object authored with `draw_double`, regardless of
  its quadrant relative to the player. The unified stacked-map painter owns
  occlusion; directional omission creates holes in tall building walls.
- Mark `DOOR` objects with `MAP2_FLAG2_DOOR` independently of the generic
  second-pass rendering bit. Cache that semantic per socket layer so a type
  change with otherwise identical face data still produces a map delta. Door
  reveal is a client camera effect and must not broaden server LOS or disclose
  building interiors.
- Upper-level visibility is camera-top-down. A solid floor, gameplay-opaque
  cell, or hidden wall-layer roof limits enclosed storeys below it to their
  structural boundary; it must not remove a middle storey's exterior wall. A
  storey covered by a higher camera surface sends only that exterior wall, not
  its hidden horizontal floor or floor mask, because those tiles would punch
  holes through the covering roof in painter order. A wall or floor below must
  not cut tiles out of a roof above. Downward visibility remains blocked by the
  player's current solid floor/opaque boundary.
- Building cutaways are derived from the stack rather than authored building
  flags. When the player's column is beneath an upper solid surface or roof,
  flood-fill that connected overhead component in the client viewport and omit
  it. Include the adjacent one-cell wall boundary because perimeter wall cells
  commonly have no floor object of their own; do not recursively follow wall
  chains into unrelated buildings. Send these camera-occluded cells with
  `MAP2_MASK_HARD_CLEAR`, not the ordinary fog-of-war clear, so cached roofs and
  walls cannot reappear grey after exit/re-entry.
- Base-map 2D LOS limits gameplay content on positive depths, but it must not
  clear their complete cells. Downgrade them to the structural floor,
  floor-mask, and wall boundary so roofs are not sliced into the silhouette of
  walls below. Vertical stack occlusion remains authoritative for whether that
  positive-depth structure is disclosed.
- Base-depth blocked cells use the normal fog-of-war clear: never-seen cells
  remain empty and previously seen contents remain cached in grayscale unless
  a visible roof needs its structural column to complete the building shell.
  The non-visual exception is base structural support elevation: send it even
  for never-seen blocked cells because the client needs it to project visible
  linked floors, walls, and roofs at the correct height. It must not disclose a
  floor face or other gameplay content.
- A visible roof is the exception to withholding all never-seen base graphics:
  send the floor, floor-mask, and wall layers in its structural column with an
  explicit fog state. This completes the building silhouette while keeping
  actors, items, effects, and interior details undisclosed.
- A connected UP/DOWN transition includes a signed depth offset so the client
  and server can shift their existing caches instead of forcing a full map
  refresh.
- Building visibility remains derived from the stack. Do not add authored
  building/balcony/overlook flags or make map serialization borrow or zoom
  magic-mirror target objects.

## Validation and release

- Follow `.clang-format`, update CMake source lists, and treat warnings and
  sanitizer findings as defects. Validate dependency locks when they change.
- Build and test through `./atrinik build server --profile PROFILE --test`.
  For runtime behavior, use the wrapper's exact
  `topology show`/`up`/`ps`/`logs`/`down` lifecycle with an isolated state and,
  when useful, a test scenario.
- For substantial native logic changes, also run the `linux-coverage` preset
  and gcovr summary documented in `README.md`; keep source/test exclusions
  intentional.
- Pull request titles and commits use Conventional Commits style. Every squash
  merge is released by semantic-release; preserve the source, Windows server
  package, checksum, and server-image release jobs together.
- Preserve unrelated work, keep generated output under `build/`, and finish
  with `git diff --check`.
- Update this `AGENTS.md` in the same change when major rework alters ownership,
  layout, commands, persistence/runtime invariants, or validation expectations.
