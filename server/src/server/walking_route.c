/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A bounded static route derived from initialized Classic maps, using the same
 * object_blocked() collision policy as move_ob(). This is planning evidence;
 * only successful live, acknowledged walking establishes traversal evidence.
 */
#include <walking_route.h>
#include <arch.h>
#include <map.h>
#include <object.h>
#include <player.h>

#define ROUTE_SIDE 24
#define ROUTE_TILES (ROUTE_SIDE * ROUTE_SIDE)
#define ROUTE_STATES (WALKING_ROUTE_MAPS * ROUTE_TILES)

/* Server compass directions -> client numeric keypad commands. */
static const uint8_t keypad[] = {0, 8, 9, 6, 3, 2, 1, 4, 7};

typedef struct route_search {
    int next[ROUTE_STATES][8];
    int parent[ROUTE_STATES];
    int queue[ROUTE_STATES];
    uint8_t direction[ROUTE_STATES];
    bool passable[ROUTE_STATES];
    walking_route_point points[WALKING_ROUTE_LIMIT];
    size_t count;
} route_search;

static int state_id(int map, int x, int y) {
    return map * ROUTE_TILES + y * ROUTE_SIDE + x;
}

static walking_route_point state_point(int state, uint8_t direction) {
    return (walking_route_point){.map = state / ROUTE_TILES,
                                 .x = state % ROUTE_SIDE,
                                 .y = state % ROUTE_TILES / ROUTE_SIDE,
                                 .direction = direction};
}

static bool tile_passable(object *human, mapstruct *map, int x, int y) {
    /* Doors require a separate action; exits and walk-on callbacks may change
     * the destination. Neither is safe for a one-command/one-arrival trace.
     * Keep authored NPCs: an unreachable route must fail, not erase blockers. */
    if (GET_MAP_FLAGS(map, x, y) & (P_DOOR_CLOSED | P_IS_EXIT | P_WALK_ON | P_WALK_OFF)) {
        return false;
    }
    /* Offline initialization does not run spawn-point ticks. Reserve their
     * authored cells instead of routing through a guard that appears as soon
     * as the live map activates. This does not remove or move any actor. */
    for (object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
        if (op->type == SPAWN_POINT) {
            return false;
        }
    }
    /* INS_FALL_THROUGH in normal movement searches TILED_DOWN when no
     * floor exists. Such a tile cannot promise this map as its arrival. */
    bool has_floor = false;
    object *floor;
    FOR_MAP_LAYER_BEGIN(map, x, y, LAYER_FLOOR, -1, floor) {
        has_floor = floor != NULL;
        FOR_MAP_LAYER_BREAK;
    }
    FOR_MAP_LAYER_END
    return has_floor && object_blocked(human, map, x, y) == 0;
}

static void search_from(route_search *search, int start) {
    for (int i = 0; i < ROUTE_STATES; i++) {
        search->parent[i] = -1;
    }
    size_t head = 0, tail = 0;
    search->parent[start] = start;
    search->queue[tail++] = start;
    while (head < tail) {
        int state = search->queue[head++];
        for (int dir = 1; dir <= 8; dir++) {
            int next = search->next[state][dir - 1];
            if (next < 0 || search->parent[next] >= 0) {
                continue;
            }
            search->parent[next] = state;
            search->direction[next] = keypad[dir];
            search->queue[tail++] = next;
        }
    }
}

static bool append_target(route_search *search, int map, int x, int y, int radius) {
    walking_route_point last = search->points[search->count - 1];
    int start = state_id(last.map, last.x, last.y);
    search_from(search, start);
    int target = -1, best_distance = INT_MAX;
    for (int ty = 0; ty < ROUTE_SIDE; ty++) {
        for (int tx = 0; tx < ROUTE_SIDE; tx++) {
            int distance = abs(tx - x) + abs(ty - y);
            int candidate = state_id(map, tx, ty);
            if (distance <= radius && distance < best_distance && search->parent[candidate] >= 0) {
                target = candidate;
                best_distance = distance;
            }
        }
    }
    if (target < 0) {
        LOG(ERROR, "Walking route cannot reach chunk %d near (%d,%d).", map, x, y);
        return false;
    }
    size_t length = 0;
    for (int state = target; state != start; state = search->parent[state]) {
        search->queue[length++] = state;
    }
    if (length > WALKING_ROUTE_LIMIT - search->count) {
        return false;
    }
    while (length > 0) {
        int state = search->queue[--length];
        search->points[search->count++] = state_point(state, search->direction[state]);
    }
    return true;
}

