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

START_TEST(test_light_material_ties_and_sequential_events) {
    mapstruct map;
    MapSpace spaces[16];
    fixture(&map, spaces);
    object glass = {0}, grate = {0};
    ck_assert(object_set_value(&glass, "celestial_transmission", "glass", true));
    ck_assert(object_set_value(&grate, "celestial_transmission", "grate", true));
    spaces[1].first = &glass;
    spaces[4].first = &grate;
    spatial_cursor cursor;
    spatial_event event;
    ck_assert(spatial_ray_init(&cursor, &map, 0, 0, 1, 1, 0));
    ck_assert_int_eq(spatial_ray_next(&cursor, &event), 1);
    ck_assert_uint_eq(spatial_event_light(&event), 192);
    ck_assert_int_eq(spatial_light_attenuate(101, spatial_event_light(&event)), 76);
    spaces[4].first = NULL;
    spaces[2].first = &grate;
    ck_assert(spatial_ray_init(&cursor, &map, 0, 0, 3, 0, 0));
    int32_t value = 101;
    int result;
    while ((result = spatial_ray_next(&cursor, &event)) == 1) {
        value = spatial_light_attenuate(value, spatial_event_light(&event));
    }
    ck_assert_int_eq(result, 0);
    ck_assert_int_eq(value, 67);
    object_free_key_values(&glass);
    object_free_key_values(&grate);
}
END_TEST

START_TEST(test_light_virtual_cover_and_aperture) {
    mapstruct lower, upper;
    MapSpace ls[16], us[16];
    fixture(&lower, ls); fixture(&upper, us);
    link_maps(&lower, &upper, TILED_UP);
    celestial_rectangle_t rectangle = {
        .type = CELESTIAL_RECT_SKY_COVERED, .width = 4, .height = 4,
    };
    lower.celestial_rectangles = &rectangle;
    lower.celestial_rectangle_count = 1;
    spatial_face face = {
        .from = {.map = &lower, .x = 1, .y = 1},
        .to = {.map = &upper, .x = 1, .y = 1, .z = 1},
        .normal = {0, 0, 1},
    };
    ck_assert_uint_eq(spatial_face_light(&face, NULL), 0);
    rectangle.type = CELESTIAL_RECT_SKY_OPEN;
    ck_assert_uint_eq(spatial_face_light(&face, NULL), 256);
    object floor = {0};
    SET_FLAG(&floor, FLAG_IS_FLOOR);
    us[5].first = &floor;
    ck_assert_uint_eq(spatial_face_light(&face, NULL), 0);
    ck_assert_uint_eq(spatial_cell_light_face(&face.from, 1, 0, 0, NULL), 256);
}
END_TEST

START_TEST(test_offset_stays_in_nearest_frontier) {
    mapstruct a, b, remote, far, target;
    MapSpace as[16], bs[16], rs[16], fs[16], ts[16];
    fixture(&a, as); fixture(&b, bs); fixture(&remote, rs);
    fixture(&far, fs); fixture(&target, ts);
    link_maps(&a, &b, TILED_EAST);
    link_maps(&a, &remote, TILED_WEST);
    link_maps(&remote, &far, TILED_WEST);
    far.tile_map[TILED_UP] = &far;
    far.tile_map[TILED_DOWN] = &far;
    int dx, dy, dz;
    ck_assert(spatial_offset(&a, &b, &dx, &dy, &dz));
    ck_assert_int_eq(dx, 4);
    link_maps(&b, &target, TILED_EAST);
    ck_assert(spatial_offset(&a, &target, &dx, &dy, &dz));
    ck_assert_int_eq(dx, 8);
    ck_assert_int_eq(dy, 0);
    ck_assert_int_eq(dz, 0);
}
END_TEST

START_TEST(test_offset_shortest_paths_detect_ambiguity) {
    mapstruct a, east, north, target;
    MapSpace as[16], es[16], ns[16], ts[16];
    fixture(&a, as); fixture(&east, es); fixture(&north, ns); fixture(&target, ts);
    link_maps(&a, &east, TILED_EAST);
    link_maps(&a, &north, TILED_NORTH);
    link_maps(&east, &target, TILED_NORTH);
    link_maps(&north, &target, TILED_EAST);
    int dx, dy, dz;
    ck_assert(spatial_offset(&a, &target, &dx, &dy, &dz));
    ck_assert_int_eq(dx, 4);
    ck_assert_int_eq(dy, -4);
    north.width = 3;
    ck_assert(!spatial_offset(&a, &target, &dx, &dy, &dz));
    /* A shortest direct diagonal cannot hide the inconsistent local diamond. */
    link_maps(&a, &target, TILED_NORTHEAST);
    ck_assert(!spatial_offset(&a, &target, &dx, &dy, &dz));
    spatial_cell cell;
    ck_assert(!spatial_resolve(&a, 4, -1, 0, &cell));
    north.width = 4;
    ck_assert(spatial_offset(&a, &target, &dx, &dy, &dz));
    ck_assert(spatial_resolve(&a, 4, -1, 0, &cell));
    ck_assert_ptr_eq(cell.map, &target);
}
END_TEST

