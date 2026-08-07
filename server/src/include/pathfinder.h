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
 * Pathfinding header file
 */

#ifndef PATHFINDER_H
#define PATHFINDER_H

#include <toolkit/toolkit.h>

/**
 * @defgroup PATH_NODE_xxx Path node flags
 * Flags for individual tiles in the path.
 *@{*/
/**
 * The path node has an exit.
 */
#define PATH_NODE_EXIT 0x01
/*@}*/

typedef enum path_algo {
    PATH_ALGO_ASTAR, ///< A*
    PATH_ALGO_DIJKSTRA, ///< Dijkstra
    PATH_ALGO_BFS, ///< Unweighted breadth-first search.
    PATH_ALGO_GREEDY, ///< Greedy best-first search.

    PATH_ALGO_NUM /// Total number of algorithms.
} path_algo_t;

/**
 * Path node.
 */
typedef struct path_node {
    struct path_node *next; ///< Next node in a linked list.
    struct path_node *prev; ///< Previous node in a linked list

    struct mapdef *map; ///< Pointer to the map.
    int16_t x; ///< X position on the map for this node.
    int16_t y; ///< Y position on the map for this node.
    uint8_t flags; ///< A combination of @ref PATH_NODE_xxx.
} path_node_t;

/** Server-facing path-search status. */
typedef enum path_status {
    PATH_STATUS_FOUND,
    PATH_STATUS_NO_PATH,
    PATH_STATUS_LIMIT_REACHED,
    PATH_STATUS_PARTIAL,
    PATH_STATUS_CANCELLED,
    PATH_STATUS_INVALID_INPUT,
    PATH_STATUS_ADAPTER_ERROR,
    PATH_STATUS_OUT_OF_MEMORY,
    PATH_STATUS_COST_OVERFLOW
} path_status_t;

/** Per-search policy. Zero budgets are unlimited. */
typedef struct path_search_options {
    size_t max_expanded;
    size_t max_generated;
    size_t max_transitions;
    size_t max_frontier;
    bool return_partial;
} path_search_options_t;

/** Search result. The path is owned by this structure. */
typedef struct path_result {
    path_status_t status;
    path_node_t *path;
    size_t expanded;
    size_t generated;
    size_t examined_transitions;
    size_t peak_frontier;
    uint64_t total_cost;
} path_result_t;

/**
 * Used for visualization of path nodes; represents one node.
 */
typedef struct path_visualizer {
    struct path_visualizer *next; ///< Next 'node'.
    struct path_visualizer *prev; ///< Previous 'node'.

    mapstruct *map; ///< Map.
    int16_t x; ///< X position.
    int16_t y; ///< Y position.

    uint32_t id; ///< UID of the node; can be used for insertion order sorting.
    bool closed; ///< Whether the node is closed or just visited.
} path_visualizer_t;

/**
 * Contains all the maps that were visited in a hash table, for the purposes of
 * path visualization.
 */
typedef struct path_visualization {
    shstr *path; ///< Map path. Used as key for the hash table.
    path_visualizer_t *nodes; ///< Visited nodes on this map.
    UT_hash_handle hh; ///< Hash handle.
} path_visualization_t;

/**
 * Pseudo-flag used to mark waypoints as "has requested path".
 *
 * Reuses a non-saved flag.
 */
#define FLAG_WP_PATH_REQUESTED FLAG_PARALYZED

/* Uncomment this to enable some verbose pathfinding debug messages */
/* #define DEBUG_PATHFINDING */

/**
 * Enable more intelligent use of CPU time for path finding?
 */
#define LEFTOVER_CPU_FOR_PATHFINDING

/* Prototypes */

TOOLKIT_FUNCS_DECLARE(pathfinder);

/** Public API implemented in src/server/pathfinder.c. */

extern void path_request(object *waypoint);

extern object *path_get_next_request(void);

extern shstr *path_encode(path_node_t *path);

extern int path_get_next(shstr *buf,
                         int16_t *off,
                         shstr **mappath,
                         mapstruct **map,
                         int *x,
                         int *y,
                         uint32_t *flags);

extern path_node_t *path_compress(path_node_t *path);

extern void path_visualize(path_visualization_t **visualization, path_visualizer_t **visualizer);

extern void path_search_options_init(path_search_options_t *options);

extern path_result_t path_search(object *op,
                                 mapstruct *map1,
                                 int x,
                                 int y,
                                 mapstruct *map2,
                                 int x2,
                                 int y2,
                                 const path_search_options_t *options,
                                 path_visualizer_t **visualizer);

extern void path_result_free(path_result_t *result);

extern const char *path_status_string(path_status_t status);

#endif
