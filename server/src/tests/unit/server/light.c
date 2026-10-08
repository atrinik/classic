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
#include <swap.h>

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

static void add_roof_surface(mapstruct *map, int x, int y) {
    object *roof = arch_get("roof_thatch");
    ck_assert_ptr_nonnull(roof);
    roof->x = x;
    roof->y = y;
    ck_assert_ptr_nonnull(object_insert_map(roof, map, NULL, 0));
    ck_assert(object_is_roof_surface(roof));
}

static void
assign_temporary_unique_path(mapstruct *map, char *path, size_t path_size, const char *label) {
    int written = snprintf(path, path_size, "/tmp/atrinik-light-%s-XXXXXX", label);
    ck_assert_int_ge(written, 0);
    ck_assert_uint_lt((size_t)written, path_size);

    int fd = mkstemp(path);
    ck_assert_int_ge(fd, 0);
    ck_assert_int_eq(close(fd), 0);
    ck_assert_int_eq(unlink(path), 0);

    map->map_flags |= MAP_FLAG_UNIQUE;
    FREE_AND_COPY_HASH(map->path, path);
}

typedef struct test_light_snapshot {
    int32_t scalar;
    int32_t positive;
    int64_t color[3];
    int64_t color_weight;
} test_light_snapshot;

static void snapshot_local_light(const mapstruct *map, test_light_snapshot *snapshot) {
    for (int y = 0; y < MAP_HEIGHT(map); y++) {
        for (int x = 0; x < MAP_WIDTH(map); x++) {
            const MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
            size_t index = (size_t)y * MAP_WIDTH(map) + x;
            snapshot[index].scalar = space->light_source_value;
            snapshot[index].positive = space->light_source_positive_value;
            memcpy(snapshot[index].color, space->light_source_color, sizeof(snapshot[index].color));
            snapshot[index].color_weight = space->light_source_color_weight;
        }
    }
}

static void assert_local_light_matches(const mapstruct *map, const test_light_snapshot *snapshot) {
    for (int y = 0; y < MAP_HEIGHT(map); y++) {
        for (int x = 0; x < MAP_WIDTH(map); x++) {
            const MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
            size_t index = (size_t)y * MAP_WIDTH(map) + x;
            ck_assert_int_eq(space->light_source_value, snapshot[index].scalar);
            ck_assert_int_eq(space->light_source_positive_value, snapshot[index].positive);
            ck_assert_mem_eq(space->light_source_color,
                             snapshot[index].color,
                             sizeof(snapshot[index].color));
            ck_assert_int_eq(space->light_source_color_weight, snapshot[index].color_weight);
        }
    }
}

static void link_horizontal_maps(mapstruct *west, mapstruct *east) {
    west->tile_path[TILED_EAST] = add_string("/tests/light-east");
    east->tile_path[TILED_WEST] = add_string("/tests/light-west");
    west->tile_map[TILED_EAST] = east;
    east->tile_map[TILED_WEST] = west;
}

START_TEST(test_rebuild_preserves_incoming_light_from_third_horizontal_map) {
    enum { WIDTH = 7, HEIGHT = 7 };
    mapstruct *maps[3];
    test_light_snapshot expected[3][WIDTH * HEIGHT];
    for (size_t i = 0; i < arraysize(maps); i++) {
        maps[i] = get_empty_map(WIDTH, HEIGHT);
        if (i != 0) {
            link_horizontal_maps(maps[i - 1], maps[i]);
        }
    }
    /* Incremental insertion is the independent replay oracle: none of these
     * source additions uses the rebuild's target/source enumeration. */
    add_colored_light(maps[2], 0, 3, 13, UINT32_C(0xff8040));
    add_colored_light(maps[2], 1, 3, -5, LIGHT_COLOR_WHITE);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(maps[1], 6, 3)->light_source_positive_value, 0);
    for (size_t i = 0; i < arraysize(maps); i++) {
        snapshot_local_light(maps[i], expected[i]);
    }
    recalculate_light_sources(maps[0]);
    recalculate_light_sources(maps[0]);
    for (size_t i = 0; i < arraysize(maps); i++) {
        assert_local_light_matches(maps[i], expected[i]);
    }
}
END_TEST

START_TEST(test_rebuild_matches_incremental_oracle_for_all_resident_sources) {
    enum { COLUMNS = 3, LEVELS = MAP2_MAX_DEPTH + 2, WIDTH = 9, HEIGHT = 9 };
    mapstruct *maps[COLUMNS][LEVELS];
    test_light_snapshot expected[COLUMNS][LEVELS][WIDTH * HEIGHT];
    for (int column = 0; column < COLUMNS; column++) {
        for (int depth = 0; depth < LEVELS; depth++) {
            maps[column][depth] = get_empty_map(WIDTH, HEIGHT);
            if (depth != 0) {
                link_stacked_maps(maps[column][depth - 1], maps[column][depth]);
            }
            if (column != 0) {
                link_horizontal_maps(maps[column - 1][depth], maps[column][depth]);
            }
        }
    }
    for (int column = 0; column < COLUMNS; column++) {
        for (int depth = 0; depth < LEVELS; depth++) {
            add_colored_light(maps[column][depth], column == 2 ? 0 : WIDTH - 1, 3,
                              13, UINT32_C(0x40a0ff));
            /* Same-origin cancellation keeps the scalar mask zero while the
             * positive/RGB origins must remain discoverable. */
            add_colored_light(maps[column][depth], column == 2 ? 0 : WIDTH - 1, 3,
                              -13, LIGHT_COLOR_WHITE);
            add_colored_light(maps[column][depth], 3, 2, -5, LIGHT_COLOR_WHITE);
        }
    }
    for (int column = 0; column < COLUMNS; column++) {
        for (int depth = 0; depth < LEVELS; depth++) {
            snapshot_local_light(maps[column][depth], expected[column][depth]);
        }
    }
    /* Each rebuild includes incoming sources beyond its horizontal and
     * vertical cleared boundaries; untouched maps must remain exact too. */
    for (int column = 0; column < COLUMNS; column++) {
        recalculate_light_sources(maps[column][0]);
        for (int check_column = 0; check_column < COLUMNS; check_column++) {
            for (int depth = 0; depth < LEVELS; depth++) {
                assert_local_light_matches(maps[check_column][depth],
                                           expected[check_column][depth]);
            }
        }
    }
}
END_TEST

