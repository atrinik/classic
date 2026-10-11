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
    const player *observer_controller = observer->type == PLAYER ? CONTR(observer) : NULL;
    const socket_struct *observer_socket = observer_controller != NULL ? observer_controller->cs : NULL;
    const player *target_controller = target->type == PLAYER ? CONTR(target) : NULL;
    const socket_struct *target_socket = target_controller != NULL ? target_controller->cs : NULL;
    bool hidden = OBJECT_IS_HIDDEN((object *)observer, (object *)target);
    /* Logout can be followed by pool reclamation inside the same callback.
     * A pointer/count check is safe only after the live connection owns it. */
    return !hidden &&
           (observer_controller == NULL ||
            player_connection_is_current(observer, observer_count, observer_controller, observer_socket)) &&
           (target_controller == NULL ||
            player_connection_is_current(target, target_count, target_controller, target_socket)) &&
           OBJECT_VALID(observer, observer_count) &&
           OBJECT_VALID(target, target_count) && observer->map != NULL &&
           target->map != NULL && observer->env == NULL && target->env == NULL &&
           perception_flags_visible(observer, target);
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
    if (observer == NULL || target == NULL) {
        return false;
    }
    const object *head = HEAD(target);
    tag_t observer_count = observer->count, target_count = target->count;
    tag_t head_count = head->count;
    bool single_part = target->head != NULL;
    if (!perception_identity_visible(observer, head) ||
        !OBJECT_VALID(observer, observer_count) || !OBJECT_VALID(head, head_count) ||
        !OBJECT_VALID(target, target_count) || HEAD(target) != head) {
        return false;
    }
    for (const object *part = target; part != NULL; part = single_part ? NULL : part->more) {
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
    object *observer = pl->ob;
    const object *head = HEAD(part);
    tag_t observer_count = observer->count, head_count = head->count, part_count = part->count;
    if (!perception_identity_visible(observer, head) ||
        !OBJECT_VALID(observer, observer_count) || !OBJECT_VALID(head, head_count) ||
        !OBJECT_VALID(part, part_count) || HEAD(part) != head || pl->ob != observer ||
        !perception_flags_visible(observer, part) ||
        QUERY_FLAG(head, FLAG_HIDDEN) || QUERY_FLAG(part, FLAG_HIDDEN) ||
        !los_player_can_see_cell(observer, part->map, part->x, part->y)) {
        return false;
    }
    /* MAP2 live objects use the aggregate sample of their actual physical cell.
     * Xray and the explicit operator light override preserve their exceptions. */
    return part == observer || QUERY_FLAG(observer, FLAG_XRAYS) || pl->tli ||
           map_get_darkness(part->map, part->x, part->y, NULL) > 0;
}

bool perception_player_authorized(const player *pl, const object *target) {
    if (pl == NULL || pl->ob == NULL || target == NULL) {
        return false;
    }
    const object *observer = pl->ob, *head = HEAD(target);
    tag_t observer_count = observer->count, head_count = head->count;
    tag_t target_count = target->count;
    const socket_struct *observer_socket = pl->cs;
    const player *target_controller = head->type == PLAYER ? CONTR(head) : NULL;
    const socket_struct *target_socket = target_controller != NULL ? target_controller->cs : NULL;
    bool single_part = target->head != NULL;
    for (const object *part = target; part != NULL; part = single_part ? NULL : part->more) {
        tag_t part_count = part->count;
        bool authorized = perception_player_part_authorized(pl, part);
        /* EVENT_ASK_SHOW may remove or replace any actor. Validate the whole
         * borrowed multipart identity before returning or reading the next link. */
        if (!player_connection_is_current(observer, observer_count, pl, observer_socket) ||
            (target_controller != NULL &&
             !player_connection_is_current(head, head_count, target_controller, target_socket)) ||
            !OBJECT_VALID(observer, observer_count) || !OBJECT_VALID(head, head_count) ||
            !OBJECT_VALID(target, target_count) || !OBJECT_VALID(part, part_count) ||
            pl->ob != observer || HEAD(target) != head) {
            return false;
        }
        if (authorized) {
            return true;
        }
    }
    return false;
}
