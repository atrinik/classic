/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <global.h>
#include <initialization.h>
#include <server_main.h>
#include <light.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <object.h>

START_TEST(test_light_level_anchors) {
    ck_assert_uint_eq(light_level_from_raw(-1), 0);
    ck_assert_uint_eq(light_level_from_raw(0), 0);
    ck_assert_uint_eq(light_level_from_raw(20), 45);
    ck_assert_uint_eq(light_level_from_raw(40), 80);
    ck_assert_uint_eq(light_level_from_raw(80), 120);
    ck_assert_uint_eq(light_level_from_raw(160), 165);
    ck_assert_uint_eq(light_level_from_raw(320), 215);
    ck_assert_uint_eq(light_level_from_raw(640), 245);
    ck_assert_uint_eq(light_level_from_raw(1280), 255);
    ck_assert_uint_eq(light_level_from_raw(4096), 255);
}
END_TEST

static void link_stacked_maps(mapstruct *lower, mapstruct *upper) {
    lower->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = lower;
}

static void add_light_source(mapstruct *map, int x, int y) {
    object *marker = arch_get("letter");
    marker->x = x;
    marker->y = y;
    object_insert_map(marker, map, NULL, 0);
    adjust_light_source(map, x, y, 13);
}

static object *add_colored_light(mapstruct *map, int x, int y, int radius, uint32_t color) {
    object *source = arch_get("letter");
    source->x = x;
    source->y = y;
    source->glow_radius = radius;
    source->light_color = color;
    return object_insert_map(source, map, NULL, 0);
}

static void set_light_falloff(const char *falloff) {
    snprintf(VS(settings.light_falloff), "%s", falloff);
}

START_TEST(test_radial_light_profile_is_symmetric_monotonic_and_exact) {
    mapstruct *map = get_empty_map(11, 11);
    set_light_falloff("radial");
    adjust_light_source(map, 5, 5, 3);

    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 5, 5)->light_source_value, 160);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 6, 5)->light_source_value, 71);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 6, 6)->light_source_value, 45);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 7, 5)->light_source_value, 18);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 7, 6)->light_source_value, 10);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 7, 7)->light_source_value, 1);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 8, 5)->light_source_value, 0);

    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 4, 5)->light_source_value,
                     GET_MAP_SPACE_PTR(map, 6, 5)->light_source_value);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value,
                     GET_MAP_SPACE_PTR(map, 6, 5)->light_source_value);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 4, 4)->light_source_value,
                     GET_MAP_SPACE_PTR(map, 6, 6)->light_source_value);

    adjust_light_source(map, 5, 5, -3);
    for (int y = 0; y < MAP_HEIGHT(map); y++) {
        for (int x = 0; x < MAP_WIDTH(map); x++) {
            ck_assert_int_eq(GET_MAP_SPACE_PTR(map, x, y)->light_source_value, 0);
        }
    }

    adjust_light_source(map, 5, 5, -3);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 5, 5)->light_source_value, -160);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 6, 6)->light_source_value, -45);
    adjust_light_source(map, 5, 5, 3);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 5, 5)->light_source_value, 0);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 6, 6)->light_source_value, 0);
}
END_TEST

START_TEST(test_legacy_light_falloff_remains_available) {
    mapstruct *map = get_empty_map(9, 9);
    set_light_falloff("legacy");
    adjust_light_source(map, 4, 4, 3);

    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 4, 4)->light_source_value, 160);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value, 80);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 5, 5)->light_source_value, 40);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 6, 4)->light_source_value, 40);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 6, 5)->light_source_value, 0);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 6, 6)->light_source_value, 0);

    adjust_light_source(map, 4, 4, -3);
    set_light_falloff("radial");
}
END_TEST

