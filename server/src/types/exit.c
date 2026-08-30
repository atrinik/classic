/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Handles code related to @ref EXIT "exits".
 *
 * @todo Somehow allow multi-part objects to pass through exits. The issue is
 * that usually portals lead to a space next to a return portal. This means that
 * when the multi-part object enters a portal, chances are that either its head
 * or its tail will be on top of the return portal, causing it to teleport back
 * to the portal it wanted to enter in the first place...
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <shop.h>
#include <server_main.h>
#include <movement.h>
#include <server_item.h>
#include <server.h>
#include <plugin.h>
#include <object.h>
#include <object_methods.h>
#include <arch.h>
#include <exit.h>

enum exit_cache_state {
    EXIT_CACHE_UNINITIALIZED = 0,
    EXIT_CACHE_STATIC_INVALID,
    EXIT_CACHE_STATIC_VALID,
    EXIT_CACHE_DYNAMIC,
};

static bool exit_static_destination_usable(mapstruct *destination,
                                           int x,
                                           int y,
                                           bool allow_direct,
                                           bool fixed_pos,
                                           bool do_load);
static mapstruct *exit_get_destination_uncached(object *op, int *x, int *y, bool do_load);
static void exit_cache_recompute(object *op, bool do_load);
#ifdef ATRINIK_TESTING
static uint64_t exit_cache_recompute_count;
#endif
static bool exit_find_landing_internal(object *applier,
                                       mapstruct *destination,
                                       int x,
                                       int y,
                                       bool allow_direct,
                                       bool fixed_pos,
                                       bool randomize,
                                       bool do_load,
                                       exit_landing_t *landing);

static bool exit_is_dynamic(const object *op) {
    HARD_ASSERT(op != NULL);

    if (op->type != EXIT) {
        return false;
    }

    return op->event_flags != 0 || op->last_eat == MAP_PLAYER_MAP ||
           (EXIT_PATH(op) != NULL && strncmp(EXIT_PATH(op), "/random/", 8) == 0);
}

/**
 * Find an automatically connected exit.
 *
 * @param op
 * Exit to find a connected exit for.
 * @param do_load
 * If true, will load maps if necessary.
 * @param altern_capacity
 * Number of elements available in altern.
 * @return
 * Connected exit if found, NULL otherwise.
 */
static size_t
exit_find_candidates(object *op, bool do_load, object **altern, size_t altern_capacity) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(altern != NULL);
    HARD_ASSERT(altern_capacity > 0);

    size_t nrofalt = 0;

    /* Find all other teleporters within range. This range should really
     * be settable by some object attribute instead of using hard coded
     * values. */
    for (int i = -5; i < 6; i++) {
        for (int j = -5; j < 6; j++) {
            if (i == 0 && j == 0) {
                /* Skip our own tile */
                continue;
            }

            int x = op->x + i;
            int y = op->y + j;
            mapstruct *m;
            if (do_load) {
                m = get_map_from_coord(op->map, &x, &y);
            } else {
                m = get_map_from_coord2(op->map, &x, &y);
            }

            if (m == NULL) {
                continue;
            }

            FOR_MAP_PREPARE(m, x, y, tmp) {
                if (tmp->type == op->type && tmp->sub_type == op->sub_type) {
                    /* Assumes altern has enough capacity for one element. */
                    altern[nrofalt++] = tmp;

                    /* Reached the maximum, no point in going on. */
                    if (nrofalt == altern_capacity) {
                        goto loop_exit;
                    }
                }
            }
            FOR_MAP_FINISH();
        }
    }

loop_exit:

    return nrofalt;
}

/**
 * Find an automatically connected exit, retaining the historical random
 * choice used during activation.
 */
static object *exit_find(object *op, bool do_load) {
    object *altern[20];
    size_t nrofalt = exit_find_candidates(op, do_load, altern, arraysize(altern));

    if (nrofalt == 0) {
        return NULL;
    }

    return altern[rndm(0, nrofalt - 1)];
}

/** Find the first statically enterable automatic-link peer. */
static object *exit_find_static(object *op, bool do_load, object **fallback) {
    object *altern[20];
    size_t nrofalt = exit_find_candidates(op, do_load, altern, arraysize(altern));

    if (fallback != NULL) {
        *fallback = NULL;
    }