START_TEST(test_unchanged_geometry_updates_do_not_rebuild_or_invalidate) {
    mapstruct *map = get_empty_map(9, 9);
    object *floor = arch_get("water_still");
    floor->x = 4;
    floor->y = 4;
    object_insert_map(floor, map, NULL, 0);
    add_colored_light(map, 3, 4, 13, UINT32_C(0xff4000));
    test_light_snapshot expected[9 * 9];
    snapshot_local_light(map, expected);
    uint64_t rebuilds = light_rebuild_count_for_test();
    uint64_t revision = map->celestial_structure_revision;
    for (int i = 0; i < 1000; i++) {
        /* A pre-existing lazy flag must still refresh, but must not turn an
         * equivalent effective geometry into a lighting invalidation. */
        GET_MAP_SPACE_PTR(map, 4, 4)->flags |= P_NEED_UPDATE;
        object_update(floor, UP_OBJ_FLAGS);
        ck_assert(!(GET_MAP_FLAGS(map, 4, 4) & P_NEED_UPDATE));
    }
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
    ck_assert_uint_eq(map->celestial_structure_revision, revision);
    assert_local_light_matches(map, expected);
}
END_TEST

static object *add_light_blocker(mapstruct *map, int x, int y) {
    object *blocker = arch_get("letter");
    SET_FLAG(blocker, FLAG_BLOCKSVIEW);
    blocker->x = x;
    blocker->y = y;
    return object_insert_map(blocker, map, NULL, INS_NO_MERGE);
}

START_TEST(test_geometry_tracks_last_blocker_and_cleared_flag) {
    mapstruct *map = get_empty_map(9, 9);
    add_colored_light(map, 3, 4, 13, UINT32_C(0x00ff00));
    object *first = add_light_blocker(map, 4, 4);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value, 0);
    uint64_t rebuilds = light_rebuild_count_for_test();
    object *second = add_light_blocker(map, 4, 4);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
    object_remove(first, 0);
    object_destroy(first);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
    CLEAR_FLAG(second, FLAG_BLOCKSVIEW);
    object_update(second, UP_OBJ_FLAGS);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value, 0);
    SET_FLAG(second, FLAG_BLOCKSVIEW);
    object_update(second, UP_OBJ_FLAGS);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value, 0);
    rebuilds = light_rebuild_count_for_test();
    object_remove(second, 0);
    object_destroy(second);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value, 0);
}
END_TEST

START_TEST(test_geometry_batches_flush_before_read_and_at_scope_end) {
    mapstruct *map = get_empty_map(9, 9);
    add_colored_light(map, 2, 4, 13, UINT32_C(0x8040ff));
    uint64_t rebuilds = light_rebuild_count_for_test();
    uint64_t revision = map->celestial_structure_revision;
    light_batch_begin();
    light_batch_begin();
    object *first = add_light_blocker(map, 3, 4);
    object *second = add_light_blocker(map, 4, 4);
    light_batch_end();
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
    (void)map_get_darkness(map, 5, 4, NULL);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);
    ck_assert_uint_eq(map->celestial_structure_revision, revision);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value, 0);
    light_batch_end();
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);
    light_batch_begin();
    object_remove(first, 0);
    object_remove(second, 0);
    light_batch_end();
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 2);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 5, 4)->light_source_value, 0);
    object_destroy(first);
    object_destroy(second);
}
END_TEST

START_TEST(test_floor_and_roof_geometry_detects_removal_and_flag_changes) {
    mapstruct *lower = get_empty_map(9, 9);
    mapstruct *upper = get_empty_map(9, 9);
    mapstruct *top = get_empty_map(9, 9);
    link_stacked_maps(lower, upper);
    link_stacked_maps(upper, top);
    add_colored_light(lower, 4, 4, 13, UINT32_C(0xff8000));
    object *floor = arch_get("water_still");
    floor->x = 4;
    floor->y = 4;
    object_insert_map(floor, upper, NULL, 0);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(upper, 4, 4)->light_source_value, 0);
    object *duplicate = arch_get("water_still");
    duplicate->x = 4;
    duplicate->y = 4;
    uint64_t rebuilds = light_rebuild_count_for_test();
    object_insert_map(duplicate, upper, NULL, INS_NO_MERGE);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
    object_remove(floor, 0);
    object_destroy(floor);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
    CLEAR_FLAG(duplicate, FLAG_IS_FLOOR);
    object_update(duplicate, UP_OBJ_FLAGS);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(upper, 4, 4)->light_source_value, 0);
    object_remove(duplicate, 0);
    object_destroy(duplicate);
    object *roof = arch_get("roof_thatch");
    roof->x = 4;
    roof->y = 4;
    object_insert_map(roof, upper, NULL, 0);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(upper, 4, 4)->light_source_value, 0);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(top, 4, 4)->light_source_value, 0);
    object_remove(roof, 0);
    object_destroy(roof);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(top, 4, 4)->light_source_value, 0);
}
END_TEST

START_TEST(test_multipart_flag_update_rebuilds_once_and_removal_withdraws_tail_once) {
    mapstruct *map = get_empty_map(9, 9);
    object *head = add_light_blocker(map, 4, 4);
    object *tail = add_light_blocker(map, 4, 5);
    head->more = tail;
    tail->head = head;
    CLEAR_FLAG(head, FLAG_BLOCKSVIEW);
    CLEAR_FLAG(tail, FLAG_BLOCKSVIEW);
    uint64_t rebuilds = light_rebuild_count_for_test();
    object_update(head, UP_OBJ_FLAGS);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);
    tail->glow_radius = 13;
    tail->light_color = UINT32_C(0xff0080);
    adjust_light_source_color(map, tail->x, tail->y, tail->glow_radius, tail->light_color, 1);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 4, 5)->light_source_value, 0);
    object_remove(head, 0);
    for (int y = 0; y < 9; y++) {
        for (int x = 0; x < 9; x++) {
            MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
            ck_assert_int_eq(space->light_source_value, 0);
            ck_assert_int_eq(space->light_source_positive_value, 0);
            ck_assert_int_eq(space->light_source_color_weight, 0);
        }
    }
    object_destroy(head);
}
END_TEST