bool walking_route_plan(mapstruct *const maps[WALKING_ROUTE_MAPS],
                        object *human,
                        walking_route_point **points,
                        size_t *count) {
    *points = NULL;
    *count = 0;
    if (human == NULL || human->type != PLAYER || human->more != NULL ||
        QUERY_FLAG(human, FLAG_FLYING) || QUERY_FLAG(human, FLAG_CAN_PASS_THRU)) {
        return false;
    }
    route_search *search = xcalloc(1, sizeof(*search));
    bool ok = false;
    for (int m = 0; m < WALKING_ROUTE_MAPS; m++) {
        if (maps[m] == NULL || MAP_WIDTH(maps[m]) != ROUTE_SIDE ||
            MAP_HEIGHT(maps[m]) != ROUTE_SIDE) {
            goto done;
        }
        for (int y = 0; y < ROUTE_SIDE; y++) {
            for (int x = 0; x < ROUTE_SIDE; x++) {
                search->passable[state_id(m, x, y)] = tile_passable(human, maps[m], x, y);
            }
        }
    }
    /* Restrict exploration before calling the normal tiled-map resolver. The
     * resolver, not guessed world-grid adjacency, verifies every accepted seam. */
    for (int state = 0; state < ROUTE_STATES; state++) {
        walking_route_point p = state_point(state, 0);
        for (int dir = 1; dir <= 8; dir++) {
            int *edge = &search->next[state][dir - 1];
            *edge = -1;
            if (!search->passable[state]) {
                continue;
            }
            int gx = (p.map % 4) * ROUTE_SIDE + p.x + freearr_x[dir];
            int gy = (p.map / 4) * ROUTE_SIDE + p.y + freearr_y[dir];
            if (gx < 0 || gy < 0 || gx >= 4 * ROUTE_SIDE || gy >= 5 * ROUTE_SIDE) {
                continue;
            }
            int m = (gy / ROUTE_SIDE) * 4 + gx / ROUTE_SIDE;
            int x = p.x + freearr_x[dir], y = p.y + freearr_y[dir];
            if (m != p.map) {
                int tile = x < 0             ? (y < 0             ? 7
                                                : y >= ROUTE_SIDE ? 6
                                                                  : 3)
                           : x >= ROUTE_SIDE ? (y < 0             ? 4
                                                : y >= ROUTE_SIDE ? 5
                                                                  : 1)
                           : y < 0           ? 0
                                             : 2;
                /* Do not let malformed/foreign authored links expand the
                 * bounded map set through the resolver's lazy loader. */
                if (maps[p.map]->tile_map[tile] != maps[m] &&
                    (maps[p.map]->tile_map[tile] != NULL || maps[p.map]->tile_path[tile] == NULL ||
                     maps[m]->path == NULL ||
                     strcmp(maps[p.map]->tile_path[tile], maps[m]->path) != 0)) {
                    continue;
                }
            }
            mapstruct *resolved = get_map_from_coord(maps[p.map], &x, &y);
            if (resolved != maps[m] || x != gx % ROUTE_SIDE || y != gy % ROUTE_SIDE) {
                continue;
            }
            int next = state_id(m, x, y);
            if (search->passable[next]) {
                *edge = next;
            }
        }
    }
    int start = state_id(16, 20, 8); /* Brynknot scenario dock: world_0_70. */
    if (!search->passable[start]) {
        LOG(ERROR, "Walking route start world_0_70 (20,8) is not walkable.");
        goto done;
    }
    search->points[search->count++] = state_point(start, 0);
    /* All twelve city chunks, including their less frequently visited edges. */
    for (int y = 4; y >= 1; y--) {
        for (int column = 0; column < 3; column++) {
            int x = y % 2 == 0 ? column : 2 - column;
            if (!append_target(search, y * 4 + x, 12, 12, 24)) {
                goto done;
            }
        }
    }
    /* Two distinct adjacent wilderness chunks north of town. */
    if (!append_target(search, 0, 12, 20, 24) || !append_target(search, 1, 12, 20, 24)) {
        goto done;
    }
    /* Outside walking checkpoints near bank, apartments, barracks, smith,
     * tavern, church, and library; never enter buildings to satisfy a target. */
    static const int landmarks[][3] = {
        {4, 7, 13},
        {8, 3, 3},
        {8, 14, 6},
        {12, 14, 3},
        {5, 1, 12},
        {9, 21, 4},
        {10, 8, 13},
    };
    for (size_t i = 0; i < arraysize(landmarks); i++) {
        if (!append_target(search, landmarks[i][0], landmarks[i][1], landmarks[i][2], 4)) {
            goto done;
        }
    }
    if (!append_target(search, 6, 7, 23, 0)) {
        goto done;
    }
    bool visited[WALKING_ROUTE_MAPS] = {false};
    for (size_t i = 0; i < search->count; i++) {
        visited[search->points[i].map] = true;
    }
    for (int y = 1; y < 5; y++) {
        for (int x = 0; x < 3; x++) {
            if (!visited[y * 4 + x]) {
                goto done;
            }
        }
    }
    if (!visited[0] || !visited[1]) {
        goto done;
    }
    *count = search->count;
    *points = xmalloc(*count * sizeof(**points));
    memcpy(*points, search->points, *count * sizeof(**points));
    ok = true;
done:
    free(search);
    return ok;
}

