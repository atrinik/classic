/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <global.h>
#include <errno.h>
#include <object.h>
#include <object_methods.h>
#include <projectile3d.h>
#include <spatial.h>

#define Q 65536

void projectile3d_reset(object *op) {
    memset(&op->projectile, 0, sizeof(op->projectile));
}

bool projectile3d_valid(const projectile3d_state *p) {
    if (p->version != 1 || p->depth < -6 || p->depth > 6 ||
        p->remaining < 0 || p->remaining > INT16_MAX) {
        return false;
    }
    int largest = 0;
    for (int i = 0; i < 3; i++) {
        if (p->heading[i] < -Q || p->heading[i] > Q ||
            p->fraction[i] < 0 || p->fraction[i] > Q) {
            return false;
        }
        largest = MAX(largest, ABS(p->heading[i]));
    }
    return largest == Q;
}

bool projectile3d_parse(object *op, const char *text) {
    /* Parse bounded integers without scanf's overflowing conversion behavior. */
    int64_t values[9];
    const char *cp = text;
    projectile3d_reset(op);
    for (size_t i = 0; i < arraysize(values); i++) {
        while (isspace((unsigned char)*cp)) {
            cp++;
        }
        if (*cp == '\0') {
            return false;
        }
        errno = 0;
        char *end;
        long long value = strtoll(cp, &end, 10);
        if (errno != 0 || cp == end || value < INT32_MIN || value > INT32_MAX ||
            (*end != '\0' && !isspace((unsigned char)*end))) {
            return false;
        }
        values[i] = value;
        cp = end;
    }
    while (isspace((unsigned char)*cp)) {
        cp++;
    }
    if (*cp != '\0' || values[0] != 1) {
        return false;
    }
    projectile3d_state candidate = {.version = 1, .remaining = values[7], .depth = values[8]};
    for (int i = 0; i < 3; i++) {
        candidate.heading[i] = values[i + 1];
        candidate.fraction[i] = values[i + 4];
    }
    if (!projectile3d_valid(&candidate)) {
        return false;
    }
    op->projectile = candidate;
    return true;
}

static void update_direction(object *op) {
    int x = op->projectile.heading[0], y = op->projectile.heading[1];
    /* Vertical trajectories retain an existing facing for presentation. */
    if (x != 0 || y != 0) {
        int sx = (x > 0) - (x < 0), sy = (y > 0) - (y < 0);
        for (int dir = 1; dir <= NUM_DIRECTION; dir++) {
            if (freearr_x[dir] == sx && freearr_y[dir] == sy) {
                op->direction = dir;
                break;
            }
        }
    }
    if (op->direction < 1 || op->direction > NUM_DIRECTION) {
        op->direction = 1;
    }
    SET_ANIMATION_STATE(op);
}

static bool aim(object *op, const object *target, bool require_sight) {
    int dx, dy, dz;
    if (op->map == NULL || target == NULL || target->map == NULL ||
        !spatial_offset(op->map, target->map, &dx, &dy, &dz)) {
        return false;
    }
    dx += target->x - op->x;
    dy += target->y - op->y;
    if (op->projectile.depth + dz < -6 || op->projectile.depth + dz > 6 ||
        (require_sight && !spatial_ray_clear(op->map, op->x, op->y, dx, dy, dz,
                                            SPATIAL_SIGHT, true))) {
        return false;
    }
    int64_t vector[3] = {(int64_t)dx * Q + Q / 2 - op->projectile.fraction[0],
                         (int64_t)dy * Q + Q / 2 - op->projectile.fraction[1],
                         (int64_t)dz * Q + Q / 2 - op->projectile.fraction[2]};
    int64_t largest = MAX(llabs(vector[0]), MAX(llabs(vector[1]), llabs(vector[2])));
    if (largest == 0) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        int64_t magnitude = (llabs(vector[i]) * Q + largest / 2) / largest;
        op->projectile.heading[i] = vector[i] < 0 ? -magnitude : magnitude;
    }
    update_direction(op);
    return true;
}

bool projectile3d_aim(object *op, const object *target) {
    return aim(op, target, true);
}