START_TEST(test_batch_geometry_and_source_deltas_preserve_all_accumulators) {
    mapstruct *map = get_empty_map(9, 9);
    object *source = add_colored_light(map, 3, 4, 13, UINT32_C(0xff0080));
    object *blocker = add_light_blocker(map, 4, 4);
    light_batch_begin();
    object_remove(blocker, 0);
    object_remove(source, 0);
    light_batch_end();
    object_destroy(blocker);
    object_destroy(source);
    test_light_snapshot empty[9 * 9] = {0};
    assert_local_light_matches(map, empty);

    add_colored_light(map, 3, 4, 1, UINT32_C(0x00ff00));
    blocker = add_light_blocker(map, 4, 4);
    light_batch_begin();
    object_remove(blocker, 0);
    add_colored_light(map, 3, 4, 13, UINT32_C(0xff0080));
    light_batch_end();
    object_destroy(blocker);
    test_light_snapshot result[9 * 9];
    snapshot_local_light(map, result);
    mapstruct *oracle = get_empty_map(9, 9);
    add_colored_light(oracle, 3, 4, 1, UINT32_C(0x00ff00));
    add_colored_light(oracle, 3, 4, 13, UINT32_C(0xff0080));
    assert_local_light_matches(oracle, result);
    recalculate_light_sources(map);
    assert_local_light_matches(map, result);
}
END_TEST

START_TEST(test_geometry_footprint_crosses_several_narrow_map_seams) {
    enum { MAPS = 7, WIDTH = 1, HEIGHT = 7 };
    mapstruct *maps[MAPS];
    for (int i = 0; i < MAPS; i++) {
        maps[i] = get_empty_map(WIDTH, HEIGHT);
        if (i != 0) {
            link_horizontal_maps(maps[i - 1], maps[i]);
        }
    }
    add_colored_light(maps[3], 0, 3, 13, UINT32_C(0x40a0ff));
    ck_assert_int_gt(GET_MAP_SPACE_PTR(maps[0], 0, 3)->light_source_value, 0);
    object *blocker = add_light_blocker(maps[2], 0, 3);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(maps[0], 0, 3)->light_source_value, 0);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(maps[6], 0, 3)->light_source_value, 0);
    object_remove(blocker, 0);
    object_destroy(blocker);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(maps[0], 0, 3)->light_source_value, 0);
    test_light_snapshot expected[MAPS][WIDTH * HEIGHT];
    for (int i = 0; i < MAPS; i++) {
        snapshot_local_light(maps[i], expected[i]);
    }
    recalculate_light_sources(maps[2]);
    for (int i = 0; i < MAPS; i++) {
        assert_local_light_matches(maps[i], expected[i]);
    }
}
END_TEST

START_TEST(test_scalar_origin_without_object_rebuilds_and_withdraws_symmetrically) {
    mapstruct *lower = get_empty_map(7, 7);
    mapstruct *upper = get_empty_map(7, 7);
    link_stacked_maps(lower, upper);
    adjust_light_source(lower, 3, 3, 13);
    test_light_snapshot expected[7 * 7];
    snapshot_local_light(upper, expected);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(upper, 3, 3)->light_source_value, 0);
    recalculate_light_sources(lower);
    assert_local_light_matches(upper, expected);
    remove_light_source_list(lower);
    test_light_snapshot empty[7 * 7] = {0};
    assert_local_light_matches(upper, empty);
}
END_TEST

START_TEST(test_rebuild_preserves_asymmetric_incoming_source) {
    mapstruct *west = get_empty_map(9, 9);
    mapstruct *middle = get_empty_map(9, 9);
    mapstruct *east = get_empty_map(9, 9);
    link_horizontal_maps(west, middle);
    link_horizontal_maps(middle, east);
    middle->tile_map[TILED_EAST] = NULL;
    add_colored_light(east, 0, 4, 13, UINT32_C(0xff8040));
    test_light_snapshot expected[9 * 9];
    snapshot_local_light(middle, expected);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(middle, 8, 4)->light_source_positive_value, 0);
    recalculate_light_sources(west);
    assert_local_light_matches(middle, expected);
    object *blocker = add_light_blocker(middle, 7, 4);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(middle, 6, 4)->light_source_value, 0);
    object_remove(blocker, 0);
    object_destroy(blocker);
    assert_local_light_matches(middle, expected);
    /* Restore the fixture's ordinary teardown backlinks. */
    middle->tile_map[TILED_EAST] = east;
}
END_TEST

START_TEST(test_disconnected_irregular_component_does_not_expand_regular_rebuild) {
    mapstruct *regular[6];
    for (size_t i = 0; i < arraysize(regular); i++) {
        regular[i] = get_empty_map(9, 9);
        if (i != 0) {
            link_horizontal_maps(regular[i - 1], regular[i]);
        }
    }
    mapstruct *irregular_west = get_empty_map(7, 9);
    mapstruct *irregular_east = get_empty_map(9, 9);
    link_horizontal_maps(irregular_west, irregular_east);
    add_colored_light(regular[0], 4, 4, 3, UINT32_C(0xff8040));
    add_colored_light(irregular_east, 4, 4, 3, UINT32_C(0x4080ff));

    /* Deliberately mark unrelated accumulators: a whole-resident fallback or
     * whole-regular-component clear would erase them. No ray from the changed
     * map reaches either cell. This tests the spatial bound, not just output
     * equivalence after a redundant clear/replay. */
    GET_MAP_SPACE_PTR(regular[5], 4, 4)->light_source_value += 123;
    GET_MAP_SPACE_PTR(irregular_east, 4, 4)->light_source_value += 456;
    test_light_snapshot far_expected[9 * 9];
    test_light_snapshot irregular_expected[9 * 9];
    test_light_snapshot local_expected[9 * 9];
    snapshot_local_light(regular[5], far_expected);
    snapshot_local_light(irregular_east, irregular_expected);
    snapshot_local_light(regular[0], local_expected);
    uint64_t rebuilds = light_rebuild_count_for_test();
    recalculate_light_sources(regular[0]);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);
    assert_local_light_matches(regular[0], local_expected);
    assert_local_light_matches(regular[5], far_expected);
    assert_local_light_matches(irregular_east, irregular_expected);
}
END_TEST

