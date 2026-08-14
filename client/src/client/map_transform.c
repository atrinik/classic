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
                                int source_width,
                                int source_height,
                                int displayed_width,
                                int displayed_height,
                                int local_x,
                                int local_y,
                                map_screen_point_t *screen) {
    if (screen == NULL || source_width <= 0 || source_height <= 0 || displayed_width <= 0 ||
        displayed_height <= 0) {
        return false;
    }

    int64_t x = (int64_t)origin_x + (int64_t)local_x * displayed_width / source_width;
    int64_t y = (int64_t)origin_y + (int64_t)local_y * displayed_height / source_height;
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
