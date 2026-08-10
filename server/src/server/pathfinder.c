/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
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
 * Pathfinding related code.
 *
 * Good pathfinding resources:
 * - Amit's Thoughts on Path-Finding and A-Star
 * http://theory.stanford.edu/~amitp/GameProgramming/
 * - Smart Moves: Intelligent Pathfinding
 * http://www.gamasutra.com/view/feature/3317/smart_move_intelligent_.php
 *
 * @author Zoey Rose - new A* algorithm and heuristics
 * @author Bjorn Axelsson (gecko@acc.umu.se) - original algorithm, functions and
 * structures
 */

#include <global.h>
#include <atrinik/pathfinding.h>
#include <server_main.h>
#include <initialization.h>
#include <toolkit/string.h>
#include <arch.h>
#include <player.h>
#include <object.h>
#include <exit.h>
#include <toolkit/clioptions.h>

#include <errno.h>
#include <math.h>

/**
 * Turns pathfinding profiling on/off.
 */
#define TIME_PATHFINDING 0

/**
 * Whether to visualize pathfinding.
 */
#define VISUALIZE_PATHFINDING 0

/**
 * Size of the pathfinder queue.
 */
#define PATHFINDER_QUEUE_SIZE 100

/**
 * Path cost when moving in a straight line.
 */
#define PATH_COST 1000U

/**
 * Path cost when moving diagonally.
 */
#define PATH_COST_DIAG 1010U

/**
 * Path cost per z level.
 */
#define PATH_COST_LEVEL 1000000U

/** Transition leaves its source through an exit. */
#define PATH_TRANSITION_SOURCE_EXIT UINT64_C(1)

/**
 * The pathfinder queue.
 */
static struct {
    object *waypoint; ///< Waypoint object.
    tag_t wp_count; ///< Waypoint's ID.
} pathfinder_queue[PATHFINDER_QUEUE_SIZE];

/**
 * First in the queue.
 */
static int pathfinder_queue_first = 0;
/**
 * Last in the queue.
 */
static int pathfinder_queue_last = 0;

/** Generic-core algorithm corresponding to each public server setting. */
static const atrinik_pf_algorithm core_algorithms[] = {
    ATRINIK_PF_ASTAR,
    ATRINIK_PF_DIJKSTRA,
    ATRINIK_PF_BREADTH_FIRST,
    ATRINIK_PF_GREEDY_BEST_FIRST,
};
CASSERT_ARRAY(core_algorithms, PATH_ALGO_NUM);

/**
 * Text representation of the algorithms.
 */
static const char *const algo_strs[] = {
    "A*",
    "Dijkstra",
    "BFS",
    "Greedy",
};
CASSERT_ARRAY(algo_strs, PATH_ALGO_NUM);

/**
 * Currently selected algorithm.
 */
static path_algo_t path_algo = PATH_ALGO_ASTAR;

/**
 * Heuristic greed modifier.
 */
static double path_greed = 1.0;

/** Default per-search generated-state budget; zero means unlimited. */
static size_t path_max_generated = 10000U;
TOOLKIT_API(IMPORTS(clioptions), DEPENDS(logger));

/**
 * Description of the --pathfinder_algorithm command.
 */
static const char *clioptions_option_pathfinder_algorithm_desc =
    "Changes the algorithm used for pathfinding.\n\n"
    "Available algorithms: A*, Dijkstra, BFS, Greedy";
/** @copydoc clioptions_handler_func */
static bool clioptions_option_pathfinder_algorithm(const char *arg, char **errmsg) {
    path_algo_t algo;
    for (algo = 0; algo < PATH_ALGO_NUM; algo++) {
        if (strcasecmp(algo_strs[algo], arg) == 0) {
            break;
        }
    }

    if (algo == PATH_ALGO_NUM) {
        *errmsg = xstrdup("Unknown algorithm");
        return false;
    }

    if (path_algo == algo) {
        *errmsg = xstrdup("Algorithm unchanged");
        return false;
    }

    LOG(INFO, "Pathfinding algorithm changed from %s to %s", algo_strs[path_algo], algo_strs[algo]);
    path_algo = algo;
    return true;
}
/**
 * Description of the --pathfinder_greed command.
 */
static const char *clioptions_option_pathfinder_greed_desc =
    "Sets the pathfinding heuristics greed modifier.";
/** @copydoc clioptions_handler_func */
static bool clioptions_option_pathfinder_greed(const char *arg, char **errmsg) {
    char *end;
    errno = 0;
    double greed = strtod(arg, &end);
    if (errno != 0 || end == arg || *end != '\0' || !isfinite(greed) || greed < 0.0 ||
        greed > 1000.0) {
        *errmsg = xstrdup("Greed modifier must be a finite number between 0 and 1000");
        return false;
    }
    if (DBL_EQUAL(path_greed, greed)) {
        *errmsg = xstrdup("Greed modifier unchanged");
        return false;
    }

    LOG(INFO, "Pathfinding greed modifier changed from %f to %f", path_greed, greed);
    path_greed = greed;
    return true;
}

