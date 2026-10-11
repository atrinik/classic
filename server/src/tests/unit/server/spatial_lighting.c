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

#include <global.h>

#include <arch.h>
#include <celestial_structure.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <light.h>
#include <map.h>
#include <object.h>
#include <region.h>
#include <spatial.h>
#include <time.h>

static unsigned long saved_todtick;

static void spatial_lighting_setup(void) {
    check_test_setup();
    saved_todtick = todtick;
}

static void spatial_lighting_teardown(void) {
    todtick = saved_todtick;
    check_test_teardown();
}

static mapstruct *open_fixture(int width, int height) {
    mapstruct *map = get_empty_map(width, height);
    map->celestial_schema = 1;
    map->celestial_schema_seen = true;
    map->celestial_sky_above = CELESTIAL_SKY_OPEN;
    map->celestial_sky_seen = true;
    map->celestial_v1_header_seen = true;
    map->celestial_width_seen = true;
    map->celestial_height_seen = true;
    map->region = region_world();
    return map;
}

static void link_stack(mapstruct *lower, mapstruct *upper) {
    FREE_AND_COPY_HASH(lower->path, "/spatial-light/lower");
    FREE_AND_COPY_HASH(upper->path, "/spatial-light/upper");
    FREE_AND_COPY_HASH(lower->tile_path[TILED_UP], upper->path);
    FREE_AND_COPY_HASH(upper->tile_path[TILED_DOWN], lower->path);
    lower->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = lower;
    lower->celestial_sky_above = CELESTIAL_SKY_LINKED;
    lower->celestial_boundary[TILED_UP] = CELESTIAL_BOUNDARY_CONTINUOUS;
    upper->celestial_boundary[TILED_DOWN] = CELESTIAL_BOUNDARY_CONTINUOUS;
}

static object *wall_at(mapstruct *map, int x, int y, const char *transmission) {
    object *wall = arch_get("wall_wood_1");
    ck_assert_ptr_nonnull(wall);
    wall->x = x;
    wall->y = y;
    if (transmission != NULL) {
        object_set_value(wall, "celestial_transmission", transmission, 1);
    }
    ck_assert_ptr_nonnull(object_insert_map(wall, map, NULL, 0));
    return wall;
}

static void floor_at(mapstruct *map, int x, int y) {
    object *floor = arch_get("roof_thatch");
    ck_assert_ptr_nonnull(floor);
    floor->x = x;
    floor->y = y;
    ck_assert_ptr_nonnull(object_insert_map(floor, map, NULL, 0));
    ck_assert(object_is_roof_surface(floor));
}

static void colored_source_at(mapstruct *map, int x, int y, uint32_t color) {
    object *source = arch_get("letter");
    ck_assert_ptr_nonnull(source);
    source->x = x;
    source->y = y;
    source->glow_radius = 13;
    source->light_color = color;
    ck_assert_ptr_nonnull(object_insert_map(source, map, NULL, 0));
}

START_TEST(test_solar_and_lunar_elevation_change_actual_open_field_radiance) {
    mapstruct *map = open_fixture(3, 3);
    MapSpace *sample = GET_MAP_SPACE_PTR(map, 1, 1);

    ck_assert(celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 12));
    int32_t noon = sample->celestial_light_value;
    ck_assert_int_eq(noon, 1281);

    ck_assert(celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 7));
    int32_t low_sun = sample->celestial_light_value;
    ck_assert_int_eq(low_sun, 378);

    ck_assert(celestial_light_rebuild(map, HOURS_PER_MONTH / 2));
    int32_t moon = sample->celestial_light_value;
    ck_assert_int_eq(moon, 22);
    ck_assert_int_gt(noon, low_sun);
    ck_assert_int_gt(low_sun, moon);
}
END_TEST

START_TEST(test_colored_and_negative_sources_change_aggregate_radiance_without_hue_leak) {
    mapstruct *map = open_fixture(3, 3);
    MapSpace *space = GET_MAP_SPACE_PTR(map, 1, 1);
    uint16_t scalar;
    uint16_t rgb[3];

    colored_source_at(map, 1, 1, UINT32_C(0xff0000));
    int positive = map_get_darkness(map, 1, 1, NULL);
    light_radiance_from_raw(space, positive, &scalar, rgb);
    ck_assert_uint_gt(rgb[0], rgb[1]);
    ck_assert_uint_gt(rgb[0], rgb[2]);

    adjust_light_source(map, 1, 1, -13);
    int negative = map_get_darkness(map, 1, 1, NULL);
    uint16_t negative_rgb[3];
    light_radiance_from_raw(space, negative, &scalar, negative_rgb);
    ck_assert_int_lt(negative, positive);
    ck_assert_uint_lt(negative_rgb[0], rgb[0]);
    ck_assert_uint_le(negative_rgb[1], rgb[1]);
    ck_assert_uint_le(negative_rgb[2], rgb[2]);
}
END_TEST