START_TEST(test_offset_nearest_route_checks_nested_local_diamond) {
    mapstruct origin, a, east, north, target;
    MapSpace os[16], as[16], es[16], ns[16], ts[16];
    fixture(&origin, os); fixture(&a, as); fixture(&east, es);
    fixture(&north, ns); fixture(&target, ts);
    link_maps(&origin, &a, TILED_EAST);
    link_maps(&a, &east, TILED_EAST);
    link_maps(&a, &north, TILED_NORTH);
    link_maps(&east, &target, TILED_NORTH);
    link_maps(&north, &target, TILED_EAST);
    link_maps(&a, &target, TILED_NORTHEAST);
    int dx, dy, dz;
    ck_assert(spatial_offset(&origin, &target, &dx, &dy, &dz));
    ck_assert_int_eq(dx, 8);
    ck_assert_int_eq(dy, -4);
    north.width = 3;
    ck_assert(!spatial_offset(&origin, &target, &dx, &dy, &dz));
}
END_TEST

typedef struct observation {
    const mapstruct *maps[16];
    size_t count;
} observation;

static void observe_map(const mapstruct *map, void *context) {
    observation *seen = context;
    for (size_t i = 0; i < seen->count; i++) {
        if (seen->maps[i] == map) {
            return;
        }
    }
    ck_assert_uint_lt(seen->count, arraysize(seen->maps));
    seen->maps[seen->count++] = map;
}

static bool observed(const observation *seen, const mapstruct *map) {
    for (size_t i = 0; i < seen->count; i++) {
        if (seen->maps[i] == map) {
            return true;
        }
    }
    return false;
}

START_TEST(test_observer_scope_and_failed_dependencies) {
    mapstruct a, east, upper;
    MapSpace as[16], es[16], us[16];
    fixture(&a, as); fixture(&east, es); fixture(&upper, us);
    link_maps(&a, &east, TILED_EAST);
    link_maps(&a, &upper, TILED_UP);
    east.in_memory = 0;
    observation first = {0}, second = {0};
    spatial_observer previous = spatial_observer_exchange(
        (spatial_observer){.visit = observe_map, .context = &first});
    spatial_cell cell;
    ck_assert(!spatial_resolve(&a, 4, 0, 1, &cell));
    ck_assert(observed(&first, &a));
    ck_assert(observed(&first, &upper));
    ck_assert(observed(&first, &east));
    spatial_observer outer = spatial_observer_exchange(
        (spatial_observer){.visit = observe_map, .context = &second});
    ck_assert(spatial_resolve(&upper, 1, 1, 0, &cell));
    ck_assert(observed(&second, &upper));
    spatial_observer_exchange(outer);
    first.count = 0;
    ck_assert(spatial_resolve(&a, 1, 1, 0, &cell));
    ck_assert(observed(&first, &a));
    spatial_observer_exchange(previous);
    first.count = second.count = 0;
    ck_assert(spatial_resolve(&a, 1, 1, 0, &cell));
    ck_assert_uint_eq(first.count, 0);
    ck_assert_uint_eq(second.count, 0);
}
END_TEST

START_TEST(test_observer_records_conflicting_seam_alternative) {
    mapstruct a, east, north, target;
    MapSpace as[16], es[16], ns[16], ts[16];
    fixture(&a, as); fixture(&east, es); fixture(&north, ns); fixture(&target, ts);
    link_maps(&a, &east, TILED_EAST);
    link_maps(&a, &north, TILED_NORTH);
    link_maps(&east, &target, TILED_NORTH);
    link_maps(&north, &target, TILED_EAST);
    link_maps(&a, &target, TILED_NORTHEAST);
    north.width = 3;
    observation seen = {0};
    spatial_observer previous = spatial_observer_exchange(
        (spatial_observer){.visit = observe_map, .context = &seen});
    spatial_cell cell;
    ck_assert(!spatial_resolve(&a, 4, -1, 0, &cell));
    ck_assert(observed(&seen, &a));
    ck_assert(observed(&seen, &target));
    ck_assert(observed(&seen, &north));
    spatial_observer_exchange(previous);
}
END_TEST

void check_server_spatial(void) {
    toolkit_import(shstr);
    Suite *suite = suite_create("spatial");
    TCase *tc = tcase_create("geometry");
    tcase_add_test(tc, test_supercover_corner_symmetry);
    tcase_add_test(tc, test_floor_planes_and_crossdepth);
    tcase_add_test(tc, test_loaded_seams_and_cycle_bounds);
    tcase_add_test(tc, test_canonical_stack_limit);
    tcase_add_test(tc, test_light_tie_and_sequential_rounding);
    tcase_add_test(tc, test_light_virtual_cover_and_aperture);
    tcase_add_test(tc, test_light_material_ties_and_sequential_events);
    tcase_add_test(tc, test_offset_stays_in_nearest_frontier);
    tcase_add_test(tc, test_offset_shortest_paths_detect_ambiguity);
    tcase_add_test(tc, test_offset_nearest_route_checks_nested_local_diamond);
    tcase_add_test(tc, test_observer_scope_and_failed_dependencies);
    tcase_add_test(tc, test_observer_records_conflicting_seam_alternative);
    suite_add_tcase(suite, tc);
    check_run_suite(suite, __FILE__);
}
