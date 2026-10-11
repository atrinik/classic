/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <object.h>
#include <celestial_structure.h>
#include <spatial.h>

#define SPATIAL_GRAPH_LIMIT 65536U
#define SPATIAL_COORD_LIMIT 1048576

typedef struct spatial_node {
    mapstruct *map;
    int offset[3];
    unsigned depth;
} spatial_node;

static spatial_observer active_observer;

spatial_observer spatial_observer_exchange(spatial_observer observer) {
    spatial_observer previous = active_observer;
    active_observer = observer;
    return previous;
}

static bool resident(const mapstruct *map) {
    if (map != NULL && active_observer.visit != NULL) {
        active_observer.visit(map, active_observer.context);
    }
    return map != NULL && map->in_memory == MAP_IN_MEMORY &&
           map->spaces != NULL && MAP_WIDTH(map) > 0 && MAP_HEIGHT(map) > 0;
}

/* Identity transforms on vertical links are meaningful only for a complete,
 * reciprocal, equal-sized, finite stack. Pointer identity catches cycles even
 * when paths are absent (e.g. isolated generated/test maps). */
static bool stack_valid(mapstruct *map) {
    mapstruct *seen[SPATIAL_STACK_LEVELS];
    unsigned count = 1;
    seen[0] = map;
    for (int direction = TILED_UP; direction <= TILED_DOWN; direction++) {
        mapstruct *current = map;
        while (current->tile_map[direction] != NULL) {
            mapstruct *next = current->tile_map[direction];
            if (!resident(next) || next->tile_map[map_tiled_reverse[direction]] != current ||
                MAP_WIDTH(next) != MAP_WIDTH(current) ||
                MAP_HEIGHT(next) != MAP_HEIGHT(current) || count == SPATIAL_STACK_LEVELS) {
                return false;
            }
            for (unsigned i = 0; i < count; i++) {
                if (seen[i] == next) {
                    return false;
                }
            }
            seen[count++] = next;
            current = next;
        }
    }
    return true;
}

static bool neighbor(const spatial_node *node, int direction, spatial_node *next) {
    mapstruct *map = node->map;
    mapstruct *other = map->tile_map[direction];
    if (!resident(other) || other->tile_map[map_tiled_reverse[direction]] != map) {
        return false;
    }
    *next = *node;
    next->map = other;
    next->depth++;
    static const int sx[TILED_NUM] = {0,1,0,-1,1,1,-1,-1,0,0};
    static const int sy[TILED_NUM] = {-1,0,1,0,-1,1,1,-1,0,0};
    next->offset[0] += sx[direction] < 0 ? -MAP_WIDTH(other) :
                       sx[direction] > 0 ? MAP_WIDTH(map) : 0;
    next->offset[1] += sy[direction] < 0 ? -MAP_HEIGHT(other) :
                       sy[direction] > 0 ? MAP_HEIGHT(map) : 0;
    next->offset[2] += direction == TILED_UP ? 1 : direction == TILED_DOWN ? -1 : 0;
    if (direction >= TILED_UP &&
        (MAP_WIDTH(map) != MAP_WIDTH(other) || MAP_HEIGHT(map) != MAP_HEIGHT(other))) {
        return false;
    }
    return abs(next->offset[0]) <= SPATIAL_COORD_LIMIT &&
           abs(next->offset[1]) <= SPATIAL_COORD_LIMIT &&
           abs(next->offset[2]) < SPATIAL_STACK_LEVELS;
}

/* Coordinate-directed search considers both orders of a vertical/horizontal
 * seam. Every visited identity must have one transform. Unlike recursive map
 * lookup this bounds cycles and does not load a map. */