/** Description of the --pathfinder_max_nodes command. */
static const char *clioptions_option_pathfinder_max_nodes_desc =
    "Sets the generated-state budget for each path search (zero is unlimited).";
/** @copydoc clioptions_handler_func */
static bool clioptions_option_pathfinder_max_nodes(const char *arg, char **errmsg) {
    char *end;
    errno = 0;
    unsigned long long value = strtoull(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || arg[0] == '-' || value > SIZE_MAX) {
        *errmsg = xstrdup("Node budget must be a non-negative integer representable by size_t");
        return false;
    }
    if (path_max_generated == (size_t)value) {
        *errmsg = xstrdup("Node budget unchanged");
        return false;
    }

    LOG(INFO,
        "Pathfinding generated-state budget changed from %" PRIuMAX " to %" PRIuMAX,
        (uintmax_t)path_max_generated,
        (uintmax_t)value);
    path_max_generated = (size_t)value;
    return true;
}

TOOLKIT_INIT_FUNC(pathfinder) {
    clioption_t *cli;
    CLIOPTIONS_CREATE_ARGUMENT(cli, pathfinder_algorithm, "Set pathfinding algorithm");
    clioptions_enable_changeable(cli);
    CLIOPTIONS_CREATE_ARGUMENT(cli, pathfinder_greed, "Set pathfinding greed modifier");
    clioptions_enable_changeable(cli);
    CLIOPTIONS_CREATE_ARGUMENT(cli, pathfinder_max_nodes, "Set pathfinding node budget");
    clioptions_enable_changeable(cli);
}
TOOLKIT_INIT_FUNC_FINISH

TOOLKIT_DEINIT_FUNC(pathfinder) {}
TOOLKIT_DEINIT_FUNC_FINISH

/**
 * Enqueue a waypoint for path computation.
 *
 * @param waypoint
 * Waypoint.
 * @return
 * True on success, false on failure.
 */
static bool pathfinder_queue_enqueue(object *waypoint) {
    HARD_ASSERT(waypoint != NULL);

    /* Queue full? */
    if (pathfinder_queue_last == pathfinder_queue_first - 1 ||
        (pathfinder_queue_first == 0 && pathfinder_queue_last == PATHFINDER_QUEUE_SIZE - 1)) {
        return false;
    }

    pathfinder_queue[pathfinder_queue_last].waypoint = waypoint;
    pathfinder_queue[pathfinder_queue_last].wp_count = waypoint->count;

    if (++pathfinder_queue_last >= PATHFINDER_QUEUE_SIZE) {
        pathfinder_queue_last = 0;
    }

    return true;
}

/**
 * Get the first waypoint from the queue.
 *
 * @param[out] count Waypoint's ID if there is a valid waypoint.
 * @return
 * The waypoint, NULL if the queue is empty.
 */
static object *pathfinder_queue_dequeue(tag_t *count) {
    HARD_ASSERT(count != NULL);

    /* Queue empty? */
    if (pathfinder_queue_last == pathfinder_queue_first) {
        return NULL;
    }

    object *waypoint = pathfinder_queue[pathfinder_queue_first].waypoint;
    *count = pathfinder_queue[pathfinder_queue_first].wp_count;

    if (++pathfinder_queue_first >= PATHFINDER_QUEUE_SIZE) {
        pathfinder_queue_first = 0;
    }

    return waypoint;
}

/**
 * Request a new path.
 *
 * @param waypoint
 * Waypoint.
 */
void path_request(object *waypoint) {
    HARD_ASSERT(waypoint != NULL);

    if (QUERY_FLAG(waypoint, FLAG_WP_PATH_REQUESTED)) {
        return;
    }

#ifdef DEBUG_PATHFINDING
    LOG(DEBUG, "enqueuing path request for >%s< -> >%s<", waypoint->env->name, waypoint->name);
#endif

    if (pathfinder_queue_enqueue(waypoint)) {
        SET_FLAG(waypoint, FLAG_WP_PATH_REQUESTED);
        waypoint->owner = waypoint->env;
        waypoint->ownercount = waypoint->env->count;
    }
}

/**
 * Get the next (valid) waypoint for which a path is requested.
 *
 * @return
 * Waypoint, NULL if there isn't any left.
 */
object *path_get_next_request(void) {
    object *waypoint;

