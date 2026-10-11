/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <perception.h>
#include <los.h>
#include <object.h>
#include <player.h>
#include <plugin.h>
#include <spatial.h>

static bool perception_flags_visible(const object *observer, const object *target) {
    if (observer->type == PLAYER) {
        if (!CONTR(observer)->tsi &&
            (QUERY_FLAG(target, FLAG_SYS_OBJECT) ||
             (QUERY_FLAG(target, FLAG_IS_INVISIBLE) &&
              !QUERY_FLAG(observer, FLAG_SEE_INVISIBLE)))) {
            return false;
        }
    } else if (IS_INVISIBLE(target, observer)) {
        return false;
    }
    return true;
}

bool perception_identity_visible(const object *observer, const object *target) {
    if (observer == NULL || target == NULL || !OBJECT_ACTIVE(observer) ||
        !OBJECT_ACTIVE(target) || observer->map == NULL || target->map == NULL ||
        observer->env != NULL || target->env != NULL) {
        return false;
    }
    if (observer == target) {
        return true;
    }
    if (!perception_flags_visible(observer, target)) {
        return false;
    }
    tag_t observer_count = observer->count, target_count = target->count;
    bool hidden = OBJECT_IS_HIDDEN((object *)observer, (object *)target);
    return !hidden && OBJECT_VALID(observer, observer_count) &&
           OBJECT_VALID(target, target_count);
}

static bool perception_part_visible(const object *observer, const object *part) {
    if (part->map == NULL || !OBJECT_ACTIVE(part) || part->env != NULL) {
        return false;
    }
    if (observer->type == PLAYER) {
        return los_player_has_geometric_sight((object *)observer, part->map, part->x, part->y);
    }
    for (const object *source = observer; source != NULL; source = source->more) {
        int dx, dy, dz;
        if (source->map != NULL &&
            spatial_offset(source->map, part->map, &dx, &dy, &dz) &&
            spatial_ray_clear(source->map, source->x, source->y,
                              dx + part->x - source->x, dy + part->y - source->y,
                              dz, SPATIAL_SIGHT, true)) {
            return true;
        }
    }
    return false;
}

bool perception_object_visible(const object *observer, const object *target) {
    if (target == NULL) {
        return false;
    }
    tag_t target_count = target->count;
    if (!perception_identity_visible(observer, HEAD(target)) ||
        !OBJECT_VALID(target, target_count)) {
        return false;
    }
    for (const object *part = target; part != NULL; part = target->head ? NULL : part->more) {
        if (perception_part_visible(observer, part)) {
            return true;
        }
    }
    return false;
}

bool perception_player_part_authorized(const player *pl, const object *part) {
    if (pl == NULL || pl->ob == NULL || part == NULL) {
        return false;
    }
    tag_t part_count = part->count;
    if (!perception_identity_visible(pl->ob, HEAD(part)) ||
        !OBJECT_VALID(part, part_count) || !perception_flags_visible(pl->ob, part) ||
        QUERY_FLAG(HEAD(part), FLAG_HIDDEN) || QUERY_FLAG(part, FLAG_HIDDEN) ||
        !los_player_can_see_cell(pl->ob, part->map, part->x, part->y)) {
        return false;
    }
    /* MAP2 live objects use the aggregate sample of their actual physical cell.
     * Xray and the explicit operator light override preserve their exceptions. */
    return part == pl->ob || QUERY_FLAG(pl->ob, FLAG_XRAYS) || pl->tli ||
           map_get_darkness(part->map, part->x, part->y, NULL) > 0;
}

bool perception_player_authorized(const player *pl, const object *target) {
    if (target == NULL) {
        return false;
    }
    for (const object *part = target; part != NULL; part = target->head ? NULL : part->more) {
        tag_t part_count = part->count;
        if (perception_player_part_authorized(pl, part)) {
            return true;
        }
        if (!OBJECT_VALID(part, part_count)) {
            return false;
        }
    }
    return false;
}