static bool resolve_search(mapstruct *origin, int x, int y, int z,
                           mapstruct *target, spatial_node *result) {
    if (!resident(origin) || !stack_valid(origin) ||
        z <= -SPATIAL_STACK_LEVELS || z >= SPATIAL_STACK_LEVELS ||
        x < -SPATIAL_COORD_LIMIT || x > SPATIAL_COORD_LIMIT ||
        y < -SPATIAL_COORD_LIMIT || y > SPATIAL_COORD_LIMIT) {
        return false;
    }
    spatial_node local_nodes[32];
    size_t local_index[64] = {0};
    size_t capacity = 32, count = 1, index_capacity = 64;
    spatial_node *nodes = local_nodes;
    size_t *index = local_index;
    size_t origin_slot = ((uintptr_t)origin >> 4) & (index_capacity - 1);
    index[origin_slot] = 1;
    nodes[0] = (spatial_node){.map = origin};
    bool found = false, valid = true;
    unsigned target_depth = SPATIAL_GRAPH_LIMIT;
    for (size_t cursor = 0; cursor < count && valid; cursor++) {
        spatial_node node = nodes[cursor];
        if (target != NULL && node.depth >= target_depth) {
            continue;
        }
        int rx = x - node.offset[0], ry = y - node.offset[1];
        bool contains = rx >= 0 && ry >= 0 && rx < MAP_WIDTH(node.map) &&
                        ry < MAP_HEIGHT(node.map) && z == node.offset[2];
        if ((target != NULL && node.map == target) || (target == NULL && contains)) {
            if (found && (result->map != node.map ||
                          memcmp(result->offset, node.offset, sizeof(node.offset)) != 0)) {
                valid = false;
                break;
            }
            *result = node;
            found = true;
        }
        int horizontal = -1;
        if (rx < 0) {
            horizontal = ry < 0 ? TILED_NORTHWEST :
                         ry >= MAP_HEIGHT(node.map) ? TILED_SOUTHWEST : TILED_WEST;
        } else if (rx >= MAP_WIDTH(node.map)) {
            horizontal = ry < 0 ? TILED_NORTHEAST :
                         ry >= MAP_HEIGHT(node.map) ? TILED_SOUTHEAST : TILED_EAST;
        } else if (ry < 0) {
            horizontal = TILED_NORTH;
        } else if (ry >= MAP_HEIGHT(node.map)) {
            horizontal = TILED_SOUTH;
        }
        for (int direction = 0; direction < TILED_NUM; direction++) {
            if (target == NULL && direction != horizontal &&
                !(direction == TILED_UP && z > node.offset[2]) &&
                !(direction == TILED_DOWN && z < node.offset[2])) {
                continue;
            }
            spatial_node next;
            if (!neighbor(&node, direction, &next)) {
                if (target == NULL && node.map->tile_map[direction] != NULL) {
                    valid = false;
                }
                continue;
            }
            if (target != NULL && next.map == target) {
                if (!stack_valid(next.map) ||
                    (found && memcmp(result->offset, next.offset, sizeof(next.offset)) != 0)) {
                    valid = false;
                    break;
                }
                *result = next;
                found = true;
                target_depth = next.depth;
            }
            size_t slot = ((uintptr_t)next.map >> 4) & (index_capacity - 1);
            while (index[slot] != 0 && nodes[index[slot] - 1].map != next.map) {
                slot = (slot + 1) & (index_capacity - 1);
            }
            if (index[slot] != 0) {
                if (memcmp(nodes[index[slot] - 1].offset, next.offset,
                           sizeof(next.offset)) != 0) {
                    valid = false;
                }
                continue;
            }
            if (count == SPATIAL_GRAPH_LIMIT) {
                valid = false;
                break;
            }
            if (!stack_valid(next.map)) {
                if (target == NULL) {
                    valid = false;
                    break;
                }
                continue;
            }
            if (count == capacity) {
                capacity *= 2;
                if (nodes == local_nodes) {
                    nodes = xmallocarray(capacity, sizeof(*nodes));
                    memcpy(nodes, local_nodes, count * sizeof(*nodes));
                } else {
                    nodes = xreallocarray(nodes, capacity, sizeof(*nodes));
                }
            }
            nodes[count++] = next;
            index[slot] = count;
            if (count * 2 == index_capacity) {
                if (index != local_index) {
                    free(index);
                }
                index_capacity *= 2;
                index = xcalloc(index_capacity, sizeof(*index));
                for (size_t i = 0; i < count; i++) {
                    size_t entry = ((uintptr_t)nodes[i].map >> 4) & (index_capacity - 1);
                    while (index[entry] != 0) {
                        entry = (entry + 1) & (index_capacity - 1);
                    }
                    index[entry] = i + 1;
                }
            }
        }
    }
    if (nodes != local_nodes) {
        free(nodes);
    }
    if (index != local_index) {
        free(index);
    }
    return valid && found;
}

