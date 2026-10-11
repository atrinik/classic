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
 * Line of sight related functions.
 */

#include <global.h>
#include <server_main.h>
#include <los.h>
#include <player.h>
#include <object.h>
#include <spatial.h>

/* A canonical stack has thirteen maps even when its top lies beyond the
 * viewer-relative six levels serialized by MAP2. */
#define LOS_OVERHEAD_LEVELS (SPATIAL_STACK_LEVELS - 1)

#define LOS_QUERY_LEVELS (LOS_OVERHEAD_LEVELS + MAP2_MAX_DEPTH + 1)
_Static_assert(LOS_QUERY_LEVELS <= 32, "LOS column lookup mask must fit uint32_t");

typedef struct los_column {
    spatial_cell cells[SPATIAL_STACK_LEVELS];
    uint8_t index[LOS_QUERY_LEVELS];
    uint32_t checked;
    uint8_t count;
} los_column;

typedef struct los_camera_view {
    bool cutaway[LOS_OVERHEAD_LEVELS][MAP_CLIENT_X][MAP_CLIENT_Y];
    uint16_t cover[MAP_CLIENT_X][MAP_CLIENT_Y];
    object *pl;
    los_column *columns;
    size_t resolves;
} los_camera_view;

#ifdef ATRINIK_TESTING
static size_t view_sight_checks[MAP2_LEVELS];
static size_t view_camera_pruned[MAP2_LEVELS];
static size_t view_cell_resolves;

size_t los_view_sight_checks_for_test(int depth) {
    return depth >= -MAP2_MAX_DEPTH && depth <= MAP2_MAX_DEPTH
               ? view_sight_checks[MAP2_DEPTH_INDEX(depth)] : 0;
}

size_t los_view_camera_pruned_for_test(int depth) {
    return depth >= -MAP2_MAX_DEPTH && depth <= MAP2_MAX_DEPTH
               ? view_camera_pruned[MAP2_DEPTH_INDEX(depth)] : 0;
}

size_t los_view_cell_resolves_for_test(void) {
    return view_cell_resolves;
}
#endif

/* Kept for startup compatibility: sight has no precomputed 2D block table. */
void init_block(void) {}

static bool camera_surface(mapstruct *map, int x, int y) {
    if (GET_MAP_FLAGS(map, x, y) & P_BLOCKSVIEW) {
        return true;
    }
    for (object *tmp = GET_MAP_OB(map, x, y); tmp != NULL; tmp = tmp->above) {
        if (QUERY_FLAG(tmp, FLAG_IS_FLOOR) || object_is_roof_surface(tmp)) {
            return true;
        }
    }
    return false;
}

static bool camera_roof(mapstruct *map, int x, int y) {
    for (object *tmp = GET_MAP_OB(map, x, y); tmp != NULL; tmp = tmp->above) {
        if (object_is_roof_surface(tmp)) {
            return true;
        }
    }
    return false;
}

static bool camera_wall(mapstruct *map, int x, int y) {
    MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        if (GET_MAP_SPACE_LAYER(space, LAYER_WALL, sub_layer) != NULL) {
            return true;
        }
    }
    return false;
}

/** One synchronous update owns this borrowed-pointer cache. No callbacks or
 * yields can mutate topology while it lives; fresh actions create their own.
 * Each column retains at most the thirteen maps in its validated stack. */
static bool view_cell(los_camera_view *view, int ax, int ay, int depth, spatial_cell *cell) {
    object *pl = view->pl;
    socket_struct *cs = CONTR(pl)->cs;
    int x = pl->x + ax - cs->mapx_2, y = pl->y + ay - cs->mapy_2;
    *cell = (spatial_cell){.x = x, .y = y, .z = depth};
    if (ax < 0 || ay < 0 || ax >= cs->mapx || ay >= cs->mapy ||
        depth < -MAP2_MAX_DEPTH || depth > LOS_OVERHEAD_LEVELS) {
        return spatial_resolve(pl->map, x, y, depth, cell);
    }
    los_column *column = &view->columns[(size_t)ax * cs->mapy + ay];
    int index = depth + MAP2_MAX_DEPTH;
    uint32_t bit = UINT32_C(1) << index;
    if ((column->checked & bit) == 0) {
        column->checked |= bit;
        view->resolves++;
        spatial_cell resolved;
        if (spatial_resolve(pl->map, x, y, depth, &resolved) &&
            column->count < SPATIAL_STACK_LEVELS) {
            column->cells[column->count] = resolved;
            column->index[index] = ++column->count;
        }
    }
    if (column->index[index] == 0) {
        return false;
    }
    *cell = column->cells[column->index[index] - 1];
    return true;
}

