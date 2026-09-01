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

#ifndef LIGHT_H
#define LIGHT_H

#include <decls.h>

#include <stdint.h>

/**
 * @file
 * Public declarations for the corresponding server module.
 */

/** Public API implemented in src/server/light.c. */

extern uint8_t light_level_from_raw(int raw_light);

/** Neutral authored light tint. */
#define LIGHT_COLOR_WHITE UINT32_C(0xffffff)

extern bool light_color_parse(const char *value, uint32_t *color);

/** Convert an authored sRGB color to normalized scene-linear Q0.16 channels. */
extern void light_color_linearize(uint32_t color, uint16_t linear[3]);

/** Resolve aggregate pre-tone Q5.11 radiance for one authorized map sample. */
extern void light_radiance_from_raw(const MapSpace *space,
                                    int raw_light,
                                    uint16_t *scalar_radiance,
                                    uint16_t radiance[3]);

/** Invalidate the bounded celestial field after structural/topology changes. */
extern void celestial_light_invalidate(mapstruct *map);

/** Invalidate all loaded celestial keyframes after a process-local override. */
extern void celestial_light_invalidate_all(void);

/** Publish the bounded celestial field for one absolute gameplay hour. */
extern bool celestial_light_rebuild(mapstruct *map, uint64_t absolute_hour);

/** Ensure the field used by map_get_darkness() matches the current hour/profile. */
extern void celestial_light_ensure(mapstruct *map);

/** Ensure current and next authoritative hourly fields are available. */
extern bool celestial_light_keyframe_ensure(mapstruct *map, uint64_t absolute_hour);

/** Return the nonzero generation identifying the current authoritative field. */
extern uint64_t celestial_light_generation(const mapstruct *map);

extern void adjust_light_source(mapstruct *map, int x, int y, int light);

extern void
adjust_light_source_color(mapstruct *map, int x, int y, int radius, uint32_t color, int direction);

extern void check_light_source_list(mapstruct *map);

extern void remove_light_source_list(mapstruct *map);

extern void recalculate_light_sources(mapstruct *map);

#endif