bool spatial_resolve(mapstruct *origin, int x, int y, int z, spatial_cell *cell) {
    if (cell == NULL) {
        return false;
    }
    *cell = (spatial_cell){.x = x, .y = y, .z = z};
    if (resident(origin) && z > -SPATIAL_STACK_LEVELS && z < SPATIAL_STACK_LEVELS &&
        x >= 0 && y >= 0 && x < MAP_WIDTH(origin) && y < MAP_HEIGHT(origin) &&
        stack_valid(origin)) {
        mapstruct *map = origin;
        int direction = z < 0 ? TILED_DOWN : TILED_UP;
        for (int depth = 0; depth < abs(z); depth++) {
            map = map->tile_map[direction];
            if (map == NULL) {
                return false;
            }
        }
        cell->map = map;
        return true;
    }
    spatial_node node;
    if (!resolve_search(origin, x, y, z, NULL, &node)) {
        return false;
    }
    cell->map = node.map;
    cell->x -= node.offset[0];
    cell->y -= node.offset[1];
    return true;
}

bool spatial_offset(mapstruct *origin, mapstruct *target, int *dx, int *dy, int *dz) {
    if (origin == target && resident(origin) && stack_valid(origin) &&
        dx != NULL && dy != NULL && dz != NULL) {
        *dx = *dy = *dz = 0;
        return true;
    }
    if (target != NULL && dx != NULL && dy != NULL && dz != NULL &&
        resident(origin) && stack_valid(origin)) {
        /* Immediate reciprocal seams are independent of remote branches. Check
         * every direct alias of the target, so conflicting direct transforms
         * still fail closed without inspecting the rest of the component. */
        spatial_node source = {.map = origin}, direct;
        bool direct_found = false;
        for (int direction = 0; direction < TILED_UP; direction++) {
            if (origin->tile_map[direction] != target) {
                continue;
            }
            spatial_node candidate;
            if (!neighbor(&source, direction, &candidate) || !stack_valid(target) ||
                (direct_found && memcmp(direct.offset, candidate.offset,
                                        sizeof(candidate.offset)) != 0)) {
                return false;
            }
            direct = candidate;
            direct_found = true;
        }
        if (direct_found) {
            *dx = direct.offset[0];
            *dy = direct.offset[1];
            *dz = direct.offset[2];
            return true;
        }
        for (int direction = TILED_UP; direction <= TILED_DOWN; direction++) {
            mapstruct *map = origin->tile_map[direction];
            for (int depth = 1; map != NULL; depth++, map = map->tile_map[direction]) {
                if (map == target) {
                    *dx = *dy = 0;
                    *dz = direction == TILED_UP ? depth : -depth;
                    return true;
                }
            }
        }
    }
    spatial_node node;
    if (target == NULL || dx == NULL || dy == NULL || dz == NULL ||
        !resolve_search(origin, 0, 0, 0, target, &node)) {
        return false;
    }
    *dx = node.offset[0];
    *dy = node.offset[1];
    *dz = node.offset[2];
    return true;
}

bool spatial_direction_init(spatial_cursor *cursor, mapstruct *origin, int x, int y,
                            int dx, int dy, int dz, unsigned max_events) {
    if (cursor == NULL) {
        return false;
    }
    memset(cursor, 0, sizeof(*cursor));
    cursor->failed = true;
    spatial_cell start;
    if (max_events == 0 || max_events > SPATIAL_MAX_EVENTS ||
        dx < -65536 || dx > 65536 || dy < -65536 || dy > 65536 ||
        dz < -65536 || dz > 65536 || (dx == 0 && dy == 0 && dz == 0) ||
        !spatial_resolve(origin, x, y, 0, &start)) {
        return false;
    }
    cursor->origin = origin;
    cursor->position[0] = x;
    cursor->position[1] = y;
    cursor->direction[0] = dx;
    cursor->direction[1] = dy;
    cursor->direction[2] = dz;
    cursor->limit = max_events;
    cursor->failed = false;
    return true;
}