START_TEST(test_partial_grid_rebuild_bounds_targets_and_incoming_sources) {
    mapstruct *maps[10];
    for (size_t i = 0; i < arraysize(maps); i++) {
        maps[i] = get_empty_map(9, 9);
        if (i != 0) {
            link_horizontal_maps(maps[i - 1], maps[i]);
        }
    }
    mapstruct *north = get_empty_map(9, 9);
    maps[0]->tile_map[TILED_NORTH] = north;
    north->tile_map[TILED_SOUTH] = maps[0];
    maps[0]->tile_path[TILED_NORTH] = add_string("/tests/light-north");
    north->tile_path[TILED_SOUTH] = add_string("/tests/light-south");
    /* This partial grid has no northeast commuting square. A source on the
     * third seam still contributes to the second-seam map being cleared. */
    add_colored_light(maps[3], 0, 4, 13, UINT32_C(0xff8040));
    add_colored_light(maps[3], 1, 4, -5, LIGHT_COLOR_WHITE);
    test_light_snapshot expected[3][9 * 9];
    for (size_t i = 0; i < arraysize(expected); i++) {
        snapshot_local_light(maps[i], expected[i]);
    }
    ck_assert_int_gt(GET_MAP_SPACE_PTR(maps[2], 8, 4)->light_source_positive_value, 0);
    /* A sentinel proves bounded writes independently of the incremental-light
     * oracle. Discovery counts also prove that remote sources are not replayed. */
    GET_MAP_SPACE_PTR(maps[4], 4, 4)->light_source_value = 123;
    recalculate_light_sources(maps[0]);
    ck_assert_uint_eq(light_rebuild_target_maps_for_test(), 4);
    ck_assert_uint_eq(light_rebuild_source_maps_for_test(), 5);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(maps[4], 4, 4)->light_source_value, 123);
    for (size_t i = 0; i < arraysize(expected); i++) {
        assert_local_light_matches(maps[i], expected[i]);
    }
}
END_TEST

START_TEST(test_loaded_reverse_link_replacement_clears_former_target) {
    mapstruct *old_lower = get_empty_map(9, 9);
    mapstruct *upper = get_empty_map(9, 9);
    mapstruct *new_lower = get_empty_map(9, 9);
    link_stacked_maps(old_lower, upper);
    add_colored_light(upper, 4, 4, 13, UINT32_C(0xff8040));
    ck_assert_int_gt(GET_MAP_SPACE_PTR(old_lower, 4, 4)->light_source_positive_value, 0);
    /* map_set_tile() can overwrite an implicit reverse pointer during load.
     * The old lower map still points up, but post-load directed reach from the
     * upper source no longer contains it. Its old contribution must be cleared. */
    link_stacked_maps(new_lower, upper);
    check_light_source_list(new_lower);
    test_light_snapshot empty[9 * 9] = {0};
    assert_local_light_matches(old_lower, empty);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(new_lower, 4, 4)->light_source_positive_value, 0);
    old_lower->tile_map[TILED_UP] = NULL;
}
END_TEST

START_TEST(test_component_includes_incoming_only_source_and_excludes_disconnected_maps) {
    mapstruct *receiver = get_empty_map(9, 9);
    mapstruct *source = get_empty_map(9, 9);
    link_horizontal_maps(receiver, source);
    receiver->tile_map[TILED_EAST] = NULL;
    mapstruct *disconnected = get_empty_map(9, 9);
    add_colored_light(source, 0, 4, 13, UINT32_C(0x4080ff));
    add_colored_light(disconnected, 4, 4, 3, UINT32_C(0xff8040));
    GET_MAP_SPACE_PTR(disconnected, 4, 4)->light_source_value += 123;
    test_light_snapshot receiver_expected[9 * 9];
    test_light_snapshot source_expected[9 * 9];
    test_light_snapshot disconnected_expected[9 * 9];
    snapshot_local_light(receiver, receiver_expected);
    snapshot_local_light(source, source_expected);
    snapshot_local_light(disconnected, disconnected_expected);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(receiver, 8, 4)->light_source_positive_value, 0);
    recalculate_light_sources(receiver);
    assert_local_light_matches(receiver, receiver_expected);
    assert_local_light_matches(source, source_expected);
    assert_local_light_matches(disconnected, disconnected_expected);
    /* Restore backlinks for the ordinary fixture teardown. */
    receiver->tile_map[TILED_EAST] = source;
}
END_TEST

START_TEST(test_rebuild_preserves_incoming_source_with_different_map_dimensions) {
    mapstruct *west = get_empty_map(9, 9);
    mapstruct *middle = get_empty_map(12, 9);
    mapstruct *east = get_empty_map(9, 9);
    link_horizontal_maps(west, middle);
    link_horizontal_maps(middle, east);
    add_colored_light(east, 0, 4, 13, UINT32_C(0x40a0ff));
    test_light_snapshot expected[12 * 9];
    snapshot_local_light(middle, expected);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(middle, 11, 4)->light_source_positive_value, 0);
    recalculate_light_sources(west);
    assert_local_light_matches(middle, expected);
}
END_TEST