    for (size_t i = 0; i < nrofalt; i++) {
        if (exit_is_dynamic(altern[i])) {
            continue;
        }

        if (fallback != NULL && *fallback == NULL) {
            *fallback = altern[i];
        }

        if (exit_static_destination_usable(altern[i]->map,
                                           altern[i]->x,
                                           altern[i]->y,
                                           false,
                                           false,
                                           do_load)) {
            return altern[i];
        }
    }

    return NULL;
}

/**
 * Determine whether an exit should be presented as having a usable
 * destination.
 *
 * @param op
 * Exit to inspect.
 * @return
 * True only after the server has published a static destination validation.
 * This is intentionally an O(1) cache read: MAP2 serialization must not load
 * maps, scan automatic-link peers, find landing squares, or consume RNG.
 */
bool exit_has_usable_destination(const object *op) {
    HARD_ASSERT(op != NULL);

    return op->exit_cache_entry != NULL &&
           op->exit_cache_entry->cache_state == EXIT_CACHE_STATIC_VALID;
}

/** Whether a resolved map coordinate has an authored walkable floor. */
static bool exit_has_floor(mapstruct *map, int x, int y) {
    if (map == NULL || map->spaces == NULL || OUT_OF_MAP(map, x, y)) {
        return false;
    }

    MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        object *floor = GET_MAP_SPACE_LAYER(space, LAYER_FLOOR, sub_layer);
        if (floor != NULL && QUERY_FLAG(floor, FLAG_IS_FLOOR)) {
            return true;
        }
    }

    return false;
}

/** Test and optionally publish one resolved landing position. */
static bool exit_landing_position(object *applier,
                                  archetype_t *arch,
                                  mapstruct *destination,
                                  int x,
                                  int y,
                                  int direction,
                                  bool do_load,
                                  bool map_entry_rules,
                                  exit_landing_t *landing) {
    mapstruct *map = do_load ? get_map_from_coord(destination, &x, &y)
                             : get_map_from_coord2(destination, &x, &y);
    if (map == NULL || !exit_has_floor(map, x, y)) {
        return false;
    }

    if (applier == NULL) {
        MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
        if (space->flags & (P_DOOR_CLOSED | P_PLAYER_ONLY | P_CHECK_INV)) {
            /* These conditions depend on the current actor or mutable
             * access rules and cannot be shared as a static MAP2 semantic.
             * Current player and monster occupancy is deliberately ignored;
             * activation performs the per-applier occupancy check. */
            return false;
        }
    }

    int blocked_flags;
    if (map_entry_rules) {
        /* Explicit exits historically test their direct destination with the
         * actor's occupancy rules but all terrain enabled. Adjacency uses the
         * terrain-neutral map_free_spot() contract below. */
        blocked_flags = blocked(applier, map, x, y, TERRAIN_ALL);
    } else if (do_load) {
        blocked_flags = arch_blocked(arch, applier, map, x, y);
    } else {
        /* Match arch_blocked(arch, NULL, ...): static validation ignores
         * actor-specific terrain abilities and only checks tile blockers. */
        int terrain = TERRAIN_ALL;
        blocked_flags = 0;
        for (archetype_t *tmp = arch; tmp != NULL; tmp = tmp->more) {
            int part_x = x + tmp->clone.x;
            int part_y = y + tmp->clone.y;
            mapstruct *part_map = get_map_from_coord2(map, &part_x, &part_y);
            if (part_map == NULL) {
                blocked_flags = P_OUT_OF_MAP;
                break;
            }

            MapSpace *part_space = GET_MAP_SPACE_PTR(part_map, part_x, part_y);
            if (part_space->flags & (P_DOOR_CLOSED | P_PLAYER_ONLY | P_CHECK_INV)) {
                blocked_flags = part_space->flags;
                break;
            }

            blocked_flags = blocked(NULL, part_map, part_x, part_y, terrain);
            if (blocked_flags != 0) {
                break;
            }
        }
    }

    if (blocked_flags != 0) {
        return false;
    }

    if (landing != NULL) {
        landing->map = map;
        landing->x = x;
        landing->y = y;
        landing->direction = direction;
    }

    return true;
}

bool exit_find_landing(object *applier,
                       mapstruct *destination,
                       int x,
                       int y,
                       bool allow_direct,
                       bool fixed_pos,
                       bool randomize,
                       exit_landing_t *landing) {
    return exit_find_landing_internal(applier,
                                      destination,
                                      x,
                                      y,
                                      allow_direct,
                                      fixed_pos,
                                      randomize,
                                      true,
                                      landing);
}