void projectile3d_launch(object *op, const object *source, const object *target, int dir) {
    (void)source;
    projectile3d_reset(op);
    op->projectile.version = 1;
    op->projectile.remaining = MAX(0, op->last_sp);
    for (int i = 0; i < 3; i++) {
        op->projectile.fraction[i] = Q / 2;
    }
    if (dir >= 1 && dir <= NUM_DIRECTION) {
        op->projectile.heading[0] = freearr_x[dir] * Q;
        op->projectile.heading[1] = freearr_y[dir] * Q;
    }
    if (target != NULL) {
        aim(op, target, false);
    }
    if (!projectile3d_valid(&op->projectile)) {
        projectile3d_reset(op);
    }
}

void projectile3d_reflect(object *op, unsigned axes) {
    for (int i = 0; i < 3; i++) {
        if ((axes & (1U << i)) != 0) {
            op->projectile.heading[i] = -op->projectile.heading[i];
        }
    }
    update_direction(op);
}

void projectile3d_turn(object *op, int dir) {
    if (dir < 1 || dir > NUM_DIRECTION) {
        return;
    }
    int horizontal = MAX(ABS(op->projectile.heading[0]), ABS(op->projectile.heading[1]));
    if (horizontal == 0) {
        horizontal = Q / 2;
    }
    op->projectile.heading[0] = freearr_x[dir] * horizontal;
    op->projectile.heading[1] = freearr_y[dir] * horizontal;
    op->direction = dir;
    update_direction(op);
}

/* Build every cell and face incident to an edge/corner before any collision
 * callback. The shared predicate owns wall/slab and loaded-topology policy. */
static void boundary_event(object *op, unsigned axes, spatial_event *event) {
    memset(event, 0, sizeof(*event));
    event->valid = true;
    event->axes = axes;
    spatial_cell cells[8] = {{0}};
    spatial_resolve(op->map, op->x, op->y, 0, &cells[0]);
    int step[3];
    for (int i = 0; i < 3; i++) {
        step[i] = op->projectile.heading[i] > 0 ? 1 : -1;
    }
    for (unsigned subset = 1; subset <= 7; subset++) {
        if ((subset & axes) != subset) {
            continue;
        }
        int x = op->x + ((subset & 1) ? step[0] : 0);
        int y = op->y + ((subset & 2) ? step[1] : 0);
        int z = (subset & 4) ? step[2] : 0;
        if (!spatial_resolve(op->map, x, y, z, &cells[subset]) ||
            op->projectile.depth + z < -6 || op->projectile.depth + z > 6) {
            event->valid = false;
        }
        event->cells[event->cell_count++] = cells[subset];
        for (int i = 0; i < 3; i++) {
            unsigned bit = 1U << i;
            if ((subset & bit) == 0) {
                continue;
            }
            spatial_face *face = &event->faces[event->face_count++];
            face->from = cells[subset ^ bit];
            face->to = cells[subset];
            face->normal[i] = step[i];
        }
    }
    event->destination = cells[axes];
}

/* Return false after a stop, destruction, inventory capture or reflection. */
static bool hit_victim(object *op, object *victim) {
    if (((QUERY_FLAG(op, FLAG_IS_MISSILE) && QUERY_FLAG(victim, FLAG_REFL_MISSILE)) ||
         (QUERY_FLAG(op, FLAG_IS_SPELL) && QUERY_FLAG(victim, FLAG_REFL_SPELL))) &&
        rndm(0, 99) < 90 - op->level / 10) {
        projectile3d_reflect(op, 7);
        return false;
    }
    tag_t count = op->count;
    int result = object_projectile_hit(op, victim);
    if (result == OBJECT_METHOD_ERROR || !OBJECT_VALID(op, count)) {
        return false;
    }
    if (result == OBJECT_METHOD_OK) {
        object *live = object_projectile_stop(op, OBJECT_PROJECTILE_STOP_HIT);
        if (!OBJECT_VALID(op, count) || live != op || live->env != NULL ||
            !projectile3d_valid(&live->projectile)) {
            return false;
        }
    }
    return true;
}

