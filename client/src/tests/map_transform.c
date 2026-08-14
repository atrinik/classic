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

#include <assert.h>
#include <limits.h>
#include <stddef.h>

#include <map_transform.h>

static void verify_zoom(int zoom, int expected_x, int expected_y) {
    map_screen_point_t screen;
    assert(map_local_anchor_to_screen(17, 23, (uint32_t)zoom, 48, 24, &screen));
    assert(screen.x == expected_x);
    assert(screen.y == expected_y);
}

int main(void) {
    verify_zoom(50, 41, 35);
    verify_zoom(100, 65, 47);
    verify_zoom(125, 77, 53);
    verify_zoom(400, 209, 119);

    map_screen_point_t screen;
    assert(map_local_anchor_to_screen(17, 23, 125, -3, -3, &screen));
    assert(screen.x == 14);
    assert(screen.y == 20);

    /* The 25-pixel/850-ms rise is screen-space policy, independent of zoom. */
    const double rise_per_ms = -(25.0 / 850.0);
    assert(map_screen_motion_offset(0, rise_per_ms) == 0);
    assert(map_screen_motion_offset(425, rise_per_ms) == -12);
    assert(map_screen_motion_offset(850, rise_per_ms) == -25);
    assert(map_local_anchor_to_screen(17, 23, 50, 48, 24, &screen));
    int risen_y_50 = screen.y + map_screen_motion_offset(850, rise_per_ms);
    assert(map_local_anchor_to_screen(17, 23, 400, 48, 24, &screen));
    int risen_y_400 = screen.y + map_screen_motion_offset(850, rise_per_ms);
    assert((35 - risen_y_50) == 25);
    assert((119 - risen_y_400) == 25);

    assert(!map_local_anchor_to_screen(0, 0, MAP_DISPLAY_ZOOM_MIN - 1, 1, 1, &screen));
    assert(!map_local_anchor_to_screen(0, 0, MAP_DISPLAY_ZOOM_MAX + 1, 1, 1, &screen));
    assert(!map_local_anchor_to_screen(0, 0, 100, 1, 1, NULL));
    assert(!map_local_anchor_to_screen(INT_MAX, 0, 400, INT_MAX, 0, &screen));
    return 0;
}