static bool exit_find_landing_internal(object *applier,
                                       mapstruct *destination,
                                       int x,
                                       int y,
                                       bool allow_direct,
                                       bool fixed_pos,
                                       bool randomize,
                                       bool do_load,
                                       exit_landing_t *landing) {
    HARD_ASSERT(destination != NULL);

    archetype_t *arch = applier != NULL ? applier->arch : arch_find("human_male");
    if (arch == NULL) {
        return false;
    }

    int target_x = x;
    int target_y = y;
    mapstruct *target_map = do_load ? get_map_from_coord(destination, &target_x, &target_y)
                                    : get_map_from_coord2(destination, &target_x, &target_y);
    if (target_map == NULL || !exit_has_floor(target_map, target_x, target_y)) {
        return false;
    }

    /* Explicit exits enter through object_enter_map(), whose direct check
     * uses actor occupancy with all terrain enabled and whose adjacency
     * fallback matches map_free_spot(). Automatic links use allow_direct=false
     * and retain actor-specific arch_blocked() checks. */
    bool map_entry_rules = allow_direct && applier != NULL;
    if (allow_direct && exit_landing_position(applier,
                                              arch,
                                              target_map,
                                              target_x,
                                              target_y,
                                              0,
                                              do_load,
                                              map_entry_rules,
                                              landing)) {
        return true;
    }

    if (fixed_pos) {
        return false;
    }

    exit_landing_t alternatives[SIZEOFFREE1];
    size_t alternatives_count = 0;
    object *alternative_applier = map_entry_rules ? NULL : applier;
    for (int i = 1; i <= SIZEOFFREE1; i++) {
        if (exit_landing_position(alternative_applier,
                                  arch,
                                  target_map,
                                  target_x + freearr_x[i],
                                  target_y + freearr_y[i],
                                  freedir[i],
                                  do_load,
                                  false,
                                  &alternatives[alternatives_count])) {
            alternatives_count++;
        }
    }

    if (alternatives_count == 0) {
        return false;
    }

    size_t selected = randomize ? (size_t)rndm(0, alternatives_count - 1) : 0;
    if (landing != NULL) {
        *landing = alternatives[selected];
    }

    return true;
}

/** Whether a destination has a structural landing for the chosen exit mode. */
static bool exit_static_destination_usable(mapstruct *destination,
                                           int x,
                                           int y,
                                           bool allow_direct,
                                           bool fixed_pos,
                                           bool do_load) {
    return exit_find_landing_internal(NULL,
                                      destination,
                                      x,
                                      y,
                                      allow_direct,
                                      fixed_pos,
                                      false,
                                      do_load,
                                      NULL);
}

static bool exit_is_automatic(const object *op) {
    return op->type == EXIT && EXIT_PATH(op) == NULL && op->sub_type != 0;
}

static void exit_cache_clear_destination(map_exit_t *entry) {
    entry->destination_map = NULL;
    entry->destination_x = 0;
    entry->destination_y = 0;
    entry->destination_exit = NULL;
}

static void exit_cache_invalidate_entry(map_exit_t *entry, bool clear_destination) {
    entry->cache_state = EXIT_CACHE_STATIC_INVALID;
    if (clear_destination) {
        exit_cache_clear_destination(entry);
    }
}

static void exit_cache_set_destination(map_exit_t *entry,
                                       mapstruct *destination,
                                       int x,
                                       int y,
                                       object *destination_exit) {
    entry->destination_map = destination;
    entry->destination_x = x;
    entry->destination_y = y;
    entry->destination_exit = destination_exit;
    entry->cache_state = EXIT_CACHE_STATIC_VALID;
}