START_TEST(test_closed_upper_roof_shadows_but_open_floorhole_lights_both_levels) {
    mapstruct *lower = open_fixture(3, 3);
    mapstruct *upper = open_fixture(3, 3);
    link_stack(lower, upper);

    ck_assert(celestial_light_rebuild(lower, 5 * HOURS_PER_MONTH + 12));
    int32_t open_lower = GET_MAP_SPACE_PTR(lower, 1, 1)->celestial_light_value;
    int32_t open_upper = GET_MAP_SPACE_PTR(upper, 1, 1)->celestial_light_value;
    ck_assert_int_gt(open_lower, 0);
    ck_assert_int_eq(open_lower, open_upper);

    /* A vertical opaque wall on the upper level has no downward floor face. */
    wall_at(upper, 1, 1, NULL);
    celestial_light_invalidate(lower);
    ck_assert(celestial_light_rebuild(lower, 5 * HOURS_PER_MONTH + 12));
    ck_assert_int_eq(GET_MAP_SPACE_PTR(lower, 1, 1)->celestial_light_value, open_lower);

    floor_at(upper, 1, 1);
    celestial_light_invalidate(lower);
    ck_assert(celestial_light_rebuild(lower, 5 * HOURS_PER_MONTH + 12));
    ck_assert_int_eq(GET_MAP_SPACE_PTR(lower, 1, 1)->celestial_light_value, 0);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(upper, 1, 1)->celestial_light_value, 0);
}
END_TEST

START_TEST(test_light_faces_preserve_authored_transmission_coefficients) {
    mapstruct *map = open_fixture(3, 3);
    spatial_cell cell = {.map = map, .x = 1, .y = 1};

    object *opaque = wall_at(map, 1, 1, NULL);
    ck_assert_uint_eq(spatial_cell_light_face(&cell, 1, 0, 0, NULL), 0);

    object_remove(opaque, 0);
    object_destroy(opaque);
    object *glass = wall_at(map, 1, 1, "glass");
    ck_assert_uint_eq(spatial_cell_light_face(&cell, 1, 0, 0, NULL), 192);
    object_set_value(glass, "celestial_transmission", "grate", 1);
    ck_assert_uint_eq(spatial_cell_light_face(&cell, 1, 0, 0, NULL), 224);
    object_set_value(glass, "celestial_transmission", "open", 1);
    ck_assert_uint_eq(spatial_cell_light_face(&cell, 1, 0, 0, NULL), 256);
}
END_TEST

START_TEST(test_corner_crossing_uses_the_most_occluding_face) {
    mapstruct *map = open_fixture(3, 3);
    wall_at(map, 0, 0, "glass");
    wall_at(map, 0, 0, "grate");
    spatial_cursor cursor;
    spatial_event event;

    ck_assert(spatial_ray_init(&cursor, map, 0, 0, 1, 1, 0));
    ck_assert_int_eq(spatial_ray_next(&cursor, &event), 1);
    ck_assert_uint_eq(spatial_event_light(&event), 192);
}
END_TEST

START_TEST(test_camera_cutaway_surface_has_no_independent_light_authority) {
    mapstruct *map = open_fixture(3, 3);
    spatial_cell cell = {.map = map, .x = 1, .y = 1};
    floor_at(map, 1, 1);

    ck_assert_uint_eq(spatial_cell_light_face(&cell, 0, 0, -1, NULL), 0);
    /* A roof is also the camera cutaway surface.  Its shared structural face,
     * rather than any viewer/camera state, is the only lighting authority. */
    ck_assert(object_is_roof_surface(GET_MAP_OB(map, 1, 1)));
    ck_assert_uint_eq(spatial_cell_light_face(&cell, 0, 0, -1, NULL), 0);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("spatial_lighting");
    TCase *tc = tcase_create("Core");
    tcase_add_unchecked_fixture(tc, check_setup, check_teardown);
    tcase_add_checked_fixture(tc, spatial_lighting_setup, spatial_lighting_teardown);
    suite_add_tcase(s, tc);
    tcase_add_test(tc, test_solar_and_lunar_elevation_change_actual_open_field_radiance);
    tcase_add_test(tc, test_colored_and_negative_sources_change_aggregate_radiance_without_hue_leak);
    tcase_add_test(tc, test_closed_upper_roof_shadows_but_open_floorhole_lights_both_levels);
    tcase_add_test(tc, test_light_faces_preserve_authored_transmission_coefficients);
    tcase_add_test(tc, test_corner_crossing_uses_the_most_occluding_face);
    tcase_add_test(tc, test_camera_cutaway_surface_has_no_independent_light_authority);
    return s;
}

void check_server_spatial_lighting(void) {
    check_run_suite(suite(), __FILE__);
}
