/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/** @file Pure coordinate transforms for direct-to-screen map overlays. */

#ifndef MAP_TRANSFORM_H
#define MAP_TRANSFORM_H

#include <stdbool.h>
#include <stdint.h>

/** A point on the displayed screen. */
typedef struct map_screen_point {
    int x;
    int y;
} map_screen_point_t;

#define MAP_DISPLAY_ZOOM_MIN 50U
#define MAP_DISPLAY_ZOOM_MAX 400U

/**
 * Convert a map-surface-local anchor to the displayed screen coordinate.
 *
 * The widget origin, source dimensions, and displayed dimensions must describe
 * the exact surfaces used by the map blit. This preserves alignment when
 * scaled dimensions round independently or scaling falls back to the source
 * surface. Presentation offsets such as damage-number rise are deliberately
 * applied after this conversion so they remain screen-sized.
 */
bool map_local_anchor_to_screen(int origin_x,
                                int origin_y,
                                int source_width,
                                int source_height,
                                int displayed_width,
                                int displayed_height,
                                int local_x,
                                int local_y,
                                map_screen_point_t *screen);

/** Convert a per-millisecond screen-space trajectory to a whole-pixel offset. */
int map_screen_motion_offset(uint32_t elapsed_ms, double pixels_per_ms);

#endif