/** Refresh an exit route derived from the source map's tiled link. */
static void exit_refresh_tiled_route(map_exit_t *entry) {
    object *op;
    int tile;

    HARD_ASSERT(entry != NULL);

    if (!entry->tiled_route || entry->obj == NULL || entry->obj->map == NULL) {
        return;
    }

    op = entry->obj;

    /* A script may replace the derived route with an explicit path. Preserve
     * that authored override instead of rewriting it on the next map change. */
    if (EXIT_PATH(op) != NULL &&
        (entry->tiled_path == NULL || EXIT_PATH(op) != entry->tiled_path)) {
        entry->tiled_route = false;
        FREE_AND_CLEAR_HASH(entry->tiled_path);
        return;
    }

    tile = op->last_heal - 1;
    if (tile < 0 || tile >= TILED_NUM || op->map->tile_path[tile] == NULL) {
        FREE_AND_CLEAR_HASH(EXIT_PATH(op));
        EXIT_X(op) = -1;
        EXIT_Y(op) = -1;
        FREE_AND_CLEAR_HASH(entry->tiled_path);
        return;
    }

    FREE_AND_ADD_REF_HASH(EXIT_PATH(op), op->map->tile_path[tile]);
    FREE_AND_ADD_REF_HASH(entry->tiled_path, op->map->tile_path[tile]);

    EXIT_X(op) = op->x;
    EXIT_Y(op) = op->y;

    if (QUERY_FLAG(op, FLAG_XRAYS)) {
        if (!movement_direction_valid(op, op->direction, false)) {
            FREE_AND_CLEAR_HASH(EXIT_PATH(op));
            EXIT_X(op) = -1;
            EXIT_Y(op) = -1;
            return;
        }

        int direction = tile == TILED_UP ? absdir(op->direction + 4) : op->direction;
        EXIT_X(op) += freearr_x[direction];
        EXIT_Y(op) += freearr_y[direction];
    }
}

/** Register the runtime cache entry owned by a mapped exit. */
static void exit_cache_register(object *op) {
    HARD_ASSERT(op != NULL);

    if (op->type != EXIT || op->map == NULL || op->exit_cache_entry != NULL) {
        return;
    }

    map_exit_t *entry = xcalloc(1, sizeof(*entry));
    entry->obj = op;
    entry->tiled_route =
        op->last_heal > 0 && op->last_heal <= TILED_NUM &&
        (EXIT_PATH(op) == NULL || EXIT_PATH(op) == op->map->tile_path[op->last_heal - 1]);
    if (entry->tiled_route && EXIT_PATH(op) != NULL) {
        entry->tiled_path = add_refcount(EXIT_PATH(op));
    }
    op->exit_cache_entry = entry;
    DL_APPEND(op->map->exits, entry);
    exit_refresh_tiled_route(entry);
}

/** Unregister the runtime cache entry owned by an exit. */
static void exit_cache_unregister(object *op) {
    HARD_ASSERT(op != NULL);

    map_exit_t *entry = op->exit_cache_entry;
    if (entry == NULL) {
        return;
    }

    if (op->map != NULL) {
        DL_DELETE(op->map->exits, entry);
    }
    FREE_AND_CLEAR_HASH(entry->tiled_path);
    free(entry);
    op->exit_cache_entry = NULL;
}

static void exit_cache_recompute(object *op, bool do_load) {
    HARD_ASSERT(op != NULL);

    if (op->type != EXIT || op->map == NULL) {
        return;
    }

#ifdef ATRINIK_TESTING
    exit_cache_recompute_count++;
#endif

    exit_cache_register(op);
    map_exit_t *entry = op->exit_cache_entry;
    if (entry == NULL) {
        return;
    }

    exit_refresh_tiled_route(entry);

    exit_cache_invalidate_entry(entry, true);

    if (exit_is_dynamic(op)) {
        entry->cache_state = EXIT_CACHE_DYNAMIC;
        return;
    }

    if (EXIT_PATH(op) != NULL) {
        int x, y;
        mapstruct *destination = exit_get_destination_uncached(op, &x, &y, do_load);
        if (destination == NULL) {
            return;
        }

        /* Keep a dependency even when the map currently has no legal floor or
         * landing. A later floor/wall lifecycle change can then refresh this
         * exact exit without making MAP2 resolve anything. */
        entry->destination_map = destination;
        entry->destination_x = x;
        entry->destination_y = y;
        if (exit_static_destination_usable(destination,
                                           x,
                                           y,
                                           true,
                                           QUERY_FLAG(op, FLAG_USE_FIX_POS),
                                           do_load)) {
            exit_cache_set_destination(entry, destination, x, y, NULL);
        }
        return;
    }

    if (exit_is_automatic(op)) {
        object *fallback_exit = NULL;
        object *destination_exit = exit_find_static(op, do_load, &fallback_exit);
        if (destination_exit == NULL) {
            destination_exit = fallback_exit;
        }
        if (destination_exit == NULL) {
            return;
        }

        entry->destination_map = destination_exit->map;
        entry->destination_x = destination_exit->x;
        entry->destination_y = destination_exit->y;
        entry->destination_exit = destination_exit;
        if (exit_static_destination_usable(destination_exit->map,
                                           destination_exit->x,
                                           destination_exit->y,
                                           false,
                                           false,
                                           do_load)) {
            exit_cache_set_destination(entry,
                                       destination_exit->map,
                                       destination_exit->x,
                                       destination_exit->y,
                                       destination_exit);
        }
    }
}