START_TEST(test_target_footprint_resolves_vertical_link_only_on_horizontal_neighbor) {
    mapstruct *west = get_empty_map(9, 9);
    mapstruct *east = get_empty_map(9, 9);
    mapstruct *upper = get_empty_map(9, 9);
    link_horizontal_maps(west, east);
    link_stacked_maps(east, upper);
    add_colored_light(east, 0, 4, 13, UINT32_C(0xff0080));
    test_light_snapshot expected[9 * 9];
    snapshot_local_light(upper, expected);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(upper, 0, 4)->light_source_positive_value, 0);
    recalculate_light_sources(west);
    assert_local_light_matches(upper, expected);
}
END_TEST

START_TEST(test_target_footprint_retains_differing_vertical_dimension_fallback) {
    mapstruct *lower = get_empty_map(12, 9);
    mapstruct *upper = get_empty_map(9, 9);
    mapstruct *upper_east = get_empty_map(9, 9);
    link_stacked_maps(lower, upper);
    link_horizontal_maps(upper, upper_east);
    /* The upper map ends inside the lower map's rectangle. Its remaining
     * coordinates must still be resolved instead of skipped as duplicates. */
    GET_MAP_SPACE_PTR(upper_east, 0, 4)->light_source_value = 1;
    recalculate_light_sources(lower);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(upper_east, 0, 4)->light_source_value, 0);
}
END_TEST

START_TEST(test_noncommuting_linked_depth_paths_rebuild_every_affected_target) {
    mapstruct *source_map = get_empty_map(9, 9);
    mapstruct *level_one = get_empty_map(9, 9);
    mapstruct *level_two = get_empty_map(9, 9);
    mapstruct *roof_map = get_empty_map(9, 9);
    mapstruct *target_map = get_empty_map(9, 9);
    link_stacked_maps(source_map, level_one);
    link_stacked_maps(level_one, level_two);
    link_horizontal_maps(level_one, roof_map);
    link_horizontal_maps(level_two, target_map);
    add_colored_light(source_map, 8, 4, 13, UINT32_C(0xff8040));
    test_light_snapshot expected[9 * 9];
    snapshot_local_light(target_map, expected);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(target_map, 0, 4)->light_source_value, 0);
    /* The ray resolves its intermediate point through level_one's east link,
     * and its endpoint through level_two's east link. roof_map itself has no
     * vertical link to that endpoint, despite every existing edge being equal
     * size and reciprocal. A geometry-origin footprint alone misses it. */
    object *roof = arch_get("roof_thatch");
    roof->x = 0;
    roof->y = 4;
    object_insert_map(roof, roof_map, NULL, 0);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(target_map, 0, 4)->light_source_value, 0);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(target_map, 0, 4)->light_source_positive_value, 0);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(target_map, 0, 4)->light_source_color_weight, 0);
    object_remove(roof, 0);
    object_destroy(roof);
    assert_local_light_matches(target_map, expected);
}
END_TEST

START_TEST(test_different_vertical_dimensions_never_publish_out_of_bounds_spaces) {
    mapstruct *lower = get_empty_map(12, 9);
    mapstruct *upper = get_empty_map(9, 9);
    link_stacked_maps(lower, upper);
    /* No east neighbor exists on the smaller upper map. Both traversal orders
     * must reject x >= 9, including the bottom row where an unchecked offset
     * would run past its allocation instead of merely aliasing another row. */
    add_colored_light(lower, 10, 8, 13, UINT32_C(0x40a0ff));
    recalculate_light_sources(lower);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(upper, 8, 8)->light_source_value, 0);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(upper, 1, 1)->light_source_color_weight, 0);
    add_colored_light(lower, 10, 0, 13, UINT32_C(0xff8040));
    recalculate_light_sources(lower);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(upper, 1, 1)->light_source_color_weight, 0);
}
END_TEST

START_TEST(test_unloading_light_relay_rebuilds_surviving_targets_once) {
    mapstruct *source_map = get_empty_map(9, 9);
    mapstruct *relay = get_empty_map(9, 9);
    mapstruct *target = get_empty_map(9, 9);
    link_stacked_maps(source_map, relay);
    link_stacked_maps(relay, target);
    add_colored_light(source_map, 4, 4, 13, UINT32_C(0xff8040));
    ck_assert_int_gt(GET_MAP_SPACE_PTR(target, 4, 4)->light_source_value, 0);
    uint64_t rebuilds = light_rebuild_count_for_test();
    free_map(relay, 1);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);
    test_light_snapshot empty[9 * 9] = {0};
    assert_local_light_matches(target, empty);
    ck_assert(!target->local_light_unlink_pending);
    ck_assert_int_gt(GET_MAP_SPACE_PTR(source_map, 4, 4)->light_source_value, 0);
}
END_TEST

START_TEST(test_deferred_unlink_flushes_before_read_and_new_source_delta) {
    mapstruct *source_map = get_empty_map(9, 9);
    mapstruct *relay = get_empty_map(9, 9);
    mapstruct *target = get_empty_map(9, 9);
    link_stacked_maps(source_map, relay);
    link_stacked_maps(relay, target);
    add_colored_light(source_map, 4, 4, 13, UINT32_C(0xff8040));
    uint64_t rebuilds = light_rebuild_count_for_test();
    light_map_unlink_begin(false);
    free_map(relay, 1);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
    ck_assert(target->local_light_unlink_pending);
    (void)map_get_darkness(target, 4, 4, NULL);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);
    test_light_snapshot empty[9 * 9] = {0};
    assert_local_light_matches(target, empty);
    light_map_unlink_end();
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);

    /* Re-establish the fixture, then exercise the pre-stack source barrier
     * with pending unlink marks instead of an explicit lighting read. */
    mapstruct *second_relay = get_empty_map(9, 9);
    link_stacked_maps(source_map, second_relay);
    link_stacked_maps(second_relay, target);
    recalculate_light_sources(source_map);
    light_map_unlink_begin(false);
    free_map(second_relay, 1);
    add_colored_light(target, 4, 4, 1, UINT32_C(0x00ff00));
    light_map_unlink_end();
    test_light_snapshot result[9 * 9];
    snapshot_local_light(target, result);
    mapstruct *oracle = get_empty_map(9, 9);
    add_colored_light(oracle, 4, 4, 1, UINT32_C(0x00ff00));
    assert_local_light_matches(oracle, result);
}
END_TEST