static bool view_resolve(mapstruct *origin, int x, int y, int z,
                         spatial_cell *cell, void *context) {
    los_camera_view *view = context;
    if (origin != view->pl->map) {
        return spatial_resolve(origin, x, y, z, cell);
    }
    socket_struct *cs = CONTR(view->pl)->cs;
    return view_cell(view, x - view->pl->x + cs->mapx_2,
                     y - view->pl->y + cs->mapy_2, z, cell);
}

static void camera_destroy(los_camera_view *view) {
    free(view->columns);
}

/** Classify connected camera cutaways before casting any player sight rays. */
static void camera_build(object *pl, los_camera_view *view) {
    socket_struct *cs = CONTR(pl)->cs;
    bool blockers[MAP_CLIENT_X][MAP_CLIENT_Y];
    bool walls[MAP_CLIENT_X][MAP_CLIENT_Y];
    bool boundary[MAP_CLIENT_X][MAP_CLIENT_Y];
    int queue_x[MAP_CLIENT_X * MAP_CLIENT_Y];
    int queue_y[MAP_CLIENT_X * MAP_CLIENT_Y];
    memset(view, 0, sizeof(*view));
    view->pl = pl;
    view->columns = xcalloc((size_t)cs->mapx * cs->mapy, sizeof(*view->columns));

    for (int depth = 1; depth <= LOS_OVERHEAD_LEVELS; depth++) {
        spatial_cell center;
        bool cut_center = view_cell(view, cs->mapx_2, cs->mapy_2, depth, &center) &&
                          camera_surface(center.map, center.x, center.y);
        memset(blockers, 0, sizeof(blockers));
        memset(walls, 0, sizeof(walls));
        for (int ax = 0; ax < cs->mapx; ax++) {
            for (int ay = 0; ay < cs->mapy; ay++) {
                spatial_cell cell;
                if (view_cell(view, ax, ay, depth, &cell)) {
                    blockers[ax][ay] = camera_surface(cell.map, cell.x, cell.y);
                    walls[ax][ay] = camera_wall(cell.map, cell.x, cell.y);
                    if (blockers[ax][ay]) {
                        view->cover[ax][ay] |= UINT16_C(1) << (depth - 1);
                    }
                } else {
                    spatial_cell previous;
                    if (view_cell(view, ax, ay, depth - 1, &previous) &&
                        (previous.map->tile_path[TILED_UP] != NULL ||
                         previous.map->tile_map[TILED_UP] != NULL)) {
                        /* Advertised unresolved cover cannot prove exposure. */
                        view->cover[ax][ay] |= UINT16_C(1) << (depth - 1);
                    }
                }
            }
        }
        if (!cut_center) {
            continue;
        }
        bool (*cutaway)[MAP_CLIENT_Y] = view->cutaway[depth - 1];
        size_t first = 0, last = 0;
        cutaway[cs->mapx_2][cs->mapy_2] = true;
        queue_x[last] = cs->mapx_2;
        queue_y[last++] = cs->mapy_2;
        static const int nx[] = {-1, 1, 0, 0}, ny[] = {0, 0, -1, 1};
        while (first < last) {
            int x = queue_x[first], y = queue_y[first++];
            for (size_t n = 0; n < arraysize(nx); n++) {
                int ax = x + nx[n], ay = y + ny[n];
                if (ax < 0 || ay < 0 || ax >= cs->mapx || ay >= cs->mapy ||
                    !blockers[ax][ay] || cutaway[ax][ay]) {
                    continue;
                }
                cutaway[ax][ay] = true;
                queue_x[last] = ax;
                queue_y[last++] = ay;
            }
        }
        /* Include only the one-cell wall perimeter, never a recursive chain. */
        memset(boundary, 0, sizeof(boundary));
        for (int ax = 0; ax < cs->mapx; ax++) {
            for (int ay = 0; ay < cs->mapy; ay++) {
                if (!walls[ax][ay] || cutaway[ax][ay]) {
                    continue;
                }
                for (int dx = -1; dx <= 1; dx++) {
                    for (int dy = -1; dy <= 1; dy++) {
                        int x = ax + dx, y = ay + dy;
                        if (x >= 0 && y >= 0 && x < cs->mapx && y < cs->mapy && cutaway[x][y]) {
                            boundary[ax][ay] = true;
                        }
                    }
                }
            }
        }
        for (int ax = 0; ax < cs->mapx; ax++) {
            for (int ay = 0; ay < cs->mapy; ay++) {
                cutaway[ax][ay] |= boundary[ax][ay];
            }
        }
    }
}

static bool camera_cutaway(const los_camera_view *view, int depth, int ax, int ay) {
    return depth > 0 && depth <= LOS_OVERHEAD_LEVELS && view->cutaway[depth - 1][ax][ay];
}

