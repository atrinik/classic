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
#include <stdint.h>

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

/** Server-thread geometry generation. Never wraps or grants action authority. */
uint64_t los_geometry_epoch(void);
void los_geometry_changed(void);
/** Draw-transaction mutation generation, independent of reusable LOS masks. */
uint64_t los_publication_epoch(void);
void los_publication_changed(void);
/** Checks retained masks against current geometry and all observer inputs. */
bool los_view_is_current(object *pl);

extern void update_los(object *op);

extern void clear_los(object *op);

#ifdef ATRINIK_TESTING
/** Boundary injection for isolated counter tests; does not invalidate masks. */
void los_epochs_set_for_test(uint64_t geometry, uint64_t publication);
uint64_t los_view_builds_for_test(void);
uint64_t los_view_cache_hits_for_test(void);
/** Counts for the last viewport update; fresh physical queries are excluded. */
size_t los_view_sight_checks_for_test(int depth);
size_t los_view_camera_pruned_for_test(int depth);
/** Default cell resolution calls in the last transient viewport cache. */
size_t los_view_cell_resolves_for_test(void);
/** Zero-volume proofs and actual ray traversals in the last viewport update. */
size_t los_view_clear_proofs_for_test(int depth);
size_t los_view_ray_traces_for_test(int depth);
/** Compare every query in one fresh viewport scope against the default resolver. */
bool los_view_matches_default_for_test(object *pl);
/** Reference update using default resolution for every cell and ray step. */
void los_update_uncached_for_test(object *pl);
#endif

extern int obj_in_line_of_sight(object *obj, rv_vector *rv);

#endif