START_TEST(test_full_map_retirement_does_not_rebuild_per_deleted_map) {
    enum { MAPS = 4 };
    mapstruct *maps[MAPS];
    for (int i = 0; i < MAPS; i++) {
        maps[i] = get_empty_map(9, 9);
        if (i != 0) {
            link_stacked_maps(maps[i - 1], maps[i]);
        }
        add_colored_light(maps[i], 4, 4, 13, UINT32_C(0x40a0ff));
    }
    uint64_t rebuilds = light_rebuild_count_for_test();
    free_all_maps();
    ck_assert_ptr_null(first_map);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
}
END_TEST

START_TEST(test_deferred_unlink_precedes_in_place_emitter_color_change) {
    mapstruct *source_map = get_empty_map(9, 9);
    mapstruct *relay = get_empty_map(9, 9);
    mapstruct *target = get_empty_map(9, 9);
    link_stacked_maps(source_map, relay);
    link_stacked_maps(relay, target);
    add_colored_light(source_map, 4, 4, 13, LIGHT_COLOR_WHITE);
    object *emitter = add_colored_light(target, 4, 4, 2, UINT32_C(0xff0000));
    light_map_unlink_begin(false);
    free_map(relay, 1);
    ck_assert(target->local_light_unlink_pending);
    light_source_prepare_change();
    emitter->light_color = UINT32_C(0x0000ff);
    adjust_light_source_color(target, 4, 4, 2, UINT32_C(0xff0000), -1);
    adjust_light_source_color(target, 4, 4, 2, emitter->light_color, 1);
    light_map_unlink_end();
    MapSpace *center = GET_MAP_SPACE_PTR(target, 4, 4);
    ck_assert_int_eq(center->light_source_value, 80);
    ck_assert_int_eq(center->light_source_positive_value, 80);
    ck_assert_int_eq(center->light_source_color[0], 0);
    ck_assert_int_eq(center->light_source_color[1], 0);
    ck_assert_int_eq(center->light_source_color[2], INT64_C(80) * UINT16_MAX);
    ck_assert_int_eq(center->light_source_color_weight, INT64_C(80) * UINT16_MAX);
    test_light_snapshot expected[9 * 9];
    snapshot_local_light(target, expected);
    mapstruct *oracle = get_empty_map(9, 9);
    add_colored_light(oracle, 4, 4, 2, UINT32_C(0x0000ff));
    assert_local_light_matches(oracle, expected);
}
END_TEST

START_TEST(test_radial_light_profile_is_symmetric_monotonic_and_exact) {
    mapstruct *map = get_empty_map(11, 11);
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

START_TEST(test_colored_radial_light_removal_restores_whole_field) {
    mapstruct *map = get_empty_map(11, 11);
    int baseline[11 * 11];
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
    ck_assert_uint_eq(yellow[0], 1024);
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
    ck_assert_uint_eq(magenta[0], 1024);
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

START_TEST(test_light_mask_preserves_exact_vertical_roof_falloff) {
    const int expected[] = {0, 180, 80, 20, 0};

    for (int roof_depth = 1; roof_depth <= 4; roof_depth++) {
        mapstruct *levels[5] = {0};
        for (size_t depth = 0; depth < arraysize(levels); depth++) {
            levels[depth] = get_empty_map(9, 9);
            if (depth != 0) {
                link_stacked_maps(levels[depth - 1], levels[depth]);
            }
        }

        add_roof_surface(levels[roof_depth], 4, 4);
        adjust_light_source(levels[0], 4, 4, 5);

        ck_assert_int_eq(GET_MAP_SPACE_PTR(levels[roof_depth], 4, 4)->light_source_value,
                         expected[roof_depth]);
    }
}
END_TEST

START_TEST(test_roof_surface_terminates_light_at_receiving_depth) {
    mapstruct *lower = get_empty_map(9, 9);
    mapstruct *roof_level = get_empty_map(9, 9);
    mapstruct *higher = get_empty_map(9, 9);
    link_stacked_maps(lower, roof_level);
    link_stacked_maps(roof_level, higher);

    add_roof_surface(roof_level, 4, 4);
    adjust_light_source(lower, 4, 4, 5);

    ck_assert_int_eq(GET_MAP_SPACE_PTR(roof_level, 4, 4)->light_source_value, 180);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(higher, 4, 4)->light_source_value, 0);
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

START_TEST(test_saved_dense_map_teardown_does_not_rebuild_light_per_object) {
    enum {
        MAPS = 3,
        WIDTH = 24,
        HEIGHT = 24
    };
    object *active_before = active_objects;
    mapstruct *maps[MAPS];
    char paths[MAPS][HUGE_BUF];

    /* Only the maps owned by this fixture may expire in this tick. */
    for (mapstruct *existing = first_map; existing != NULL; existing = existing->next) {
        ck_assert(existing->in_memory != MAP_IN_MEMORY || existing->timeout != 1);
    }

    for (int map_index = 0; map_index < MAPS; map_index++) {
        mapstruct *map = get_empty_map(WIDTH, HEIGHT);
        maps[map_index] = map;
        char label[32];
        snprintf(VS(label), "dense-%d", map_index);
        assign_temporary_unique_path(map, paths[map_index], sizeof(paths[map_index]), label);

        map->in_memory = MAP_LOADING;
        for (int y = 0; y < HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++) {
                object *floor = arch_get("water_still");
                ck_assert_ptr_nonnull(floor);
                floor->x = x;
                floor->y = y;
                ck_assert_ptr_eq(object_insert_map(floor, map, NULL, 0), floor);
            }
        }
        map->in_memory = MAP_IN_MEMORY;

        map->timeout = 1;
        map->map_flags |= MAP_FLAG_FIXED_RTIME;
        map->reset_time = (uint32_t)seconds() + 3600;
    }

    uint64_t rebuilds = light_rebuild_count_for_test();
    for (int tick = 0; tick < MAPS; tick++) {
        check_active_maps();
        int swapped = 0;
        for (int map_index = 0; map_index < MAPS; map_index++) {
            swapped += maps[map_index]->in_memory == MAP_SWAPPED;
        }
        ck_assert_int_eq(swapped, tick + 1);
    }
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);

    for (int map_index = 0; map_index < MAPS; map_index++) {
        mapstruct *map = maps[map_index];
        ck_assert_int_eq(access(paths[map_index], F_OK), 0);
        ck_assert_int_eq(map->in_memory, MAP_SWAPPED);
        ck_assert_ptr_null(map->spaces);
        ck_assert_ptr_eq(active_objects, active_before);
        delete_map(map);
        ck_assert_int_eq(unlink(paths[map_index]), 0);
    }
}
END_TEST