bool spatial_ray_init(spatial_cursor *cursor, mapstruct *origin, int x, int y,
                      int dx, int dy, int dz) {
    if (dx < -4096 || dx > 4096 || dy < -4096 || dy > 4096 ||
        dz <= -SPATIAL_STACK_LEVELS || dz >= SPATIAL_STACK_LEVELS) {
        return false;
    }
    bool stationary = dx == 0 && dy == 0 && dz == 0;
    if (!spatial_direction_init(cursor, origin, x, y, stationary ? 1 : dx,
                                dy, dz, SPATIAL_MAX_EVENTS)) {
        return false;
    }
    cursor->segment = true;
    cursor->endpoint[0] = x + dx;
    cursor->endpoint[1] = y + dy;
    cursor->endpoint[2] = dz;
    return true;
}

int spatial_ray_next(spatial_cursor *cursor, spatial_event *event) {
    if (cursor == NULL || event == NULL || cursor->failed) {
        return -1;
    }
    memset(event, 0, sizeof(*event));
    if (cursor->segment && memcmp(cursor->position, cursor->endpoint,
                                  sizeof(cursor->position)) == 0) {
        return 0;
    }
    if (cursor->events == cursor->limit) {
        cursor->failed = true;
        return -1;
    }
    int first = -1;
    unsigned axes = 0;
    for (int axis = 0; axis < 3; axis++) {
        if (cursor->direction[axis] == 0) {
            continue;
        }
        if (first < 0) {
            first = axis;
            axes = 1U << axis;
            continue;
        }
        uint64_t left = (2ULL * cursor->crossed[axis] + 1) *
                        (unsigned)abs(cursor->direction[first]);
        uint64_t right = (2ULL * cursor->crossed[first] + 1) *
                         (unsigned)abs(cursor->direction[axis]);
        if (left < right) {
            first = axis;
            axes = 1U << axis;
        } else if (left == right) {
            axes |= 1U << axis;
        }
    }
    spatial_cell cells[8];
    event->valid = true;
    event->axes = axes;
    for (unsigned mask = 0; mask < 8; mask++) {
        if ((mask & ~axes) != 0) {
            continue;
        }
        int point[3];
        for (int axis = 0; axis < 3; axis++) {
            point[axis] = cursor->position[axis] +
                ((mask & (1U << axis)) != 0 ? (cursor->direction[axis] > 0 ? 1 : -1) : 0);
        }
        if (!spatial_resolve(cursor->origin, point[0], point[1], point[2], &cells[mask])) {
            event->valid = false;
        }
        if (mask != 0) {
            event->cells[event->cell_count++] = cells[mask];
        }
    }
    for (unsigned mask = 0; mask < 8; mask++) {
        if ((mask & ~axes) != 0) {
            continue;
        }
        for (int axis = 0; axis < 3; axis++) {
            unsigned bit = 1U << axis;
            if ((axes & bit) == 0 || (mask & bit) != 0) {
                continue;
            }
            spatial_face *face = &event->faces[event->face_count++];
            face->from = cells[mask];
            face->to = cells[mask | bit];
            face->normal[axis] = cursor->direction[axis] > 0 ? 1 : -1;
        }
    }
    for (int axis = 0; axis < 3; axis++) {
        if ((axes & (1U << axis)) != 0) {
            cursor->position[axis] += cursor->direction[axis] > 0 ? 1 : -1;
            cursor->crossed[axis]++;
        }
    }
    cursor->events++;
    event->destination = cells[axes];
    event->terminal = cursor->segment && memcmp(cursor->position, cursor->endpoint,
                                                sizeof(cursor->position)) == 0;
    if (!event->valid) {
        cursor->failed = true;
    }
    return 1;
}