/** Invalidate cache entries that refer to an exit which is being removed. */
static void exit_cache_invalidate_references(const object *removed) {
    for (mapstruct *map = first_map; map != NULL; map = map->next) {
        for (map_exit_t *entry = map->exits; entry != NULL; entry = entry->next) {
            if (entry->destination_exit == removed) {
                exit_cache_invalidate_entry(entry, true);
            }
        }
    }
}

static void exit_cache_refresh_automatic(mapstruct *changed_map, const object *skip) {
    for (mapstruct *map = first_map; map != NULL; map = map->next) {
        for (map_exit_t *entry = map->exits; entry != NULL; entry = entry->next) {
            if (entry->obj == NULL || !exit_is_automatic(entry->obj) ||
                exit_is_dynamic(entry->obj)) {
                continue;
            }

            if (entry->obj == skip) {
                continue;
            }

            /* The ordinary map pass already handles entries whose source or
             * retained destination is the changed map. Avoid recomputing
             * those entries a second time when a new/changed peer requires a
             * complete automatic-link refresh. */
            if (changed_map != NULL &&
                (entry->obj->map == changed_map || entry->destination_map == changed_map ||
                 (entry->destination_exit != NULL &&
                  entry->destination_exit->map == changed_map))) {
                continue;
            }

            exit_cache_recompute(entry->obj, false);
        }
    }
}

/** Whether a map contains an exit that can be an automatic-link peer. */
static bool exit_map_has_link_candidate(mapstruct *map) {
    for (map_exit_t *entry = map->exits; entry != NULL; entry = entry->next) {
        if (entry->obj != NULL && entry->obj->type == EXIT && entry->obj->sub_type != 0 &&
            !exit_is_dynamic(entry->obj)) {
            return true;
        }
    }

    return false;
}

void exit_destination_cache_refresh(object *op) {
    HARD_ASSERT(op != NULL);

    exit_cache_recompute(op, true);
}

void exit_destination_cache_refresh_all(void) {
    for (mapstruct *map = first_map; map != NULL; map = map->next) {
        for (map_exit_t *entry = map->exits; entry != NULL; entry = entry->next) {
            if (entry->obj != NULL) {
                exit_cache_recompute(entry->obj, false);
            }
        }
    }
}

#ifdef ATRINIK_TESTING
void exit_destination_cache_test_reset(void) {
    exit_cache_recompute_count = 0;
}

uint64_t exit_destination_cache_test_recompute_count(void) {
    return exit_cache_recompute_count;
}
#endif

static void exit_destination_cache_map_changed_internal(mapstruct *map,
                                                        const object *skip,
                                                        bool refresh_automatic) {
    HARD_ASSERT(map != NULL);

    for (map_exit_t *entry = map->exits; entry != NULL; entry = entry->next) {
        exit_refresh_tiled_route(entry);
    }

    for (mapstruct *source = first_map; source != NULL; source = source->next) {
        for (map_exit_t *entry = source->exits; entry != NULL; entry = entry->next) {
            if (entry->obj == NULL || entry->obj == skip || exit_is_dynamic(entry->obj)) {
                continue;
            }

            if (entry->obj->map == map || entry->destination_map == map ||
                (entry->destination_exit != NULL && entry->destination_exit->map == map)) {
                exit_cache_recompute(entry->obj, false);
            }
        }
    }

    /* A newly usable peer may be any one of the bounded candidates, not only
     * the peer retained as the current fallback. Re-evaluate the complete
     * automatic-link set so a change on a non-selected peer cannot leave a
     * previously invalid cache hidden forever. */
    if (refresh_automatic && exit_map_has_link_candidate(map)) {
        exit_cache_refresh_automatic(map, skip);
    }
}

void exit_destination_cache_map_changed(mapstruct *map) {
    exit_destination_cache_map_changed_internal(map, NULL, true);
}