START_TEST(test_colored_radial_light_removal_restores_whole_field) {
    mapstruct *map = get_empty_map(11, 11);
    int baseline[11 * 11];
    set_light_falloff("radial");
    adjust_light_source(map, 5, 5, -3);
    for (int y = 0; y < MAP_HEIGHT(map); y++) {
        for (int x = 0; x < MAP_WIDTH(map); x++) {
            baseline[y * MAP_WIDTH(map) + x] = GET_MAP_SPACE_PTR(map, x, y)->light_source_value;
        }
    }

    adjust_light_source_color(map, 5, 5, 3, UINT32_C(0x40a0ff), 1);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 7, 6)->light_source_positive_value, 0);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 7, 6)->light_source_color_weight, 0);
    adjust_light_source_color(map, 5, 5, 3, UINT32_C(0x40a0ff), -1);

    for (int y = 0; y < MAP_HEIGHT(map); y++) {
        for (int x = 0; x < MAP_WIDTH(map); x++) {
            MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
            ck_assert_int_eq(space->light_source_value, baseline[y * MAP_WIDTH(map) + x]);
            ck_assert_int_eq(space->light_source_positive_value, 0);
            ck_assert_int_eq(space->light_source_color[0], 0);
            ck_assert_int_eq(space->light_source_color[1], 0);
            ck_assert_int_eq(space->light_source_color[2], 0);
            ck_assert_int_eq(space->light_source_color_weight, 0);
        }
    }
    adjust_light_source(map, 5, 5, 3);
}
END_TEST

static uint16_t test_raw_to_radiance(int raw) {
    if (raw <= 0) {
        return 0;
    }
    if (raw >= 40959) {
        return UINT16_MAX;
    }
    return (uint16_t)((raw * 8 + 2) / 5);
}

static void test_light_radiance(const MapSpace *space, int raw, uint16_t radiance[3]) {
    uint16_t scalar;
    light_radiance_from_raw(space, raw, &scalar, radiance);
}

START_TEST(test_light_color_parser_is_exact) {
    uint32_t color = 0;
    ck_assert(light_color_parse("12aBcF", &color));
    ck_assert_uint_eq(color, UINT32_C(0x12abcf));
    ck_assert(!light_color_parse(NULL, &color));
    ck_assert(!light_color_parse("fff", &color));
    ck_assert(!light_color_parse("#ffffff", &color));
    ck_assert(!light_color_parse("ffffff00", &color));
    ck_assert(!light_color_parse("fffffg", &color));
}
END_TEST

START_TEST(test_radiance_resolver_preserves_linear_warm_and_cool_daylight) {
    MapSpace space = {0};
    uint16_t scalar;
    uint16_t radiance[3];

    light_radiance_from_raw(&space, 1280, &scalar, radiance);
    ck_assert_uint_eq(scalar, 2048);
    ck_assert_uint_eq(radiance[0], 2048);
    ck_assert_uint_eq(radiance[1], 2048);
    ck_assert_uint_eq(radiance[2], 2048);

    space.light_source_positive_value = 80;
    space.light_source_color_weight = INT64_C(80) * UINT16_MAX;
    space.light_source_color[0] = INT64_C(80) * UINT16_MAX;
    space.light_source_color[1] = INT64_C(80) * 7666;
    space.light_source_color[2] = INT64_C(80) * 1937;
    light_radiance_from_raw(&space, 1360, &scalar, radiance);
    ck_assert_uint_eq(scalar, 2176);
    ck_assert_uint_eq(radiance[0], 2176);
    ck_assert_uint_eq(radiance[1], 2062);
    ck_assert_uint_eq(radiance[2], 2051);

    space.light_source_positive_value = 320;
    space.light_source_color_weight = INT64_C(320) * UINT16_MAX;
    space.light_source_color[0] = INT64_C(320) * 7666;
    space.light_source_color[1] = INT64_C(320) * 41337;
    space.light_source_color[2] = INT64_C(320) * UINT16_MAX;
    light_radiance_from_raw(&space, 1600, &scalar, radiance);
    ck_assert_uint_eq(scalar, 2560);
    ck_assert_uint_eq(radiance[0], 2107);
    ck_assert_uint_eq(radiance[1], 2371);
    ck_assert_uint_eq(radiance[2], 2560);

    memset(&space, 0, sizeof(space));
    light_radiance_from_raw(&space, 40960, &scalar, radiance);
    ck_assert_uint_eq(scalar, UINT16_MAX);
    ck_assert_uint_eq(radiance[0], UINT16_MAX);
    ck_assert_uint_eq(radiance[1], UINT16_MAX);
    ck_assert_uint_eq(radiance[2], UINT16_MAX);

    memset(&space, 0, sizeof(space));
    light_radiance_from_raw(&space, -1, &scalar, radiance);
    ck_assert_uint_eq(scalar, 0);
    ck_assert_uint_eq(radiance[0], 0);
    ck_assert_uint_eq(radiance[1], 0);
    ck_assert_uint_eq(radiance[2], 0);

    space.light_source_positive_value = 20479;
    space.light_source_color_weight = INT64_C(20479) * UINT16_MAX;
    space.light_source_color[0] = INT64_C(61439) * UINT16_MAX;
    space.light_source_color[1] = INT64_C(20479) * UINT16_MAX;
    space.light_source_color[2] = 0;
    light_radiance_from_raw(&space, 40959, &scalar, radiance);
    ck_assert_uint_eq(scalar, UINT16_MAX);
    ck_assert_uint_eq(radiance[0], UINT16_MAX);
    ck_assert_uint_eq(radiance[1], 32768);
    ck_assert_uint_eq(radiance[2], 16384);
}
END_TEST