    do {
        tag_t count;
        waypoint = pathfinder_queue_dequeue(&count);

        if (waypoint == NULL) {
            return NULL;
        }

        /* Verify the waypoint and its monster. */
        if (!OBJECT_VALID(waypoint, count) ||
            !OBJECT_VALID(waypoint->owner, waypoint->ownercount) ||
            !(QUERY_FLAG(waypoint, FLAG_CURSED) || QUERY_FLAG(waypoint, FLAG_DAMNED)) ||
            (QUERY_FLAG(waypoint, FLAG_DAMNED) &&
             !OBJECT_VALID(waypoint->enemy, waypoint->enemy_count))) {
            waypoint = NULL;
        }
    } while (waypoint == NULL);

#ifdef DEBUG_PATHFINDING
    LOG(DEBUG, "dequeued '%s' -> '%s'", waypoint->owner->name, waypoint->name);
#endif

    CLEAR_FLAG(waypoint, FLAG_WP_PATH_REQUESTED);
    return waypoint;
}

/**
 * Remove a node from a list.
 *
 * @param node
 * Node to remove.
 * @param[out] list List to remove from.
 */
static void path_node_remove(path_node_t *node, path_node_t **list) {
    HARD_ASSERT(node != NULL);
    HARD_ASSERT(list != NULL);

    SOFT_ASSERT(*list != NULL,
                "Removing node from an empty list: %s, %d, %d",
                node->map->path,
                node->x,
                node->y);

    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        *list = node->next;
    }

    if (node->next != NULL) {
        node->next->prev = node->prev;
    }

    node->next = node->prev = NULL;
}

/**
 * Check if the specified tile is blocked.
 *
 * @param op
 * Object.
 * @param map
 * Map.
 * @param x
 * X coordinate.
 * @param y
 * Y coordinate.
 * @return
 * 0 if the tile is not blocked, non-zero otherwise.
 */
int path_tile_blocked(object *op, mapstruct *map, int x, int y) {
    int block;
    if (op->type == PLAYER && CONTR(op)->tcl) {
        block = 0;
    } else {
        block = object_blocked(op, map, x, y);
    }

    if (block != 0) {
        return block;
    }

    if ((GET_MAP_FLAGS(map, x, y) & P_DOOR_CLOSED) == 0 &&
        (op->type != PLAYER || !CONTR(op)->tcl) && (op->behavior & BEHAVIOR_SECRET_PASSAGES) == 0 &&
        (op->type == PLAYER || !OBJECT_VALID(op->enemy, op->enemy_count)) &&
        blocks_view(map, x, y)) {
        return P_BLOCKSVIEW;
    }

    return 0;
}

/**
 * Generate a string representation of a path.
 *
 * Ideas on how to store paths:
 * - a) Store path as real waypoint objects (might be a lot of objects...)
 * - b) Store path as field in waypoints
 *   - b1) Linked list in i.e. ob->enemy (needs special free() call when
 * removing object).
 *   - b2) In ASCII in waypoint->msg (will even be saved out).
 *     - b2.1) Direction list (e.g. 1234155532, compact but fragile)
 *     - b2.2) Map / coordinate list: (/dev/testmaps:13,12 14,12 ...)
 * (human-readable (and editable), complex parsing)
 *             Approx: 600 steps in one 4096 bytes msg field
 *     - b2.2.1) Hex (/dev/testmaps/xxx D,C E,C ...) (harder to read and write,
 * more compact)
 *               Approx: 1000 steps in one 4096 bytes msg field
 *
 * @param path
 * Path node.
 * @return
 * Encoded path as a shared string.
 */
shstr *path_encode(path_node_t *path) {
    mapstruct *last_map;
    StringBuffer *sb;
    path_node_t *tmp;
    char *cp;
    shstr *ret;

    HARD_ASSERT(path != NULL);

    last_map = NULL;
    sb = stringbuffer_new();

    for (tmp = path; tmp != NULL; tmp = tmp->next) {
        if (tmp->map != last_map) {
            if (last_map != NULL) {
                stringbuffer_append_char(sb, '\n');
            }

            stringbuffer_append_string(sb, tmp->map->path);
            last_map = tmp->map;
        }

        stringbuffer_append_printf(sb, " %d,%d,%d", tmp->x, tmp->y, tmp->flags);
    }

    cp = stringbuffer_finish(sb);
    ret = add_string(cp);
    free(cp);

    return ret;
}

/**
 * Get the next location from a textual path description (generated by
 * path_encode()) starting from the character index indicated by off.
 *
 * Example text path description:
 * - /dev/testmaps/testmap_waypoints 17,7 17,10 18,11 19,10 20,10
 * - /dev/testmaps/testmap_waypoints2 8,22
 * - /dev/testmaps/testmap_waypoints3 0,22 1,23
 * - /dev/testmaps/testmap_waypoints4 1,1 2,2
 * @param buf
 * Buffer.
 * @param off
 * Offset.
 * @param mappath
 * Map path.
 * @param map
 * Map. Should be initialized with whatever map the object we are
 * working on currently lives on (to handle paths without map strings).
 * @param x
 * X position.
 * @param y
 * Y position.
 * @param flags
 * Flags.
 * @return
 * If a location is found, will return 1 and update map, x, y and off
 * (off will be set to the index to use for the next call to this function).
 * Otherwise 0 will be returned and the values of map, x and y will be undefined
 * and off will not be touched.
 */