void exit_destination_cache_map_unloaded(mapstruct *map) {
    HARD_ASSERT(map != NULL);

    for (mapstruct *source = first_map; source != NULL; source = source->next) {
        for (map_exit_t *entry = source->exits; entry != NULL; entry = entry->next) {
            if (entry->obj == NULL || entry->obj->map == map || entry->destination_map == map ||
                (entry->destination_exit != NULL && entry->destination_exit->map == map)) {
                exit_cache_invalidate_entry(entry, true);
            }
        }
    }
}

static bool exit_object_affects_destination(const object *op, int action) {
    if (op->type == EXIT) {
        return true;
    }

    if (op->type == PLAYER || op->type == MONSTER) {
        return false;
    }

    /* The current flags are insufficient to detect a relevant flag being
     * cleared. Treat every map insertion/removal and explicit flag rebuild as
     * a possible floor, door, blocker, or inventory-rule transition. */
    if (action == UP_OBJ_INSERT || action == UP_OBJ_REMOVE || action == UP_OBJ_FLAGS ||
        action == UP_OBJ_FLAGFACE || action == UP_OBJ_ALL) {
        return true;
    }

    return QUERY_FLAG(op, FLAG_IS_FLOOR) || QUERY_FLAG(op, FLAG_NO_PASS) ||
           QUERY_FLAG(op, FLAG_PASS_THRU) || QUERY_FLAG(op, FLAG_DOOR_CLOSED) ||
           QUERY_FLAG(op, FLAG_PLAYER_ONLY) || op->type == CHECK_INV;
}

void exit_destination_cache_object_changed(object *op, int action) {
    HARD_ASSERT(op != NULL);

    if (op->type != EXIT && op->exit_cache_entry != NULL) {
        exit_cache_invalidate_references(op);
        exit_cache_unregister(op);
        if (op->map != NULL && op->map->in_memory == MAP_IN_MEMORY) {
            exit_cache_refresh_automatic(NULL, NULL);
        }
        return;
    }

    if (op->map == NULL || !exit_object_affects_destination(op, action)) {
        return;
    }

    /* remove_map_func owns exit-entry teardown, reference invalidation, and
     * automatic-peer refresh. Recomputing before or after that callback would
     * both observe stale topology and duplicate the lifecycle work. */
    if (op->type == EXIT && action == UP_OBJ_REMOVE) {
        return;
    }

    if (op->type == EXIT && (action == UP_OBJ_INSERT || action == UP_OBJ_ALL)) {
        exit_cache_register(op);
        exit_cache_recompute(op, true);
        exit_destination_cache_map_changed_internal(op->map, op, true);
    } else {
        exit_destination_cache_map_changed_internal(op->map, NULL, op->type == EXIT);
    }
}

/**
 * Activate the exit, teleporting the person who applied it to the appropriate
 * destination.
 *
 * @param op
 * The exit.
 * @param applier
 * Who applied the exit.
 * @return
 * True on success, false on failure.
 */
static bool exit_activate(object *op, object *applier) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(applier != NULL);

    if (EXIT_PATH(op) != NULL) {
        return object_enter_map(applier, op, NULL, 0, 0, false);
    }

    bool dynamic = exit_is_dynamic(op);
    if (!dynamic && !exit_has_usable_destination(op)) {
        /* An explicit destination or automatic peer may not have been
         * resident when the source map was cached. Loading it is acceptable
         * at activation time; rendering remains a cache-only operation. Once
         * the lifecycle refresh has run, a known invalid destination stays
         * fail-closed. */
        int resolved_x, resolved_y;
        (void)exit_get_destination_uncached(op, &resolved_x, &resolved_y, true);
        exit_destination_cache_refresh(op);

        if (!exit_has_usable_destination(op)) {
            return false;
        }
    }

    int x, y;
    /* Static automatic links use the cache to decide whether the exit is
     * structurally usable, while retaining the historical random peer choice
     * when activation actually selects a destination. */
    mapstruct *m = dynamic || exit_is_automatic(op)
                       ? exit_get_destination_uncached(op, &x, &y, true)
                       : exit_get_destination(op, &x, &y, true);
    if (m == NULL) {
        return false;
    }

    exit_landing_t landing;
    if (!exit_find_landing(applier, m, x, y, false, false, true, &landing)) {
        return false;
    }

    applier->direction = landing.direction;
    SET_ANIMATION_STATE(applier);

    object_remove(applier, 0);
    applier->x = landing.x;
    applier->y = landing.y;
    object_insert_map(applier, landing.map, NULL, 0);

    return true;
}