START_TEST(test_colored_lights_add_remove_and_order_are_exact) {
    mapstruct *first = get_empty_map(9, 9);
    mapstruct *second = get_empty_map(9, 9);
    object *red = add_colored_light(first, 4, 4, 1, UINT32_C(0xff0000));
    uint16_t red_only[3];
    MapSpace *space = GET_MAP_SPACE_PTR(first, 4, 4);
    test_light_radiance(space, space->light_source_value, red_only);
    ck_assert_uint_gt(red_only[0], 0);
    ck_assert_uint_eq(red_only[1], 0);
    ck_assert_uint_eq(red_only[2], 0);

    object *blue = add_colored_light(first, 4, 4, 1, UINT32_C(0x0000ff));
    uint16_t red_blue[3];
    test_light_radiance(space, space->light_source_value, red_blue);
    ck_assert_uint_eq(red_blue[0], red_blue[2]);
    ck_assert_uint_gt(red_blue[0], 0);
    ck_assert_uint_eq(red_blue[1], 0);

    add_colored_light(second, 4, 4, 1, UINT32_C(0x0000ff));
    add_colored_light(second, 4, 4, 1, UINT32_C(0xff0000));
    uint16_t reverse[3];
    MapSpace *reverse_space = GET_MAP_SPACE_PTR(second, 4, 4);
    test_light_radiance(reverse_space, reverse_space->light_source_value, reverse);
    ck_assert_mem_eq(red_blue, reverse, sizeof(red_blue));

    object_remove(blue, 0);
    uint16_t restored[3];
    test_light_radiance(space, space->light_source_value, restored);
    ck_assert_mem_eq(red_only, restored, sizeof(red_only));
    object_destroy(blue);
    object_remove(red, 0);
    ck_assert_int_eq(space->light_source_value, 0);
    ck_assert_int_eq(space->light_source_color_weight, 0);
    object_destroy(red);
}
END_TEST