static los_camera_visibility camera_classify(los_camera_view *view,
                                             int ax, int ay, int depth) {
    spatial_cell target;
    if (!view_cell(view, ax, ay, depth, &target) ||
        camera_cutaway(view, depth, ax, ay)) {
        return LOS_CAMERA_HIDDEN;
    }
    /* A current/lower slab remains a physical downward camera boundary. */
    for (int z = 0; depth < 0 && z > depth; z--) {
        spatial_cell cell;
        if (!view_cell(view, ax, ay, z, &cell) ||
            camera_surface(cell.map, cell.x, cell.y)) {
            return LOS_CAMERA_HIDDEN;
        }
    }
    /* Full-stack classification was built once before rays. Cutaway is a
     * presentation subtraction from cover, never a physical ray exception. */
    uint16_t cover = view->cover[ax][ay];
    for (int z = 1; z <= LOS_OVERHEAD_LEVELS; z++) {
        if (camera_cutaway(view, z, ax, ay)) {
            cover &= ~(UINT16_C(1) << (z - 1));
        }
    }
    if ((cover >> MAX(depth, 0)) != 0) {
        return LOS_CAMERA_WALL_BOUNDARY;
    }
    /* A floor supports occupants on its top face; it does not hide them.
     * A roof itself is camera structure and never authorizes its contents. */
    return camera_roof(target.map, target.x, target.y) ? LOS_CAMERA_BOUNDARY : LOS_CAMERA_VISIBLE;
}

bool los_camera_layer_visible(los_camera_visibility visibility, int layer) {
    if (visibility == LOS_CAMERA_WALL_BOUNDARY) {
        return layer == LAYER_WALL;
    }
    if (visibility == LOS_CAMERA_BOUNDARY) {
        return layer == LAYER_FLOOR || layer == LAYER_FMASK || layer == LAYER_WALL;
    }
    return visibility == LOS_CAMERA_VISIBLE;
}

static bool player_offset(object *pl, mapstruct *map, int x, int y,
                          int *dx, int *dy, int *dz) {
    int mx, my;
    if (pl == NULL || pl->type != PLAYER || QUERY_FLAG(pl, FLAG_REMOVED) ||
        CONTR(pl) == NULL || CONTR(pl)->cs == NULL || pl->map == NULL || map == NULL ||
        x < 0 || y < 0 || x >= MAP_WIDTH(map) || y >= MAP_HEIGHT(map) ||
        !spatial_offset(pl->map, map, &mx, &my, dz)) {
        return false;
    }
    *dx = mx + x - pl->x;
    *dy = my + y - pl->y;
    socket_struct *cs = CONTR(pl)->cs;
    return *dx >= -cs->mapx_2 && *dx < cs->mapx - cs->mapx_2 &&
           *dy >= -cs->mapy_2 && *dy < cs->mapy - cs->mapy_2 &&
           *dz >= -MAP2_MAX_DEPTH && *dz <= MAP2_MAX_DEPTH;
}

static bool player_ray(object *pl, int dx, int dy, int dz, los_camera_view *view) {
    if (CONTR(pl)->tls) {
        return true; /* Explicit administrator true-line-of-sight exception. */
    }
    if (QUERY_FLAG(pl, FLAG_BLIND)) {
        return dx == 0 && dy == 0 && dz == 0;
    }
    if (dz == 0 && abs(dx) <= 3 && abs(dy) <= 3 && QUERY_FLAG(pl, FLAG_XRAYS)) {
        return true; /* Same-story wall bypass; camera/slabs remain authoritative. */
    }
    if (view == NULL) {
        return spatial_ray_clear(pl->map, pl->x, pl->y, dx, dy, dz, SPATIAL_SIGHT, true);
    }
    spatial_cursor cursor;
    if (!spatial_ray_init(&cursor, pl->map, pl->x, pl->y, dx, dy, dz)) {
        return false;
    }
    spatial_event event;
    int result;
    while ((result = spatial_ray_next_resolved(&cursor, &event, view_resolve, view)) == 1) {
        /* Cached coordinates do not cache mutable flags, slabs or objects. */
        if (!spatial_event_clear(&event, SPATIAL_SIGHT, true)) {
            return false;
        }
    }
    return result == 0;
}

bool los_player_has_geometric_sight(object *pl, mapstruct *map, int x, int y) {
    int dx, dy, dz;
    return player_offset(pl, map, x, y, &dx, &dy, &dz) && player_ray(pl, dx, dy, dz, NULL);
}