int path_get_next(shstr *buf,
                  int16_t *off,
                  shstr **mappath,
                  mapstruct **map,
                  int *x,
                  int *y,
                  uint32_t *flags) {
    const char *coord_start, *coord_end, *map_def;

    HARD_ASSERT(buf != NULL);
    HARD_ASSERT(off != NULL);
    HARD_ASSERT(mappath != NULL);
    HARD_ASSERT(map != NULL && *map != NULL);
    HARD_ASSERT(x != NULL);
    HARD_ASSERT(y != NULL);
    HARD_ASSERT(flags != NULL);

    map_def = coord_start = buf + *off;

    if (string_isempty(*mappath)) {
        /* Scan backwards from requested offset to previous linebreak or start
         * of string */
        for (map_def = coord_start; map_def > buf && *(map_def - 1) != '\n'; map_def--) {}
    }

    /* Extract map path if any at the current position (this part is only used
     * when we go between map tiles, or when we extract the first step). */
    if (!isdigit(*map_def)) {
        /* Temporary buffer for map path extraction */
        char map_name[HUGE_BUF];
        const char *mapend;

        /* Find the end of the map path */
        mapend = strchr(map_def, ' ');

        if (mapend == NULL) {
            LOG(BUG,
                "No delimeter after map name in path description "
                "'%s' off %d",
                buf,
                *off);
            return 0;
        }

        strncpy(map_name, map_def, mapend - map_def);
        map_name[mapend - map_def] = '\0';

        /* Store the new map path in the given shared string */
        FREE_AND_COPY_HASH(*mappath, map_name);

        /* Adjust coordinate pointer to point after map path */
        if (!isdigit(*coord_start)) {
            coord_start = mapend + 1;
        }
    }

    /* Select the map we are aiming at. */
    if (*mappath) {
        /* We assume map name is already normalized. */
        if (*map == NULL || (*map)->path != *mappath) {
            *map = ready_map_name(*mappath, NULL, MAP_NAME_SHARED);
        }
    }

    if (*map == NULL) {
        LOG(BUG, "Couldn't load map from description '%s' off %d", buf, *off);
        return 0;
    }

    /* Get the requested coordinate pair. */
    coord_end = coord_start + strcspn(coord_start, " \n");

    if (coord_end == coord_start || sscanf(coord_start, "%d,%d,%d", x, y, flags) != 3) {
        LOG(BUG, "Illegal coordinate pair in '%s' off %d", buf, *off);
        return 0;
    }

    /* Adjust coordinates to be on the safe side */
    *map = get_map_from_coord(*map, x, y);

    if (*map == NULL) {
        LOG(BUG, "Location (%d, %d) is out of map", *x, *y);
        return 0;
    }

    /* Adjust the offset */
    *off = coord_end - buf + (*coord_end ? 1 : 0);

    return 1;
}

/**
 * Compress a path by removing redundant segments.
 *
 * Current implementation removes segments that can be traversed by
 * walking in a single direction.
 *
 * Something advanced could be to use a Hough transform / or something smart
 * with cross products.
 *
 * Rules:
 *  - always leave first and last path nodes
 *  - if the movement direction of node n to n+1 is the same
 *    as for n-1 to n, then remove node n.
 *
 * @param path
 * Path node.
 * @return
 * 'path' with redundant segments removed.
 */
path_node_t *path_compress(path_node_t *path) {
    path_node_t *tmp, *next;
    int last_dir;
    rv_vector v;
#ifdef DEBUG_PATHFINDING
    int removed_nodes = 0, total_nodes = 2;
#endif

    /* Guarantee at least length 3 */
    if (path == NULL || path->next == NULL) {
        return path;
    }

    next = path->next;

    get_rangevector_from_mapcoords(path->map,
                                   path->x,
                                   path->y,
                                   next->map,
                                   next->x,
                                   next->y,
                                   &v,
                                   RV_EUCLIDIAN_DISTANCE);
    last_dir = v.direction;

    for (tmp = next; tmp != NULL && tmp->next != NULL; tmp = next) {
        next = tmp->next;

#ifdef DEBUG_PATHFINDING
        total_nodes++;
#endif
        get_rangevector_from_mapcoords(tmp->map,
                                       tmp->x,
                                       tmp->y,
                                       next->map,
                                       next->x,
                                       next->y,
                                       &v,
                                       RV_EUCLIDIAN_DISTANCE);

        if (last_dir == v.direction) {
            path_node_remove(tmp, &path);
            free(tmp);
#ifdef DEBUG_PATHFINDING
            removed_nodes++;
#endif
        } else {
            last_dir = v.direction;
        }
    }

#ifdef DEBUG_PATHFINDING
    LOG(DEBUG,
        "removed %d nodes of %d (%.0f%%)",
        removed_nodes,
        total_nodes,
        (float)removed_nodes * 100.0 / (float)total_nodes);
#endif

    return path;
}