START_TEST(test_colored_lights_blend_green_yellow_and_capped_same_cell) {
    mapstruct *green_map = get_empty_map(9, 9);
    add_colored_light(green_map, 4, 4, 13, UINT32_C(0x00ff00));
    uint16_t green[3];
    MapSpace *space = GET_MAP_SPACE_PTR(green_map, 4, 4);
    test_light_radiance(space, space->light_source_value, green);
    ck_assert_uint_eq(green[0], 0);
    ck_assert_uint_gt(green[1], 0);
    ck_assert_uint_eq(green[2], 0);

    mapstruct *yellow_map = get_empty_map(9, 9);
    add_colored_light(yellow_map, 4, 4, 13, UINT32_C(0xff0000));
    add_colored_light(yellow_map, 4, 4, 13, UINT32_C(0x00ff00));
    uint16_t yellow[3];
    space = GET_MAP_SPACE_PTR(yellow_map, 4, 4);
    test_light_radiance(space, space->light_source_value, yellow);
    ck_assert_uint_eq(space->light_source_value, 1280);
    ck_assert_uint_eq(yellow[0], yellow[1]);
    ck_assert_uint_gt(yellow[0], 0);
    ck_assert_uint_eq(yellow[2], 0);

    mapstruct *reverse_map = get_empty_map(9, 9);
    add_colored_light(reverse_map, 4, 4, 13, UINT32_C(0x00ff00));
    add_colored_light(reverse_map, 4, 4, 13, UINT32_C(0xff0000));
    uint16_t reverse[3];
    space = GET_MAP_SPACE_PTR(reverse_map, 4, 4);
    test_light_radiance(space, space->light_source_value, reverse);
    ck_assert_mem_eq(yellow, reverse, sizeof(yellow));

    mapstruct *magenta_map = get_empty_map(9, 9);
    add_colored_light(magenta_map, 4, 4, 13, UINT32_C(0xff0000));
    add_colored_light(magenta_map, 4, 4, 13, UINT32_C(0x0000ff));
    uint16_t magenta[3];
    space = GET_MAP_SPACE_PTR(magenta_map, 4, 4);
    test_light_radiance(space, space->light_source_value, magenta);
    ck_assert_uint_eq(magenta[0], magenta[2]);
    ck_assert_uint_gt(magenta[0], 0);
    ck_assert_uint_eq(magenta[1], 0);

    mapstruct *overlap_map = get_empty_map(9, 9);
    add_colored_light(overlap_map, 3, 4, 13, UINT32_C(0xff0000));
    add_colored_light(overlap_map, 5, 4, 13, UINT32_C(0x00ff00));
    space = GET_MAP_SPACE_PTR(overlap_map, 4, 4);
    uint16_t overlap[3];
    test_light_radiance(space, space->light_source_value, overlap);
    ck_assert_uint_eq(overlap[0], overlap[1]);
    ck_assert_uint_gt(overlap[0], 0);
    ck_assert_uint_eq(overlap[2], 0);

    mapstruct *reverse_overlap_map = get_empty_map(9, 9);
    add_colored_light(reverse_overlap_map, 5, 4, 13, UINT32_C(0x00ff00));
    add_colored_light(reverse_overlap_map, 3, 4, 13, UINT32_C(0xff0000));
    space = GET_MAP_SPACE_PTR(reverse_overlap_map, 4, 4);
    uint16_t reverse_overlap[3];
    test_light_radiance(space, space->light_source_value, reverse_overlap);
    ck_assert_mem_eq(overlap, reverse_overlap, sizeof(overlap));
}
END_TEST

START_TEST(test_neutral_and_darkness_sources_remain_achromatic) {
    mapstruct *map = get_empty_map(9, 9);
    object *white = add_colored_light(map, 4, 4, 1, LIGHT_COLOR_WHITE);
    MapSpace *space = GET_MAP_SPACE_PTR(map, 4, 4);
    uint16_t levels[3];
    test_light_radiance(space, space->light_source_value, levels);
    ck_assert_uint_eq(levels[0], test_raw_to_radiance(space->light_source_value));
    ck_assert_uint_eq(levels[0], levels[1]);
    ck_assert_uint_eq(levels[1], levels[2]);

    object *dark = add_colored_light(map, 4, 4, -1, UINT32_C(0xff0000));
    ck_assert_int_eq(space->light_source_color_weight, INT64_C(40) * UINT16_MAX);
    test_light_radiance(space, space->light_source_value, levels);
    ck_assert_uint_eq(levels[0], levels[1]);
    ck_assert_uint_eq(levels[1], levels[2]);

    object_remove(dark, 0);
    object_destroy(dark);
    object_remove(white, 0);
    object_destroy(white);
}
END_TEST

