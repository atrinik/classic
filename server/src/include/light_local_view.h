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
 * One viewer's cache of coarse local light levels that leave out the viewer's
 * own light (light_local_level_viewer()).
 */

#ifndef LIGHT_LOCAL_VIEW_H
#define LIGHT_LOCAL_VIEW_H

#include <stddef.h>
#include <stdint.h>

/** Direct-mapped entries: a viewer's reach on every shown level fits. */
#define LIGHT_LOCAL_VIEW_CACHE_SIZE 256

/** The level of one block without the viewer. All zero is an empty entry. */
typedef struct light_local_view_entry {
    /** The block's map: compared, never followed. */
    const void *map;
    /** The block's index in its map's coarse cache. */
    size_t block;
    /** The topology, the map's source stamp and the global stamp the level
     * was computed at, as a shared coarse cache entry keeps them. */
    uint64_t geometry_epoch, validated, validated_all;
    /** Tag of the viewer object the level leaves out. */
    uint32_t viewer;
    int8_t level;
} light_local_view_entry_t;

typedef struct light_local_view_cache {
    light_local_view_entry_t entries[LIGHT_LOCAL_VIEW_CACHE_SIZE];
} light_local_view_cache_t;

#endif