START_TEST(test_map_teardown_withdraws_linked_light_and_invalidates_celestial_once) {
    enum {
        WIDTH = 9,
        HEIGHT = 9
    };
    mapstruct *departing = get_empty_map(WIDTH, HEIGHT);
    mapstruct *survivor = get_empty_map(WIDTH, HEIGHT);
    test_light_snapshot baseline[WIDTH * HEIGHT];
    link_stacked_maps(departing, survivor);
    snapshot_local_light(survivor, baseline);

    object *source = add_colored_light(departing, 4, 4, 3, UINT32_C(0x40a0ff));
    ck_assert_ptr_nonnull(source);
    MapSpace *lit = GET_MAP_SPACE_PTR(survivor, 4, 4);
    ck_assert_int_gt(lit->light_source_value, baseline[4 * WIDTH + 4].scalar);
    ck_assert_int_gt(lit->light_source_positive_value, baseline[4 * WIDTH + 4].positive);
    ck_assert_int_gt(lit->light_source_color_weight, baseline[4 * WIDTH + 4].color_weight);

    survivor->celestial_structure_revision = 41;
    survivor->celestial_light_valid = true;
    survivor->celestial_light_keyframe_valid = true;
    uint64_t rebuilds = light_rebuild_count_for_test();

    free_map(departing, 1);

    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
    assert_local_light_matches(survivor, baseline);
    ck_assert_uint_eq(survivor->celestial_structure_revision, 42);
    ck_assert(!survivor->celestial_light_valid);
    ck_assert(!survivor->celestial_light_keyframe_valid);
    ck_assert_ptr_null(survivor->tile_map[TILED_DOWN]);
    ck_assert_ptr_null(departing->tile_map[TILED_UP]);
    ck_assert_int_eq(departing->in_memory, MAP_SWAPPED);
    ck_assert_ptr_null(departing->spaces);

    delete_map(departing);
    delete_map(survivor);
}
END_TEST

START_TEST(test_successful_map_save_preserves_light_and_future_gameplay_rebuilds) {
    mapstruct *map = get_empty_map(9, 9);
    char path[HUGE_BUF];
    assign_temporary_unique_path(map, VS(path), "save-preserves-live");

    object *source = add_colored_light(map, 4, 4, 3, UINT32_C(0xff8040));
    object *floor = arch_get("water_still");
    ck_assert_ptr_nonnull(source);
    ck_assert_ptr_nonnull(floor);
    floor->x = 1;
    floor->y = 1;
    ck_assert_ptr_eq(object_insert_map(floor, map, NULL, 0), floor);

    MapSpace *lit = GET_MAP_SPACE_PTR(map, 4, 4);
    int32_t scalar = lit->light_source_value;
    int32_t positive = lit->light_source_positive_value;
    int64_t color[3];
    memcpy(color, lit->light_source_color, sizeof(color));
    int64_t color_weight = lit->light_source_color_weight;
    uint64_t rebuilds = light_rebuild_count_for_test();

    ck_assert_int_eq(new_save_map(map, 0), 0);

    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
    ck_assert_int_eq(map->in_memory, MAP_IN_MEMORY);
    ck_assert_ptr_nonnull(map->spaces);
    ck_assert_ptr_eq(source->map, map);
    ck_assert_ptr_eq(floor->map, map);
    ck_assert_ptr_nonnull(map->first_light);
    ck_assert_int_eq(lit->light_source_value, scalar);
    ck_assert_int_eq(lit->light_source_positive_value, positive);
    ck_assert_mem_eq(lit->light_source_color, color, sizeof(color));
    ck_assert_int_eq(lit->light_source_color_weight, color_weight);

    object_remove(floor, 0);
    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds + 1);
    object_destroy(floor);

    delete_map(map);
    ck_assert_int_eq(unlink(path), 0);
}
END_TEST