/**
 * Build a visualization hash table out of a list of visited/closed nodes.
 * @param[out] visualization Hash table to use. Must be initialized to NULL.
 * @param[out] visualizer List of the visited/closed nodes.
 */
void path_visualize(path_visualization_t **visualization, path_visualizer_t **visualizer) {
    path_visualizer_t *node, *tmp;
    path_visualization_t *visualization_node;

    HARD_ASSERT(visualization != NULL);
    HARD_ASSERT(visualizer != NULL);

    DL_FOREACH_SAFE(*visualizer, node, tmp) {
        HASH_FIND(hh, *visualization, &node->map->path, sizeof(shstr *), visualization_node);

        if (visualization_node == NULL) {
            visualization_node = xcalloc(1, sizeof(*visualization_node));
            FREE_AND_ADD_REF_HASH(visualization_node->path, node->map->path);
            HASH_ADD(hh, *visualization, path, sizeof(shstr *), visualization_node);
        }

        DL_DELETE(*visualizer, node);
        DL_APPEND(visualization_node->nodes, node);
    }
}

typedef struct path_state_key {
    mapstruct *map;
    int16_t x;
    int16_t y;
} path_state_key_t;

typedef struct path_state {
    path_state_key_t key;
    atrinik_pf_state_id id;
    bool exit_landing;
    UT_hash_handle hh;
} path_state_t;

typedef struct server_path_adapter {
    object *op;
    mapstruct *goal_map;
    int goal_x;
    int goal_y;
    int64_t start_distance_x;
    int64_t start_distance_y;
    path_state_t *states;
    path_state_t **states_by_id;
    size_t state_count;
    size_t state_capacity;
    path_visualizer_t **visualizer;
    uint32_t visualizer_id;
    bool out_of_memory;
} server_path_adapter_t;