static bool slab(const spatial_cell *cell) {
    if (cell->map == NULL) {
        return true;
    }
    for (const object *op = GET_MAP_OB(cell->map, cell->x, cell->y);
         op != NULL; op = op->above) {
        if (QUERY_FLAG(op, FLAG_IS_FLOOR) ||
            (op->layer == LAYER_WALL && QUERY_FLAG(op, FLAG_HIDDEN))) {
            return true;
        }
    }
    return false;
}

bool spatial_event_clear(const spatial_event *event, spatial_policy policy,
                         bool allow_terminal) {
    if (event == NULL || !event->valid) {
        return false;
    }
    for (size_t i = 0; i < event->face_count; i++) {
        const spatial_face *face = &event->faces[i];
        if (face->normal[2] != 0 && slab(face->normal[2] > 0 ? &face->to : &face->from)) {
            return false;
        }
    }
    for (size_t i = 0; i < event->cell_count; i++) {
        const spatial_cell *cell = &event->cells[i];
        if (allow_terminal && event->terminal && cell->map == event->destination.map &&
            cell->x == event->destination.x && cell->y == event->destination.y &&
            cell->z == event->destination.z) {
            continue;
        }
        uint32_t mask = policy == SPATIAL_SIGHT ? P_BLOCKSVIEW :
                       P_DOOR_CLOSED | P_PLAYER_ONLY | P_NO_PASS | P_PASS_THRU;
        if ((GET_MAP_FLAGS(cell->map, cell->x, cell->y) & mask) != 0) {
            return false;
        }
    }
    return true;
}

unsigned spatial_event_blocked_axes(const spatial_event *event, spatial_policy policy) {
    if (event == NULL) {
        return 7;
    }
    if (!event->valid) {
        return event->axes;
    }
    unsigned axes = 0;
    uint32_t mask = policy == SPATIAL_SIGHT ? P_BLOCKSVIEW :
                   P_DOOR_CLOSED | P_PLAYER_ONLY | P_NO_PASS | P_PASS_THRU;
    for (size_t i = 0; i < event->face_count; i++) {
        const spatial_face *face = &event->faces[i];
        bool blocked = (GET_MAP_FLAGS(face->to.map, face->to.x, face->to.y) & mask) != 0;
        if (face->normal[2] != 0 && slab(face->normal[2] > 0 ? &face->to : &face->from)) {
            blocked = true;
        }
        if (blocked) {
            for (int axis = 0; axis < 3; axis++) {
                if (face->normal[axis] != 0) {
                    axes |= 1U << axis;
                }
            }
        }
    }
    return axes;
}

bool spatial_ray_clear(mapstruct *origin, int x, int y, int dx, int dy, int dz,
                       spatial_policy policy, bool allow_terminal) {
    spatial_cursor cursor;
    if (!spatial_ray_init(&cursor, origin, x, y, dx, dy, dz)) {
        return false;
    }
    spatial_event event;
    int result;
    while ((result = spatial_ray_next(&cursor, &event)) == 1) {
        if (!spatial_event_clear(&event, policy, allow_terminal)) {
            return false;
        }
    }
    return result == 0;
}