/** @copydoc object_methods_t::apply_func */
static int apply_func(object *op, object *applier, int aflags) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(applier != NULL);

    if (op->map == NULL) {
        return OBJECT_METHOD_OK;
    }

    /* Do not allow multi-part objects to use exits. */
    if (op->more != NULL) {
        return OBJECT_METHOD_OK;
    }

    if (QUERY_FLAG(applier, FLAG_NO_TELEPORT)) {
        return OBJECT_METHOD_OK;
    }

    bool is_shop = false;
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        object *tmp = GET_MAP_OB_LAYER(op->map, op->x, op->y, LAYER_FLOOR, sub_layer);
        if (tmp != NULL && tmp->type == SHOP_FLOOR) {
            is_shop = true;
            break;
        }
    }

    /* It's a shop exit, don't let the player out until they've paid for
     * all the items they want to buy (if any). */
    if (is_shop && applier->type == PLAYER && !shop_pay_items(applier)) {
        int i = map_free_spot(applier->map,
                              applier->x,
                              applier->y,
                              1,
                              SIZEOFFREE1,
                              applier->arch,
                              NULL);
        if (i != -1) {
            object_remove(applier, 0);
            applier->x += freearr_x[i];
            applier->y += freearr_y[i];
            object_insert_map(applier, applier->map, op, 0);
        }

        return OBJECT_METHOD_OK;
    }

    /* Don't display messages for random maps. */
    if (op->msg != NULL &&
        (EXIT_PATH(op) == NULL ||
         (strncmp(EXIT_PATH(op), "/!", 2) != 0 && strncmp(EXIT_PATH(op), "/random/", 8) != 0))) {
        draw_info(COLOR_NAVY, applier, op->msg);
    } else if (is_shop) {
        draw_info(COLOR_WHITE, applier, "Thank you for visiting our shop.");
    }

    if (op->race != NULL) {
        play_sound_map(op->map, CMD_SOUND_EFFECT, op->race, op->x, op->y, 0, 0);
    }

    if (!exit_activate(op, applier)) {
        if (!QUERY_FLAG(op, FLAG_SYS_OBJECT)) {
            char *name = object_get_name_s(op, applier);
            draw_info_format(COLOR_WHITE, applier, "The %s is closed.", name);
            free(name);
        }

        log_error("Exit %s leads nowhere, applier: %s",
                  object_get_str(op),
                  object_get_str(applier));
        return OBJECT_METHOD_OK;
    }

    return OBJECT_METHOD_OK;
}

/** @copydoc object_methods_t::move_on_func */
static int move_on_func(object *op, object *victim, object *originator, int state) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(victim != NULL);

    return apply_func(op, victim, 0);
}

/** @copydoc object_methods_t::process_func */
static void process_func(object *op) {
    HARD_ASSERT(op != NULL);

    if (op->map == NULL) {
        return;
    }

    FOR_MAP_PREPARE(op->map, op->x, op->y, tmp) {
        if (tmp == op || QUERY_FLAG(tmp, FLAG_NO_TELEPORT)) {
            continue;
        }

        if (HAS_EVENT(op, EVENT_TRIGGER)) {
            int ret = trigger_event(EVENT_TRIGGER, tmp, op, NULL, NULL, 0, 0, 0, 0);
            if (ret == 1) {
                return;
            } else if (ret == 2) {
                continue;
            }
        }

        object_apply(op, tmp, 0);
    }
    FOR_MAP_FINISH();
}

/** @copydoc object_methods_t::trigger_func */
static int trigger_func(object *op, object *cause, int state) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(cause != NULL);

    process_func(op);
    return OBJECT_METHOD_OK;
}

/** @copydoc object_methods_t::insert_map_func */
static void insert_map_func(object *op) {
    HARD_ASSERT(op != NULL);

    if (EXIT_PATH(op) != NULL) {
        /* Exit has a path, ensure it's absolute and take unique maps
         * into account. */
        bool is_unique = MAP_UNIQUE(op->map) && !map_path_isabs(EXIT_PATH(op));
        char *path = map_get_path(op->map, EXIT_PATH(op), is_unique, NULL);
        FREE_AND_COPY_HASH(EXIT_PATH(op), path);
        free(path);
    } else if (EXIT_X(op) != -1 && EXIT_Y(op) != -1) {
        /* Exit with no path but has X/Y coordinates, use the map's path. */
        FREE_AND_ADD_REF_HASH(EXIT_PATH(op), op->map->path);
    }

    /* Keep every exit in the map's cache list. Pathless automatic links need
     * the same lifecycle-bound cache as explicit paths. The cached semantic
     * controls MAP2 presentation; map traversal retains the graph edge even
     * when its current destination is not enterable. */
    exit_cache_register(op);
}