START_TEST(test_darkness_subtracts_achromatically_from_colored_light) {
    mapstruct *map = get_empty_map(9, 9);
    MapSpace *space = GET_MAP_SPACE_PTR(map, 4, 4);
    space->light_value = 40;
    object *red = add_colored_light(map, 4, 4, 1, UINT32_C(0xff0000));
    object *dark = add_colored_light(map, 4, 4, -1, LIGHT_COLOR_WHITE);
    ck_assert_int_eq(space->light_source_value, 0);
    ck_assert_int_eq(space->light_source_positive_value, 40);

    uint16_t levels[3];
    test_light_radiance(space, space->light_value + space->light_source_value, levels);
    ck_assert_uint_eq(levels[0], test_raw_to_radiance(40));
    ck_assert_uint_eq(levels[1], 0);
    ck_assert_uint_eq(levels[2], 0);

    object_remove(dark, 0);
    test_light_radiance(space, space->light_value + space->light_source_value, levels);
    ck_assert_uint_eq(levels[0], test_raw_to_radiance(80));
    ck_assert_uint_eq(levels[1], test_raw_to_radiance(40));
    ck_assert_uint_eq(levels[2], test_raw_to_radiance(40));
    object_destroy(dark);

    object_remove(red, 0);
    test_light_radiance(space, space->light_value + space->light_source_value, levels);
    ck_assert_uint_eq(levels[0], test_raw_to_radiance(40));
    ck_assert_uint_eq(levels[0], levels[1]);
    ck_assert_uint_eq(levels[1], levels[2]);
    object_destroy(red);

    mapstruct *reverse = get_empty_map(9, 9);
    add_colored_light(reverse, 4, 4, -1, LIGHT_COLOR_WHITE);
    add_colored_light(reverse, 4, 4, 1, UINT32_C(0xff0000));
    MapSpace *reverse_space = GET_MAP_SPACE_PTR(reverse, 4, 4);
    reverse_space->light_value = 40;
    uint16_t reverse_levels[3];
    test_light_radiance(reverse_space,
                          reverse_space->light_value + reverse_space->light_source_value,
                          reverse_levels);
    ck_assert_uint_eq(reverse_levels[0], test_raw_to_radiance(40));
    ck_assert_uint_eq(reverse_levels[1], 0);
    ck_assert_uint_eq(reverse_levels[2], 0);

    mapstruct *overlap = get_empty_map(9, 9);
    add_colored_light(overlap, 3, 4, 1, UINT32_C(0xff0000));
    add_colored_light(overlap, 5, 4, -1, LIGHT_COLOR_WHITE);
    MapSpace *overlap_space = GET_MAP_SPACE_PTR(overlap, 4, 4);
    overlap_space->light_value = 40;
    test_light_radiance(overlap_space,
                          overlap_space->light_value + overlap_space->light_source_value,
                          levels);
    ck_assert_uint_gt(levels[0], levels[1]);
    ck_assert_uint_eq(levels[1], levels[2]);
}
END_TEST

START_TEST(test_colored_light_recalculation_and_linked_depth_are_stable) {
    mapstruct *lower = get_empty_map(9, 9);
    mapstruct *upper = get_empty_map(9, 9);
    link_stacked_maps(lower, upper);
    add_colored_light(lower, 4, 4, 13, UINT32_C(0x00ff00));
    MapSpace *space = GET_MAP_SPACE_PTR(upper, 4, 4);
    int32_t scalar = space->light_source_value;
    int64_t green = space->light_source_color[1];
    ck_assert_int_gt(scalar, 0);
    ck_assert_int_gt(green, 0);

    recalculate_light_sources(lower);
    ck_assert_int_eq(space->light_source_value, scalar);
    ck_assert_int_eq(space->light_source_color[0], 0);
    ck_assert_int_eq(space->light_source_color[1], green);
    ck_assert_int_eq(space->light_source_color[2], 0);
}
END_TEST

START_TEST(test_light_mask_propagates_in_three_dimensions) {
    mapstruct *lower = get_empty_map(9, 9);
    mapstruct *upper = get_empty_map(9, 9);
    mapstruct *top = get_empty_map(9, 9);
    link_stacked_maps(lower, upper);
    link_stacked_maps(upper, top);

    add_light_source(lower, 4, 4);

    ck_assert_int_eq(GET_MAP_SPACE_PTR(lower, 4, 4)->light_source_value, 1280);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(upper, 4, 4)->light_source_value, 720);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(upper, 5, 4)->light_source_value, 535);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(top, 4, 4)->light_source_value, 320);
}
END_TEST

START_TEST(test_radial_light_crosses_horizontal_map_boundaries) {
    mapstruct *west = get_empty_map(7, 7);
    mapstruct *east = get_empty_map(7, 7);
    west->tile_path[TILED_EAST] = add_string("/east");
    east->tile_path[TILED_WEST] = add_string("/west");
    west->tile_map[TILED_EAST] = east;
    east->tile_map[TILED_WEST] = west;

    add_light_source(west, 6, 3);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(east, 0, 3)->light_source_value, 720);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(east, 1, 3)->light_source_value, 320);
}
END_TEST

START_TEST(test_light_mask_is_blocked_by_floors_in_both_directions) {
    mapstruct *lower = get_empty_map(9, 9);
    mapstruct *upper = get_empty_map(9, 9);
    link_stacked_maps(lower, upper);

    object *floor = arch_get("water_still");
    floor->x = 4;
    floor->y = 4;
    object_insert_map(floor, upper, NULL, 0);

    add_light_source(lower, 4, 4);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(upper, 4, 4)->light_source_value, 0);

    adjust_light_source(lower, 4, 4, -13);
    add_light_source(upper, 4, 4);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(lower, 4, 4)->light_source_value, 0);
}
END_TEST

