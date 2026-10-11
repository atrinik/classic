/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PERCEPTION_H
#define PERCEPTION_H

#include <stdbool.h>
#include <decls.h>

/* Server-thread only, borrowed objects/maps. Callers must validate retained
 * object counts before calling. No map loads or retained visibility cache. */
bool perception_object_visible(const object *observer, const object *target);
bool perception_player_authorized(const player *pl, const object *target);
/* Exact clicked/serialized part: no other part can authorize this cell. */
bool perception_player_part_authorized(const player *pl, const object *part);

#endif