/** @copydoc object_methods_t::remove_map_func */
static void remove_map_func(object *op) {
    HARD_ASSERT(op != NULL);

    bool refresh_automatic = op->sub_type != 0;
    exit_cache_invalidate_references(op);
    exit_cache_unregister(op);

    /* Removing a peer can expose an alternate on a different map, so no
     * cached destination pointer can identify every affected automatic link.
     * Map teardown already invalidates the complete cache and does not need a
     * refresh while its entries are being destroyed. */
    if (refresh_automatic && op->map->in_memory == MAP_IN_MEMORY) {
        exit_cache_refresh_automatic(NULL, NULL);
    }
}

/**
 * Initialize the exit type object methods.
 */
OBJECT_TYPE_INIT_DEFINE(exit) {
    OBJECT_METHODS(EXIT)->apply_func = apply_func;
    OBJECT_METHODS(EXIT)->move_on_func = move_on_func;
    OBJECT_METHODS(EXIT)->process_func = process_func;
    OBJECT_METHODS(EXIT)->trigger_func = trigger_func;
    OBJECT_METHODS(EXIT)->insert_map_func = insert_map_func;
    OBJECT_METHODS(EXIT)->remove_map_func = remove_map_func;
}

/**
 * Acquires the specified exit's destination (map and coordinates).
 *
 * If this function returns NULL, the contents of 'x' and 'y' are undefined.
 *
 * @param op
 * Exit.
 * @param[out] x
 * Will contain the destination X coordinate. Can be NULL.
 * @param[out] y
 * Will contain the destination Y coordinate. Can be NULL.
 * @param do_load
 * Whether to load maps if necessary.
 * @return
 * Destination map. Can be NULL.
 */
static mapstruct *exit_get_destination_uncached(object *op, int *x, int *y, bool do_load) {
    HARD_ASSERT(op != NULL);

    if (EXIT_PATH(op) != NULL) {
        mapstruct *m;
        if (do_load) {
            m = ready_map_name(EXIT_PATH(op), NULL, 0);
        } else {
            m = has_been_loaded_sh(EXIT_PATH(op));
        }

        if (m == NULL) {
            return NULL;
        }

        int xt = EXIT_X(op);
        int yt = EXIT_Y(op);
        m = do_load ? get_map_from_coord(m, &xt, &yt) : get_map_from_coord2(m, &xt, &yt);

        if (x != NULL) {
            *x = xt;
        }

        if (y != NULL) {
            *y = yt;
        }

        return m;
    } else if (op->sub_type != 0) {
        object *other_exit = exit_find(op, do_load);
        if (other_exit == NULL) {
            return NULL;
        }

        if (x != NULL) {
            *x = other_exit->x;
        }

        if (y != NULL) {
            *y = other_exit->y;
        }

        return other_exit->map;
    }

    return NULL;
}

mapstruct *exit_get_destination(object *op, int *x, int *y, bool do_load) {
    HARD_ASSERT(op != NULL);

    map_exit_t *entry = op->exit_cache_entry;
    if (entry != NULL) {
        if (entry->cache_state == EXIT_CACHE_STATIC_VALID && entry->destination_map != NULL &&
            (!do_load || entry->destination_map->in_memory == MAP_IN_MEMORY ||
             entry->destination_map->in_memory == MAP_LOADING)) {
            if (x != NULL) {
                *x = entry->destination_x;
            }

            if (y != NULL) {
                *y = entry->destination_y;
            }

            return entry->destination_map;
        }

        if (!do_load && entry->cache_state != EXIT_CACHE_DYNAMIC) {
            /* Map traversal still needs the resolved graph edge for an exit
             * that is structurally present but currently non-enterable.
             * Enterability is enforced by exit activation and advertised only
             * by the cached semantic above. */
            if (entry->destination_map != NULL) {
                if (x != NULL) {
                    *x = entry->destination_x;
                }

                if (y != NULL) {
                    *y = entry->destination_y;
                }

                return entry->destination_map;
            }

            return NULL;
        }
    }

    return exit_get_destination_uncached(op, x, y, do_load);
}