START_TEST(test_light_mask_lights_exposed_upper_wall_face) {
    mapstruct *lower = get_empty_map(9, 9);
    mapstruct *upper = get_empty_map(9, 9);
    link_stacked_maps(lower, upper);

    object *floor = arch_get("water_still");
    floor->x = 5;
    floor->y = 4;
    object_insert_map(floor, upper, NULL, 0);
    GET_MAP_SPACE_PTR(upper, 5, 4)->flags |= P_BLOCKSVIEW;

    add_light_source(lower, 4, 4);

    ck_assert_int_eq(GET_MAP_SPACE_PTR(upper, 5, 4)->light_source_value, 535);
}
END_TEST

START_TEST(test_light_mask_recalculates_around_opaque_cells) {
    mapstruct *map = get_empty_map(9, 9);
    add_light_source(map, 3, 4);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value, 0);

    GET_MAP_SPACE_PTR(map, 4, 4)->flags |= P_BLOCKSVIEW;
    recalculate_light_sources(map);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 4, 4)->light_source_value, 0);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value, 0);

    GET_MAP_SPACE_PTR(map, 4, 4)->flags &= ~P_BLOCKSVIEW;
    recalculate_light_sources(map);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value, 0);
}
END_TEST

START_TEST(test_loaded_map_light_check_is_idempotent) {
    mapstruct *map = get_empty_map(9, 9);
    add_light_source(map, 4, 4);
    int expected = GET_MAP_SPACE_PTR(map, 4, 4)->light_source_value;

    check_light_source_list(map);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 4, 4)->light_source_value, expected);

    check_light_source_list(map);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 4, 4)->light_source_value, expected);
}
END_TEST

START_TEST(test_remove_light_source_list_accepts_swapped_map) {
    mapstruct swapped = {
        .width = 1,
        .height = 1,
        .in_memory = MAP_SWAPPED,
        .spaces = NULL,
    };

    remove_light_source_list(&swapped);
    ck_assert_ptr_null(swapped.first_light);
}
END_TEST

START_TEST(test_light_level_interpolation) {
    ck_assert_uint_eq(light_level_from_raw(10), 23);
    ck_assert_uint_eq(light_level_from_raw(30), 63);
    ck_assert_uint_eq(light_level_from_raw(60), 100);

    uint8_t previous = light_level_from_raw(0);
    for (int raw_light = 1; raw_light <= 2048; raw_light++) {
        uint8_t level = light_level_from_raw(raw_light);
        ck_assert_uint_ge(level, previous);
        previous = level;
    }
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("light");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);

    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_light_level_anchors);
    tcase_add_test(tc_core, test_light_level_interpolation);
    tcase_add_test(tc_core, test_radial_light_profile_is_symmetric_monotonic_and_exact);
    tcase_add_test(tc_core, test_legacy_light_falloff_remains_available);
    tcase_add_test(tc_core, test_colored_radial_light_removal_restores_whole_field);
    tcase_add_test(tc_core, test_light_color_parser_is_exact);
    tcase_add_test(tc_core, test_radiance_resolver_preserves_linear_warm_and_cool_daylight);
    tcase_add_test(tc_core, test_colored_lights_add_remove_and_order_are_exact);
    tcase_add_test(tc_core, test_colored_lights_blend_green_yellow_and_capped_same_cell);
    tcase_add_test(tc_core, test_neutral_and_darkness_sources_remain_achromatic);
    tcase_add_test(tc_core, test_darkness_subtracts_achromatically_from_colored_light);
    tcase_add_test(tc_core, test_colored_light_recalculation_and_linked_depth_are_stable);
    tcase_add_test(tc_core, test_light_mask_propagates_in_three_dimensions);
    tcase_add_test(tc_core, test_radial_light_crosses_horizontal_map_boundaries);
    tcase_add_test(tc_core, test_light_mask_is_blocked_by_floors_in_both_directions);
    tcase_add_test(tc_core, test_light_mask_lights_exposed_upper_wall_face);
    tcase_add_test(tc_core, test_light_mask_recalculates_around_opaque_cells);
    tcase_add_test(tc_core, test_loaded_map_light_check_is_idempotent);
    tcase_add_test(tc_core, test_remove_light_source_list_accepts_swapped_map);

    return s;
}

void check_server_light(void) {
    check_run_suite(suite(), __FILE__);
}