static bool hit_cells(object *op, const spatial_event *event) {
    tag_t *seen = NULL;
    size_t seen_count = 0;
    bool keep_moving = true;
    for (size_t i = 0; i < event->cell_count && keep_moving; i++) {
        const spatial_cell *cell = &event->cells[i];
        object *victim;
        FOR_MAP_LAYER_BEGIN(cell->map, cell->x, cell->y, LAYER_LIVING, -1, victim) {
            victim = HEAD(victim);
            bool duplicate = false;
            for (size_t j = 0; j < seen_count; j++) {
                if (seen[j] == victim->count) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            seen = xrealloc(seen, (seen_count + 1) * sizeof(*seen));
            seen[seen_count++] = victim->count;
            if (!hit_victim(op, victim)) {
                keep_moving = false;
                FOR_MAP_LAYER_BREAK;
            }
        }
        FOR_MAP_LAYER_END;
    }
    free(seen);
    return keep_moving;
}

bool projectile3d_hit_current(object *op) {
    spatial_event event = {.valid = true, .cell_count = 1};
    event.cells[0] = (spatial_cell){.map = op->map, .x = op->x, .y = op->y};
    return op->map != NULL && hit_cells(op, &event);
}

static object *advance(object *op, bool probe) {
    projectile3d_state *p = &op->projectile;
    if (!projectile3d_valid(p)) {
        if (!probe) {
            object_projectile_stop(op, OBJECT_PROJECTILE_STOP_WALL);
        }
        return NULL;
    }
    int budget = Q;
    for (unsigned iteration = 0; iteration < 7 && budget > 0; iteration++) {
        int numerator = 0, denominator = 1;
        unsigned axes = 0;
        for (int i = 0; i < 3; i++) {
            int speed = ABS(p->heading[i]);
            if (speed == 0) {
                continue;
            }
            int distance = p->heading[i] > 0 ? Q - p->fraction[i] : p->fraction[i];
            int64_t left = (int64_t)distance * denominator;
            int64_t right = (int64_t)numerator * speed;
            if (axes == 0 || left < right) {
                numerator = distance;
                denominator = speed;
                axes = 1U << i;
            } else if (left == right) {
                axes |= 1U << i;
            }
        }
        if ((int64_t)numerator * Q > (int64_t)budget * denominator) {
            for (int i = 0; i < 3; i++) {
                p->fraction[i] += (int64_t)p->heading[i] * budget / Q;
            }
            return op;
        }
        for (int i = 0; i < 3; i++) {
            p->fraction[i] += (int64_t)p->heading[i] * numerator / denominator;
        }
        budget -= ((int64_t)numerator * Q + denominator - 1) / denominator;
        spatial_event event;
        boundary_event(op, axes, &event);
        if (!spatial_event_clear(&event, SPATIAL_COMBAT, false)) {
            if (probe) {
                return NULL;
            }
            if (QUERY_FLAG(op, FLAG_REFLECTING) && event.valid) {
                projectile3d_reflect(op, spatial_event_blocked_axes(&event, SPATIAL_COMBAT));
                return op; /* This movement event is consumed exactly once. */
            }
            object_projectile_stop(op, OBJECT_PROJECTILE_STOP_WALL);
            return NULL;
        }
        for (int i = 0; i < 3; i++) {
            if ((axes & (1U << i)) != 0) {
                p->fraction[i] = p->heading[i] > 0 ? 0 : Q;
            }
        }
        p->depth += event.destination.z;
        if (!probe) {
            object_remove(op, 0);
        }
        op->x = event.destination.x;
        op->y = event.destination.y;
        if (probe) {
            op->map = event.destination.map;
        } else {
            op = object_insert_map(op, event.destination.map, op, INS_NO_MERGE | INS_NO_WALK_ON);
            if (op == NULL || !hit_cells(op, &event)) {
                return NULL;
            }
        }
        p = &op->projectile;
    }
    return op;
}

object *projectile3d_move(object *op) {
    return advance(op, false);
}

bool projectile3d_can_move(const object *op) {
    /* A stack value copy is never inserted, destroyed, or passed to callbacks. */
    object preview = *op;
    return advance(&preview, true) != NULL;
}