uint16_t spatial_cell_light_face(const spatial_cell *cell, int nx, int ny, int nz,
                                 bool *aperture) {
    if (aperture != NULL) {
        *aperture = false;
    }
    if (cell == NULL || !resident(cell->map) || cell->x < 0 || cell->y < 0 ||
        cell->x >= MAP_WIDTH(cell->map) || cell->y >= MAP_HEIGHT(cell->map)) {
        return 0;
    }
    /* Typed virtual covers belong to the plane immediately above this cell.
     * An OPEN rectangle can remove only the sealed virtual cap, never an
     * actual upper floor (which spatial_face_light tests independently). */
    if (nz > 0) {
        for (size_t i = 0; i < cell->map->celestial_rectangle_count; i++) {
            const celestial_rectangle_t *rectangle = &cell->map->celestial_rectangles[i];
            if (cell->x < rectangle->x || cell->y < rectangle->y ||
                cell->x >= rectangle->x + rectangle->width ||
                cell->y >= rectangle->y + rectangle->height) {
                continue;
            }
            if (rectangle->type == CELESTIAL_RECT_SKY_COVERED) {
                return 0;
            }
            if (rectangle->type == CELESTIAL_RECT_SKY_OPEN) {
                return SPATIAL_LIGHT_SCALE;
            }
        }
        return cell->map->celestial_schema == 1 &&
               cell->map->celestial_sky_above == CELESTIAL_SKY_SEALED ?
               0 : SPATIAL_LIGHT_SCALE;
    }
    uint8_t face = nz < 0 ? CELESTIAL_FACE_DOWN : nz > 0 ? 0 :
                   nx < 0 ? CELESTIAL_FACE_WEST : nx > 0 ? CELESTIAL_FACE_EAST :
                   ny < 0 ? CELESTIAL_FACE_NORTH : ny > 0 ? CELESTIAL_FACE_SOUTH : 0;
    uint16_t coefficient = SPATIAL_LIGHT_SCALE;
    bool found = false;
    for (const object *op = GET_MAP_OB(cell->map, cell->x, cell->y);
         op != NULL; op = op->above) {
        uint8_t faces = celestial_structure_faces(op);
        if (op->layer == LAYER_WALL && QUERY_FLAG(op, FLAG_HIDDEN)) {
            faces |= CELESTIAL_FACE_DOWN;
        }
        if ((faces & face) == 0) {
            continue;
        }
        found = true;
        const char *value;
        if (op->type == DOOR || op->type == GATE) {
            bool closed = op->type == DOOR ? QUERY_FLAG(op, FLAG_DOOR_CLOSED) :
                                            QUERY_FLAG(op, FLAG_NO_PASS);
            value = object_get_value(op, closed ? "celestial_transmission_closed" :
                                                  "celestial_transmission_open");
            if (aperture != NULL) {
                *aperture = true;
            }
        } else {
            value = object_get_value(op, "celestial_transmission");
            if (aperture != NULL && value != NULL) {
                *aperture = true;
            }
        }
        celestial_transmission_t transmission = celestial_structure_transmission(value);
        coefficient = MIN(coefficient, transmission == CELESTIAL_TRANSMISSION_INVALID ?
                                        0 : (uint16_t)transmission);
    }
    if (!found && face != 0 && face != CELESTIAL_FACE_DOWN &&
        (GET_MAP_FLAGS(cell->map, cell->x, cell->y) & P_BLOCKSVIEW) != 0) {
        coefficient = 0;
    }
    return coefficient;
}

uint16_t spatial_face_light(const spatial_face *face, bool *aperture) {
    if (face->normal[2] != 0) {
        uint16_t physical = spatial_cell_light_face(
            face->normal[2] > 0 ? &face->to : &face->from, 0, 0, -1, aperture);
        uint16_t virtual_cover = spatial_cell_light_face(
            face->normal[2] > 0 ? &face->from : &face->to, 0, 0, 1, NULL);
        return MIN(physical, virtual_cover);
    }
    return spatial_cell_light_face(&face->from, face->normal[0], face->normal[1], 0,
                                   aperture);
}

uint16_t spatial_event_light(const spatial_event *event) {
    if (event == NULL || !event->valid) {
        return 0;
    }
    uint16_t coefficient = SPATIAL_LIGHT_SCALE;
    for (size_t i = 0; i < event->face_count; i++) {
        coefficient = MIN(coefficient, spatial_face_light(&event->faces[i], NULL));
    }
    return coefficient;
}

int32_t spatial_light_attenuate(int32_t value, uint16_t coefficient) {
    return (int32_t)(((uint64_t)MAX(value, 0) * MIN(coefficient, SPATIAL_LIGHT_SCALE) +
                      SPATIAL_LIGHT_SCALE / 2) / SPATIAL_LIGHT_SCALE);
}