START_TEST(test_failed_map_save_preserves_live_objects_state_and_light) {
    char directory[] = "/tmp/atrinik-light-save-failure-XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(directory));

    mapstruct *map = get_empty_map(9, 9);
    FREE_AND_COPY_HASH(map->path, "/tests/light-save-failure");
    char invalid_path[HUGE_BUF];
    int written = snprintf(VS(invalid_path), "%s/missing/runtime.map", directory);
    ck_assert_int_ge(written, 0);
    ck_assert_uint_lt((size_t)written, sizeof(invalid_path));
    map->tmpname = xstrdup(invalid_path);

    object *source = add_colored_light(map, 4, 4, 3, UINT32_C(0x8040ff));
    object *floor = arch_get("water_still");
    ck_assert_ptr_nonnull(source);
    ck_assert_ptr_nonnull(floor);
    floor->x = 1;
    floor->y = 1;
    ck_assert_ptr_eq(object_insert_map(floor, map, NULL, 0), floor);

    test_light_snapshot baseline[9 * 9];
    snapshot_local_light(map, baseline);
    MapSpace *source_space = GET_MAP_SPACE_PTR(map, 4, 4);
    MapSpace *floor_space = GET_MAP_SPACE_PTR(map, 1, 1);
    object *source_first = source_space->first;
    object *floor_first = floor_space->first;
    uint64_t rebuilds = light_rebuild_count_for_test();

    ck_assert_int_eq(new_save_map(map, 0), -1);

    ck_assert_uint_eq(light_rebuild_count_for_test(), rebuilds);
    ck_assert_int_eq(map->in_memory, MAP_IN_MEMORY);
    ck_assert_ptr_nonnull(map->spaces);
    ck_assert_ptr_eq(source->map, map);
    ck_assert_ptr_eq(floor->map, map);
    ck_assert_ptr_eq(source_space->first, source_first);
    ck_assert_ptr_eq(floor_space->first, floor_first);
    ck_assert_ptr_nonnull(map->first_light);
    assert_local_light_matches(map, baseline);

    clean_tmp_map(map);
    delete_map(map);
    ck_assert_int_eq(rmdir(directory), 0);
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
    tcase_add_test(tc_core, test_rebuild_preserves_incoming_light_from_third_horizontal_map);
    tcase_add_test(tc_core, test_rebuild_matches_incremental_oracle_for_all_resident_sources);
    tcase_add_test(tc_core, test_unchanged_geometry_updates_do_not_rebuild_or_invalidate);
    tcase_add_test(tc_core, test_geometry_tracks_last_blocker_and_cleared_flag);
    tcase_add_test(tc_core, test_geometry_batches_flush_before_read_and_at_scope_end);
    tcase_add_test(tc_core, test_floor_and_roof_geometry_detects_removal_and_flag_changes);
    tcase_add_test(tc_core, test_multipart_flag_update_rebuilds_once_and_removal_withdraws_tail_once);
    tcase_add_test(tc_core, test_batch_geometry_and_source_deltas_preserve_all_accumulators);
    tcase_add_test(tc_core, test_geometry_footprint_crosses_several_narrow_map_seams);
    tcase_add_test(tc_core, test_scalar_origin_without_object_rebuilds_and_withdraws_symmetrically);
    tcase_add_test(tc_core, test_rebuild_preserves_asymmetric_incoming_source);
    tcase_add_test(tc_core, test_disconnected_irregular_component_does_not_expand_regular_rebuild);
    tcase_add_test(tc_core, test_partial_grid_rebuild_bounds_targets_and_incoming_sources);
    tcase_add_test(tc_core, test_loaded_reverse_link_replacement_clears_former_target);
    tcase_add_test(tc_core,
                   test_component_includes_incoming_only_source_and_excludes_disconnected_maps);
    tcase_add_test(tc_core, test_rebuild_preserves_incoming_source_with_different_map_dimensions);
    tcase_add_test(tc_core, test_target_footprint_resolves_vertical_link_only_on_horizontal_neighbor);
    tcase_add_test(tc_core, test_target_footprint_retains_differing_vertical_dimension_fallback);
    tcase_add_test(tc_core, test_noncommuting_linked_depth_paths_rebuild_every_affected_target);
    tcase_add_test(tc_core, test_different_vertical_dimensions_never_publish_out_of_bounds_spaces);
    tcase_add_test(tc_core, test_unloading_light_relay_rebuilds_surviving_targets_once);
    tcase_add_test(tc_core, test_deferred_unlink_flushes_before_read_and_new_source_delta);
    tcase_add_test(tc_core, test_full_map_retirement_does_not_rebuild_per_deleted_map);
    tcase_add_test(tc_core, test_deferred_unlink_precedes_in_place_emitter_color_change);
    tcase_add_test(tc_core, test_light_level_interpolation);
    tcase_add_test(tc_core, test_radial_light_profile_is_symmetric_monotonic_and_exact);
    tcase_add_test(tc_core, test_colored_radial_light_removal_restores_whole_field);
    tcase_add_test(tc_core, test_light_color_parser_is_exact);
    tcase_add_test(tc_core, test_radiance_resolver_preserves_linear_warm_and_cool_daylight);
    tcase_add_test(tc_core, test_colored_lights_add_remove_and_order_are_exact);
    tcase_add_test(tc_core, test_colored_lights_blend_green_yellow_and_capped_same_cell);
    tcase_add_test(tc_core, test_neutral_and_darkness_sources_remain_achromatic);
    tcase_add_test(tc_core, test_darkness_subtracts_achromatically_from_colored_light);
    tcase_add_test(tc_core, test_colored_light_recalculation_and_linked_depth_are_stable);
    tcase_add_test(tc_core, test_light_mask_propagates_in_three_dimensions);
    tcase_add_test(tc_core, test_light_mask_preserves_exact_vertical_roof_falloff);
    tcase_add_test(tc_core, test_roof_surface_terminates_light_at_receiving_depth);
    tcase_add_test(tc_core, test_radial_light_crosses_horizontal_map_boundaries);
    tcase_add_test(tc_core, test_light_mask_is_blocked_by_floors_in_both_directions);
    tcase_add_test(tc_core, test_light_mask_lights_exposed_upper_wall_face);
    tcase_add_test(tc_core, test_light_mask_recalculates_around_opaque_cells);
    tcase_add_test(tc_core, test_loaded_map_light_check_is_idempotent);
    tcase_add_test(tc_core, test_remove_light_source_list_accepts_swapped_map);
    tcase_add_test(tc_core, test_saved_dense_map_teardown_does_not_rebuild_light_per_object);
    tcase_add_test(tc_core,
                   test_map_teardown_withdraws_linked_light_and_invalidates_celestial_once);
    tcase_add_test(tc_core, test_successful_map_save_preserves_light_and_future_gameplay_rebuilds);
    tcase_add_test(tc_core, test_failed_map_save_preserves_live_objects_state_and_light);

    return s;
}

void check_server_light(void) {
    check_run_suite(suite(), __FILE__);
}