los_camera_visibility los_player_cell_camera(object *pl, mapstruct *map, int x, int y) {
    int dx, dy, dz;
    if (!player_offset(pl, map, x, y, &dx, &dy, &dz)) {
        return LOS_CAMERA_HIDDEN;
    }
    if (CONTR(pl)->tls) {
        return LOS_CAMERA_VISIBLE;
    }
    los_camera_view view;
    camera_build(pl, &view);
    los_camera_visibility visibility = camera_classify(&view, dx + CONTR(pl)->cs->mapx_2,
                                                        dy + CONTR(pl)->cs->mapy_2, dz);
    camera_destroy(&view);
    return visibility;
}

bool los_player_can_see_cell(object *pl, mapstruct *map, int x, int y) {
    return los_player_has_geometric_sight(pl, map, x, y) &&
           los_player_cell_camera(pl, map, x, y) == LOS_CAMERA_VISIBLE;
}

void clear_los(object *pl) {
    for (int ax = 0; ax < MAP_CLIENT_X; ax++) {
        for (int ay = 0; ay < MAP_CLIENT_Y; ay++) {
            CONTR(pl)->blocked_los[ax][ay] = BLOCKED_LOS_OUT_OF_MAP;
        }
    }
    memset(CONTR(pl)->blocked_los_depth, BLOCKED_LOS_OUT_OF_MAP,
           sizeof(CONTR(pl)->blocked_los_depth));
    memset(CONTR(pl)->camera_visibility, LOS_CAMERA_HIDDEN, sizeof(CONTR(pl)->camera_visibility));
}

void update_los(object *pl) {
#ifdef ATRINIK_TESTING
    memset(view_sight_checks, 0, sizeof(view_sight_checks));
    memset(view_camera_pruned, 0, sizeof(view_camera_pruned));
    view_cell_resolves = 0;
#endif
    clear_los(pl);
    if (QUERY_FLAG(pl, FLAG_REMOVED) || pl->map == NULL) {
        return;
    }
    socket_struct *cs = CONTR(pl)->cs;
    los_camera_view view;
    camera_build(pl, &view);
    /* All camera classifications precede rays; covered gameplay never gets
     * discovered while iterating positive levels in packet order. */
    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        for (int ax = 0; ax < cs->mapx; ax++) {
            for (int ay = 0; ay < cs->mapy; ay++) {
                CONTR(pl)->camera_visibility[MAP2_DEPTH_INDEX(depth)][ax][ay] =
                    CONTR(pl)->tls ? LOS_CAMERA_VISIBLE : camera_classify(&view, ax, ay, depth);
            }
        }
    }
    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        for (int ax = 0; ax < cs->mapx; ax++) {
            for (int ay = 0; ay < cs->mapy; ay++) {
                int dx = ax - cs->mapx_2, dy = ay - cs->mapy_2;
                spatial_cell cell;
                uint8_t flags = BLOCKED_LOS_OUT_OF_MAP;
                if (view_cell(&view, ax, ay, depth, &cell)) {
                    flags = GET_MAP_FLAGS(cell.map, cell.x, cell.y) & P_BLOCKSVIEW
                                ? BLOCKED_LOS_BLOCKSVIEW : BLOCKED_LOS_VISIBLE;
                    if (CONTR(pl)->camera_visibility[MAP2_DEPTH_INDEX(depth)][ax][ay] ==
                        LOS_CAMERA_HIDDEN) {
                        /* Camera-hidden depths cannot disclose content. Prune
                         * before physical rays; fresh perception queries use
                         * player_ray directly and never trust this mask. */
                        flags |= BLOCKED_LOS_BLOCKED;
#ifdef ATRINIK_TESTING
                        view_camera_pruned[MAP2_DEPTH_INDEX(depth)]++;
#endif
                    } else {
#ifdef ATRINIK_TESTING
                        view_sight_checks[MAP2_DEPTH_INDEX(depth)]++;
#endif
                        if (!player_ray(pl, dx, dy, depth, &view)) {
                            flags |= BLOCKED_LOS_BLOCKED;
                        }
                    }
                }
                CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(depth)][ax][ay] = flags;
                if (depth == 0) {
                    CONTR(pl)->blocked_los[ax][ay] = flags;
                }
            }
        }
    }
#ifdef ATRINIK_TESTING
    view_cell_resolves = view.resolves;
#endif
    camera_destroy(&view);
}

/** Physical object sight is independent of player camera presentation. */
int obj_in_line_of_sight(object *obj, rv_vector *rv) {
    if (obj == NULL || rv == NULL || rv->part == NULL || rv->part->map == NULL) {
        return 0;
    }
    int dx, dy, dz;
    if (!spatial_offset(rv->part->map, obj->map, &dx, &dy, &dz)) {
        return 0;
    }
    return spatial_ray_clear(rv->part->map, rv->part->x, rv->part->y,
                             dx + obj->x - rv->part->x, dy + obj->y - rv->part->y,
                             dz, SPATIAL_SIGHT, true);
}
