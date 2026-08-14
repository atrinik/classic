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

#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <map_transform.h>

#define CHECK(expression)                                                            \
    do {                                                                             \
        if (!(expression)) {                                                         \
            fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expression); \
            return EXIT_FAILURE;                                                     \
        }                                                                            \
    } while (0)

static bool verify_zoom(int zoom, int expected_x, int expected_y) {
    map_screen_point_t screen = {0};
    return map_local_anchor_to_screen(17, 23, (uint32_t)zoom, 48, 24, &screen) &&
           screen.x == expected_x && screen.y == expected_y;
}

int main(void) {
    CHECK(verify_zoom(50, 41, 35));
    CHECK(verify_zoom(100, 65, 47));
    CHECK(verify_zoom(125, 77, 53));
    CHECK(verify_zoom(400, 209, 119));

    map_screen_point_t screen = {0};
    CHECK(map_local_anchor_to_screen(17, 23, 125, -3, -3, &screen));
    CHECK(screen.x == 14);
    CHECK(screen.y == 20);

    map_screen_point_t moved_screen = {0};
    CHECK(map_local_anchor_to_screen(89, 101, 125, 48, 24, &moved_screen));
    CHECK(moved_screen.x - 77 == 72);
    CHECK(moved_screen.y - 53 == 78);

    /* The 25-pixel/850-ms rise is screen-space policy, independent of zoom. */
    const double rise_per_ms = -(25.0 / 850.0);
    CHECK(map_screen_motion_offset(0, rise_per_ms) == 0);
    CHECK(map_screen_motion_offset(425, rise_per_ms) == -12);
    CHECK(map_screen_motion_offset(850, rise_per_ms) == -25);
    CHECK(map_local_anchor_to_screen(17, 23, 50, 48, 24, &screen));
    int risen_y_50 = screen.y + map_screen_motion_offset(850, rise_per_ms);
    CHECK(map_local_anchor_to_screen(17, 23, 400, 48, 24, &screen));
    int risen_y_400 = screen.y + map_screen_motion_offset(850, rise_per_ms);
    CHECK((35 - risen_y_50) == 25);
    CHECK((119 - risen_y_400) == 25);

    CHECK(!map_local_anchor_to_screen(0, 0, MAP_DISPLAY_ZOOM_MIN - 1, 1, 1, &screen));
    CHECK(!map_local_anchor_to_screen(0, 0, MAP_DISPLAY_ZOOM_MAX + 1, 1, 1, &screen));
    CHECK(!map_local_anchor_to_screen(0, 0, 100, 1, 1, NULL));
    CHECK(!map_local_anchor_to_screen(INT_MAX, 0, 400, INT_MAX, 0, &screen));
    return EXIT_SUCCESS;
}