static uint64_t path_saturating_add(uint64_t left, uint64_t right) {
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint64_t path_saturating_multiply(uint64_t left, uint64_t right) {
    return left != 0U && right > UINT64_MAX / left ? UINT64_MAX : left * right;
}

static uint64_t path_abs_i64(int64_t value) {
    if (value >= 0) {
        return (uint64_t)value;
    }
    return value == INT64_MIN ? (uint64_t)INT64_MAX + 1U : (uint64_t)-value;
}

/** Return |a*b - c*d| without signed overflow. */
static uint64_t path_saturating_cross(int64_t a, int64_t b, int64_t c, int64_t d) {
    uint64_t a_abs = path_abs_i64(a);
    uint64_t b_abs = path_abs_i64(b);
    uint64_t c_abs = path_abs_i64(c);
    uint64_t d_abs = path_abs_i64(d);
    if ((a_abs != 0U && b_abs > UINT64_MAX / a_abs) ||
        (c_abs != 0U && d_abs > UINT64_MAX / c_abs)) {
        return UINT64_MAX;
    }

    uint64_t left = a_abs * b_abs;
    uint64_t right = c_abs * d_abs;
    bool left_negative = (a < 0) != (b < 0);
    bool right_negative = (c < 0) != (d < 0);
    if (left_negative != right_negative) {
        return path_saturating_add(left, right);
    }
    return left > right ? left - right : right - left;
}

static uint64_t
path_heuristic_value(const server_path_adapter_t *adapter, mapstruct *map, int x, int y) {
    rv_vector rv;
    if (!get_rangevector_from_mapcoords(map,
                                        x,
                                        y,
                                        adapter->goal_map,
                                        adapter->goal_x,
                                        adapter->goal_y,
                                        &rv,
                                        RV_RECURSIVE_SEARCH | RV_NO_DISTANCE)) {
        return UINT64_MAX;
    }

    uint64_t dx = path_abs_i64(rv.distance_x);
    uint64_t dy = path_abs_i64(rv.distance_y);
    uint64_t straight = dx > dy ? dx - dy : dy - dx;
    uint64_t diagonal = MIN(dx, dy);
    uint64_t cross = path_saturating_cross(rv.distance_x,
                                           adapter->start_distance_y,
                                           adapter->start_distance_x,
                                           rv.distance_y);
    uint64_t result = path_saturating_multiply(straight, PATH_COST);
    result = path_saturating_add(result, path_saturating_multiply(diagonal, PATH_COST_DIAG));
    result = path_saturating_add(result, cross);
    result =
        path_saturating_add(result,
                            path_saturating_multiply(path_abs_i64(rv.distance_z), PATH_COST_LEVEL));
    return result;
}

static path_state_t *path_state_get(server_path_adapter_t *adapter,
                                    mapstruct *map,
                                    int x,
                                    int y,
                                    bool create,
                                    bool *created) {
    path_state_key_t key;
    /* uthash hashes the complete object representation, including padding. */
    memset(&key, 0, sizeof(key));
    key.map = map;
    key.x = (int16_t)x;
    key.y = (int16_t)y;
    path_state_t *state;
    HASH_FIND(hh, adapter->states, &key, sizeof(key), state);
    if (state != NULL || !create) {
        if (created != NULL) {
            *created = false;
        }
        return state;
    }

    if (adapter->state_count == adapter->state_capacity) {
        size_t capacity = adapter->state_capacity == 0U ? 64U : adapter->state_capacity * 2U;
        if (capacity < adapter->state_capacity ||
            capacity > SIZE_MAX / sizeof(*adapter->states_by_id)) {
            adapter->out_of_memory = true;
            return NULL;
        }
        path_state_t **replacement =
            realloc(adapter->states_by_id, capacity * sizeof(*adapter->states_by_id));
        if (replacement == NULL) {
            adapter->out_of_memory = true;
            return NULL;
        }
        adapter->states_by_id = replacement;
        adapter->state_capacity = capacity;
    }

    state = calloc(1U, sizeof(*state));
    if (state == NULL) {
        adapter->out_of_memory = true;
        return NULL;
    }
    state->key = key;
    state->id = adapter->state_count;
    HASH_ADD(hh, adapter->states, key, sizeof(state->key), state);
    adapter->states_by_id[adapter->state_count++] = state;
    if (created != NULL) {
        *created = true;
    }
    return state;
}

static path_state_t *path_state_by_id(server_path_adapter_t *adapter, atrinik_pf_state_id id) {
    if (id >= adapter->state_count) {
        return NULL;
    }
    return adapter->states_by_id[(size_t)id];
}

static void path_states_free(server_path_adapter_t *adapter) {
    path_state_t *state;
    path_state_t *next;
    HASH_ITER(hh, adapter->states, state, next) {
        HASH_DEL(adapter->states, state);
        free(state);
    }
    free(adapter->states_by_id);
}

static void
path_visualizer_append(server_path_adapter_t *adapter, const path_state_t *state, bool closed) {
    if (adapter->visualizer == NULL) {
        return;
    }

    path_visualizer_t *node = xcalloc(1U, sizeof(*node));
    node->map = state->key.map;
    node->x = state->key.x;
    node->y = state->key.y;
    node->closed = closed;
    node->id = adapter->visualizer_id++;
    DL_APPEND(*adapter->visualizer, node);
}

static bool path_core_goal(void *context, atrinik_pf_state_id id) {
    server_path_adapter_t *adapter = context;
    path_state_t *state = path_state_by_id(adapter, id);
    if (state == NULL) {
        return false;
    }

    bool reached = path_heuristic_value(adapter, state->key.map, state->key.x, state->key.y) <=
                   PATH_COST + PATH_COST / 5U;
    if (!reached && adapter->op->more != NULL) {
        for (object *part = adapter->op; part != NULL; part = part->more) {
            int x = state->key.x + part->arch->clone.x;
            int y = state->key.y + part->arch->clone.y;
            mapstruct *map = get_map_from_coord(state->key.map, &x, &y);
            rv_vector rv;
            if (map != NULL &&
                get_rangevector_from_mapcoords(map,
                                               x,
                                               y,
                                               adapter->goal_map,
                                               adapter->goal_x,
                                               adapter->goal_y,
                                               &rv,
                                               0) &&
                rv.distance <= 1) {
                reached = true;
                break;
            }
        }
    }
    if (reached) {
        path_visualizer_append(adapter, state, true);
        path_state_t goal = {
            .key =
                {
                    .map = adapter->goal_map,
                    .x = (int16_t)adapter->goal_x,
                    .y = (int16_t)adapter->goal_y,
                },
        };
        path_visualizer_append(adapter, &goal, true);
    }
    return reached;
}

static uint64_t path_core_heuristic(void *context, atrinik_pf_state_id id) {
    server_path_adapter_t *adapter = context;
    path_state_t *state = path_state_by_id(adapter, id);
    if (state == NULL) {
        return UINT64_MAX;
    }

    uint64_t heuristic = path_heuristic_value(adapter, state->key.map, state->key.x, state->key.y);
    if (heuristic == UINT64_MAX || path_greed == 1.0) {
        return heuristic;
    }
    long double weighted = (long double)heuristic * (long double)path_greed;
    return weighted >= (long double)UINT64_MAX ? UINT64_MAX : (uint64_t)weighted;
}

static uint64_t path_core_partial_rank(void *context, atrinik_pf_state_id id) {
    server_path_adapter_t *adapter = context;
    path_state_t *state = path_state_by_id(adapter, id);
    return state == NULL
               ? UINT64_MAX
               : path_heuristic_value(adapter, state->key.map, state->key.x, state->key.y);
}

static bool path_core_neighbors(void *context,
                                atrinik_pf_state_id id,
                                atrinik_pf_emit_fn emit,
                                void *emit_context) {
    server_path_adapter_t *adapter = context;
    path_state_t *state = path_state_by_id(adapter, id);
    if (state == NULL) {
        return false;
    }

    path_visualizer_append(adapter, state, true);
    mapstruct *origin_map = state->key.map;
    int origin_x = state->key.x;
    int origin_y = state->key.y;
    bool used_exit = false;

    if ((GET_MAP_FLAGS(origin_map, origin_x, origin_y) & P_IS_EXIT) != 0 &&
        (adapter->op->behavior & BEHAVIOR_EXITS) != 0) {
        rv_vector current_distance;
        bool have_current_distance = get_rangevector_from_mapcoords(origin_map,
                                                                    origin_x,
                                                                    origin_y,
                                                                    adapter->goal_map,
                                                                    adapter->goal_x,
                                                                    adapter->goal_y,
                                                                    &current_distance,
                                                                    RV_RECURSIVE_SEARCH);
        for (object *candidate = GET_MAP_OB(origin_map, origin_x, origin_y); candidate != NULL;
             candidate = candidate->above) {
            if (candidate->type != EXIT) {
                continue;
            }
            int destination_x;
            int destination_y;
            mapstruct *destination =
                exit_get_destination(candidate, &destination_x, &destination_y, true);
            rv_vector destination_distance;
            if (destination == NULL || !have_current_distance ||
                !get_rangevector_from_mapcoords(destination,
                                                destination_x,
                                                destination_y,
                                                adapter->goal_map,
                                                adapter->goal_x,
                                                adapter->goal_y,
                                                &destination_distance,
                                                RV_RECURSIVE_SEARCH) ||
                path_abs_i64(destination_distance.distance_z) >
                    path_abs_i64(current_distance.distance_z)) {
                continue;
            }

            path_state_t *landing =
                path_state_get(adapter, destination, destination_x, destination_y, true, NULL);
            if (landing == NULL) {
                return false;
            }
            landing->exit_landing = true;
            origin_map = destination;
            origin_x = destination_x;
            origin_y = destination_y;
            used_exit = true;
            break;
        }
    }

    for (int direction = 1; direction <= SIZEOFFREE1; direction++) {
        int x = origin_x + freearr_x[direction];
        int y = origin_y + freearr_y[direction];
        bool diagonal = x != origin_x && y != origin_y;
        mapstruct *map = get_map_from_coord(origin_map, &x, &y);
        if (map == NULL || path_tile_blocked(adapter->op, map, x, y) != 0) {
            continue;
        }
        bool created;
        path_state_t *neighbor = path_state_get(adapter, map, x, y, true, &created);
        if (neighbor == NULL) {
            return false;
        }
        if (neighbor->exit_landing) {
            continue;
        }
        if (created) {
            path_visualizer_append(adapter, neighbor, false);
        }

        uint64_t cost = diagonal ? PATH_COST_DIAG : PATH_COST;
        cost = path_saturating_add(cost, GET_MAP_MOVE_FLAGS(map, x, y));
        if ((adapter->op->behavior & BEHAVIOR_STEALTH) != 0) {
            cost = path_saturating_add(cost, (uint64_t)MAX(0, GET_MAP_LIGHT(map, x, y)));
        }
        atrinik_pf_transition transition = {
            .state = neighbor->id,
            .cost = cost,
            .data = used_exit ? PATH_TRANSITION_SOURCE_EXIT : 0U,
        };
        if (!emit(emit_context, &transition)) {
            return false;
        }
    }
    return true;
}

static path_status_t path_status_from_core(atrinik_pf_status status) {
    switch (status) {
        case ATRINIK_PF_FOUND:
            return PATH_STATUS_FOUND;
        case ATRINIK_PF_NO_PATH:
            return PATH_STATUS_NO_PATH;
        case ATRINIK_PF_LIMIT_REACHED:
            return PATH_STATUS_LIMIT_REACHED;
        case ATRINIK_PF_CANCELLED:
            return PATH_STATUS_CANCELLED;
        case ATRINIK_PF_PARTIAL:
            return PATH_STATUS_PARTIAL;
        case ATRINIK_PF_INVALID_INPUT:
            return PATH_STATUS_INVALID_INPUT;
        case ATRINIK_PF_ADAPTER_ERROR:
            return PATH_STATUS_ADAPTER_ERROR;
        case ATRINIK_PF_OUT_OF_MEMORY:
            return PATH_STATUS_OUT_OF_MEMORY;
        case ATRINIK_PF_COST_OVERFLOW:
            return PATH_STATUS_COST_OVERFLOW;
        case ATRINIK_PF_COMPLETE:
            return PATH_STATUS_ADAPTER_ERROR;
    }
    return PATH_STATUS_ADAPTER_ERROR;
}

void path_search_options_init(path_search_options_t *options) {
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->max_generated = path_max_generated;
}

static bool path_result_build(path_result_t *result,
                              server_path_adapter_t *adapter,
                              const atrinik_pf_result *core_result) {
    path_node_t *last = NULL;
    for (size_t i = 0U; i < core_result->step_count; i++) {
        path_state_t *state = path_state_by_id(adapter, core_result->steps[i].state);
        if (state == NULL) {
            return false;
        }
        path_node_t *node = calloc(1U, sizeof(*node));
        if (node == NULL) {
            return false;
        }
        node->map = state->key.map;
        node->x = state->key.x;
        node->y = state->key.y;
        node->prev = last;
        if (last == NULL) {
            result->path = node;
        } else {
            last->next = node;
        }
        last = node;
        if (i + 1U < core_result->step_count &&
            (core_result->steps[i + 1U].data & PATH_TRANSITION_SOURCE_EXIT) != 0U) {
            node->flags |= PATH_NODE_EXIT;
        }
    }
    return true;
}

path_result_t path_search(object *op,
                          mapstruct *map1,
                          int x,
                          int y,
                          mapstruct *map2,
                          int x2,
                          int y2,
                          const path_search_options_t *options,
                          path_visualizer_t **visualizer) {
    path_result_t result = {
        .status = PATH_STATUS_INVALID_INPUT,
    };
    path_search_options_t defaults;
    if (options == NULL) {
        path_search_options_init(&defaults);
        options = &defaults;
    }
    if (op == NULL || map1 == NULL || map2 == NULL || OUT_OF_MAP(map1, x, y) ||
        OUT_OF_MAP(map2, x2, y2)) {
        return result;
    }

    rv_vector start_distance;
    if (!get_rangevector_from_mapcoords(map1,
                                        x,
                                        y,
                                        map2,
                                        x2,
                                        y2,
                                        &start_distance,
                                        RV_RECURSIVE_SEARCH | RV_NO_DISTANCE)) {
        result.status = PATH_STATUS_NO_PATH;
        return result;
    }
    server_path_adapter_t server_adapter = {
        .op = op,
        .goal_map = map2,
        .goal_x = x2,
        .goal_y = y2,
        .start_distance_x = start_distance.distance_x,
        .start_distance_y = start_distance.distance_y,
        .visualizer = visualizer,
    };
    path_state_t *start = path_state_get(&server_adapter, map1, x, y, true, NULL);
    atrinik_pf_context *context = atrinik_pf_context_create();
    if (start == NULL || context == NULL) {
        result.status = PATH_STATUS_OUT_OF_MEMORY;
        atrinik_pf_context_destroy(context);
        path_states_free(&server_adapter);
        return result;
    }

    atrinik_pf_adapter adapter = {
        .context = &server_adapter,
        .neighbors = path_core_neighbors,
        .goal = path_core_goal,
        .heuristic = path_core_heuristic,
        .partial_rank = path_core_partial_rank,
    };
    atrinik_pf_options core_options;
    atrinik_pf_options_init(&core_options);
    core_options.algorithm = core_algorithms[path_algo];
    core_options.max_expanded = options->max_expanded;
    core_options.max_generated = options->max_generated;
    core_options.max_transitions = options->max_transitions;
    core_options.max_frontier = options->max_frontier;
    core_options.return_partial = options->return_partial;

    atrinik_pf_result core_result = atrinik_pf_search(context, &adapter, start->id, &core_options);
    result.status = server_adapter.out_of_memory ? PATH_STATUS_OUT_OF_MEMORY
                                                 : path_status_from_core(core_result.status);
    result.expanded = core_result.metrics.expanded;
    result.generated = core_result.metrics.generated;
    result.examined_transitions = core_result.metrics.examined_transitions;
    result.peak_frontier = core_result.metrics.peak_frontier;
    result.total_cost = core_result.metrics.total_cost;
    if (!server_adapter.out_of_memory && core_result.steps != NULL &&
        !path_result_build(&result, &server_adapter, &core_result)) {
        path_result_free(&result);
        result.status = PATH_STATUS_OUT_OF_MEMORY;
    }

    atrinik_pf_context_destroy(context);
    path_states_free(&server_adapter);
    return result;
}

void path_result_free(path_result_t *result) {
    if (result == NULL) {
        return;
    }
    path_node_t *node = result->path;
    while (node != NULL) {
        path_node_t *next = node->next;
        free(node);
        node = next;
    }
    result->path = NULL;
}

const char *path_status_string(path_status_t status) {
    static const char *const names[] = {
        "found",
        "no path",
        "limit reached",
        "partial",
        "cancelled",
        "invalid input",
        "adapter error",
        "out of memory",
        "cost overflow",
    };
    return (size_t)status < arraysize(names) ? names[status] : "unknown";
}
