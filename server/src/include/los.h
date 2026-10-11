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

#ifndef LOS_H
#define LOS_H

#include <decls.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @file
 * Public declarations for the corresponding server module.
 */

/** Public API implemented in src/server/los.c. */

extern void init_block(void);

/** Camera presentation is independent of physical sight. */
typedef enum los_camera_visibility {
    LOS_CAMERA_HIDDEN,
    LOS_CAMERA_WALL_BOUNDARY,
    LOS_CAMERA_BOUNDARY,
    LOS_CAMERA_VISIBLE,
} los_camera_visibility;

/** Fresh checks; do not use mutable viewport masks to authorize actions. */
bool los_player_can_see_cell(object *pl, mapstruct *map, int x, int y);
bool los_player_has_geometric_sight(object *pl, mapstruct *map, int x, int y);
los_camera_visibility los_player_cell_camera(object *pl, mapstruct *map, int x, int y);
bool los_camera_layer_visible(los_camera_visibility visibility, int layer);

extern void update_los(object *op);

extern void clear_los(object *op);

#ifdef ATRINIK_TESTING
/** Counts for the last viewport update; fresh physical queries are excluded. */
size_t los_view_sight_checks_for_test(int depth);
size_t los_view_camera_pruned_for_test(int depth);
#endif

extern int obj_in_line_of_sight(object *obj, rv_vector *rv);

#endif