object *walking_route_candidate_create(archetype_t *archetype) {
    if (archetype == NULL || archetype->clone.type != PLAYER ||
        strcmp(archetype->name, "human_male") != 0) {
        return NULL;
    }
    object *human = arch_to_object(archetype);
    /* The normal object destructor owns this pool-allocated controller. No
     * player-list membership, socket, account, save, or inventory is created. */
    human->custom_attrset = mempool_get(pool_player);
    CONTR(human)->ob = human;
    return human;
}

int walking_route_export(void) {
    mapstruct *maps[WALKING_ROUTE_MAPS] = {NULL};
    walking_route_point *points = NULL;
    size_t count = 0;
    int result = EXIT_FAILURE;
    object *human = walking_route_candidate_create(arch_find("human_male"));
    if (human == NULL) {
        return result;
    }
    for (int m = 0; m < WALKING_ROUTE_MAPS; m++) {
        char path[MAX_BUF];
        snprintf(VS(path), "/shattered_islands/world_%d_%d", m % 4, m / 4 + 66);
        maps[m] = ready_map_name(path, NULL, MAP_FLUSH | MAP_NO_DYNAMIC);
        if (maps[m] == NULL) {
            LOG(ERROR, "Walking route could not load %s.", path);
            goto done;
        }
    }
    if (!walking_route_plan(maps, human, &points, &count)) {
        LOG(ERROR, "Authoritative Brynknot static walking route is unreachable or invalid.");
        goto done;
    }
    puts("ATRINIK_WALKING_ROUTE_BEGIN");
    puts("<live-movement-route version=\"1\" timeout-ms=\"1800000\" step-timeout-ms=\"10000\">");
    for (size_t i = 0; i < count; i++) {
        walking_route_point p = points[i];
        printf("  <checkpoint map=\"/shattered_islands/world_%d_%d\" x=\"%d\" y=\"%d\" "
               "direction=\"%d\"/>\n",
               p.map % 4,
               p.map / 4 + 66,
               p.x,
               p.y,
               p.direction);
    }
    puts("</live-movement-route>");
    puts("ATRINIK_WALKING_ROUTE_END");
    result = fflush(stdout) == 0 && !ferror(stdout) ? EXIT_SUCCESS : EXIT_FAILURE;
done:
    free(points);
    object_destroy(human);
    /* Loaded maps remain owned by the ordinary offline server cleanup(). */
    return result;
}
