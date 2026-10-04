/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef WALKING_ROUTE_H
#define WALKING_ROUTE_H

#include <global.h>

#define WALKING_ROUTE_MAPS 20
#define WALKING_ROUTE_LIMIT 8192

typedef struct walking_route_point {
    uint16_t map;
    uint8_t x, y, direction;
} walking_route_point;

/* Offline, single-threaded planner. Borrowed maps are ordered (y-66)*4+x,
 * are exactly 24x24, and retain normal server tile links. The caller owns the
 * human candidate and maps. Success returns caller-owned free()-able points;
 * failure leaves *points NULL and *count zero. No movement or persistence. */
bool walking_route_plan(mapstruct *const maps[WALKING_ROUTE_MAPS],
                        object *human,
                        walking_route_point **points,
                        size_t *count);
/* Returns an unplaced owned human candidate or NULL for a missing/wrong
 * archetype. Release with object_destroy(), including its owned controller. */
object *walking_route_candidate_create(archetype_t *archetype);
int walking_route_export(void);

#endif
