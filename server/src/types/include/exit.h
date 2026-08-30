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
 * Exit related header file.
 */

#ifndef EXIT_H
#define EXIT_H

/* Prototypes */

bool exit_has_usable_destination(const object *op);
mapstruct *exit_get_destination(object *op, int *x, int *y, bool do_load);

/** A resolved landing position for an exit activation. */
typedef struct exit_landing {
    mapstruct *map;
    int x;
    int y;
    int direction;
} exit_landing_t;

/**
 * Find a legal landing for an exit without changing the applier.
 *
 * The destination tile must contain a floor. If allow_direct is false, the
 * destination is used as the anchor for an adjacent landing, as used by
 * automatic links and shop mats. If fixed_pos is true, adjacent fallback is
 * disabled. The renderer must never call this function.
 */
bool exit_find_landing(object *applier,
                       mapstruct *destination,
                       int x,
                       int y,
                       bool allow_direct,
                       bool fixed_pos,
                       bool randomize,
                       exit_landing_t *landing);

/** Recompute the static semantic for a single inserted exit. */
void exit_destination_cache_refresh(object *op);

/** Refresh exit semantics affected by an object lifecycle change. */
void exit_destination_cache_object_changed(object *op, int action);

/** Recompute cached exits whose source or destination is on the map. */
void exit_destination_cache_map_changed(mapstruct *map);

/** Invalidate cached exits before a map is swapped out or deleted. */
void exit_destination_cache_map_unloaded(mapstruct *map);

/** Recompute all static exit semantics after a map load boundary. */
void exit_destination_cache_refresh_all(void);

#ifdef ATRINIK_TESTING
/** Test-only cache instrumentation; never part of the production contract. */
void exit_destination_cache_test_reset(void);
uint64_t exit_destination_cache_test_recompute_count(void);
#endif

#endif
