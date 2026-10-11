/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <object.h>
#include <spatial.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>

static void fixture(mapstruct *map, MapSpace *spaces) {
    memset(map, 0, sizeof(*map));
    memset(spaces, 0, 16 * sizeof(*spaces));
    map->width = map->height = 4;
    map->spaces = spaces;
    map->in_memory = MAP_IN_MEMORY;
}

static void link_maps(mapstruct *a, mapstruct *b, int direction) {
    a->tile_map[direction] = b;
    b->tile_map[map_tiled_reverse[direction]] = a;
}

START_TEST(test_supercover_corner_symmetry) {
    mapstruct map;
    MapSpace spaces[16];
    fixture(&map, spaces);
    spaces[1].flags = P_BLOCKSVIEW;
    ck_assert(!spatial_ray_clear(&map, 0, 0, 2, 2, 0, SPATIAL_SIGHT, false));
    ck_assert(!spatial_ray_clear(&map, 2, 2, -2, -2, 0, SPATIAL_SIGHT, false));
    spaces[1].flags = 0;
    ck_assert(spatial_ray_clear(&map, 0, 0, 2, 2, 0, SPATIAL_SIGHT, false));
    spatial_cursor cursor;
    spatial_event event;
    ck_assert(spatial_ray_init(&cursor, &map, 0, 0, 2, 2, 0));
    ck_assert_int_eq(spatial_ray_next(&cursor, &event), 1);
    ck_assert_uint_eq(event.cell_count, 3);
    ck_assert_uint_eq(event.face_count, 4);
    ck_assert_uint_eq(event.axes, 3);
    spaces[1].flags = P_NO_PASS;
    ck_assert_uint_eq(spatial_event_blocked_axes(&event, SPATIAL_COMBAT), 1);
}
END_TEST

START_TEST(test_floor_planes_and_crossdepth) {
    mapstruct lower, upper;
    MapSpace low_spaces[16], up_spaces[16];
    fixture(&lower, low_spaces);
    fixture(&upper, up_spaces);
    link_maps(&lower, &upper, TILED_UP);
    ck_assert(spatial_ray_clear(&lower, 1, 1, 0, 0, 1, SPATIAL_SIGHT, false));
    object floor = {0};
    SET_FLAG(&floor, FLAG_IS_FLOOR);
    up_spaces[5].first = &floor;
    ck_assert(!spatial_ray_clear(&lower, 1, 1, 0, 0, 1, SPATIAL_SIGHT, true));
    ck_assert(!spatial_ray_clear(&upper, 1, 1, 0, 0, -1, SPATIAL_COMBAT, true));
    ck_assert(spatial_ray_clear(&upper, 0, 1, 2, 0, 0, SPATIAL_SIGHT, false));
    CLEAR_FLAG(&floor, FLAG_IS_FLOOR);
    floor.layer = LAYER_WALL;
    SET_FLAG(&floor, FLAG_HIDDEN);
    ck_assert(!spatial_ray_clear(&lower, 1, 1, 0, 0, 1, SPATIAL_SIGHT, true));
    spatial_cursor cursor;
    spatial_event event;
    ck_assert(spatial_ray_init(&cursor, &lower, 0, 0, 1, 1, 1));
    ck_assert_int_eq(spatial_ray_next(&cursor, &event), 1);
    ck_assert_uint_eq(event.cell_count, 7);
    ck_assert_uint_eq(event.face_count, 12);
    ck_assert(!spatial_event_clear(&event, SPATIAL_SIGHT, true));
}
END_TEST

START_TEST(test_loaded_seams_and_cycle_bounds) {
    mapstruct a, b, c, d;
    MapSpace as[16], bs[16], cs[16], ds[16];
    fixture(&a, as); fixture(&b, bs); fixture(&c, cs); fixture(&d, ds);
    link_maps(&a, &b, TILED_EAST);
    link_maps(&a, &c, TILED_UP);
    link_maps(&b, &d, TILED_UP);
    link_maps(&c, &d, TILED_EAST);
    spatial_cell cell;
    ck_assert(spatial_resolve(&a, 4, 2, 1, &cell));
    ck_assert_ptr_eq(cell.map, &d);
    ck_assert_int_eq(cell.x, 0);
    int dx, dy, dz;
    ck_assert(spatial_offset(&a, &d, &dx, &dy, &dz));
    ck_assert_int_eq(dx, 4); ck_assert_int_eq(dy, 0); ck_assert_int_eq(dz, 1);
    b.in_memory = 0;
    ck_assert(!spatial_resolve(&a, 4, 2, 0, &cell));
    b.in_memory = MAP_IN_MEMORY;
    c.width = 3;
    ck_assert(!spatial_resolve(&a, 1, 1, 1, &cell));
    c.width = 4;
    c.tile_map[TILED_UP] = &a;
    a.tile_map[TILED_DOWN] = &c;
    ck_assert(!spatial_resolve(&a, 1, 1, 0, &cell));
}
END_TEST

START_TEST(test_canonical_stack_limit) {
    mapstruct maps[14];
    MapSpace spaces[14][16];
    for (int i = 0; i < 14; i++) {
        fixture(&maps[i], spaces[i]);
        if (i > 0 && i < 13) {
            link_maps(&maps[i - 1], &maps[i], TILED_UP);
        }
    }
    spatial_cell cell;
    ck_assert(spatial_resolve(&maps[0], 1, 1, 12, &cell));
    ck_assert_ptr_eq(cell.map, &maps[12]);
    ck_assert(spatial_resolve(&maps[12], 1, 1, -12, &cell));
    link_maps(&maps[12], &maps[13], TILED_UP);
    ck_assert(!spatial_resolve(&maps[0], 1, 1, 1, &cell));
}
END_TEST

START_TEST(test_light_tie_and_sequential_rounding) {
    mapstruct map;
    MapSpace spaces[16];
    fixture(&map, spaces);
    spatial_cursor cursor;
    spatial_event event;
    ck_assert(spatial_ray_init(&cursor, &map, 0, 0, 2, 2, 0));
    ck_assert_int_eq(spatial_ray_next(&cursor, &event), 1);
    ck_assert_uint_eq(spatial_event_light(&event), 256);
    spaces[1].flags = P_BLOCKSVIEW;
    ck_assert_uint_eq(spatial_event_light(&event), 0);
    ck_assert_int_eq(spatial_light_attenuate(101, 192), 76);
    ck_assert_int_eq(spatial_light_attenuate(spatial_light_attenuate(101, 192), 224), 67);
    ck_assert_int_eq(spatial_light_attenuate(101, MIN(192, 224)), 76);
    spatial_cell cell = {.map = &map, .x = 1};
    ck_assert_uint_eq(spatial_cell_light_face(&cell, 0, 0, -1, NULL), 256);
}
END_TEST

void check_server_spatial(void) {
    Suite *suite = suite_create("spatial");
    TCase *tc = tcase_create("geometry");
    tcase_add_test(tc, test_supercover_corner_symmetry);
    tcase_add_test(tc, test_floor_planes_and_crossdepth);
    tcase_add_test(tc, test_loaded_seams_and_cycle_bounds);
    tcase_add_test(tc, test_canonical_stack_limit);
    tcase_add_test(tc, test_light_tie_and_sequential_rounding);
    suite_add_tcase(suite, tc);
    check_run_suite(suite, __FILE__);
}
