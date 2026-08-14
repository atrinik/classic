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

/** @file Map-overlay coordinate transforms. */

#include <limits.h>
#include <stddef.h>

#include <map_transform.h>

bool map_local_anchor_to_screen(int origin_x,
                                int origin_y,
                                uint32_t zoom_percent,
                                int local_x,
                                int local_y,
                                map_screen_point_t *screen) {
    if (screen == NULL || zoom_percent < MAP_DISPLAY_ZOOM_MIN ||
        zoom_percent > MAP_DISPLAY_ZOOM_MAX) {
        return false;
    }

    int64_t x = (int64_t)origin_x + (int64_t)local_x * zoom_percent / 100;
    int64_t y = (int64_t)origin_y + (int64_t)local_y * zoom_percent / 100;
    if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX) {
        return false;
    }

    screen->x = (int)x;
    screen->y = (int)y;
    return true;
}

int map_screen_motion_offset(uint32_t elapsed_ms, double pixels_per_ms) {
    double offset = elapsed_ms * pixels_per_ms;
    if (offset < INT_MIN) {
        return INT_MIN;
    }
    if (offset > INT_MAX) {
        return INT_MAX;
    }
    return (int)offset;
}
