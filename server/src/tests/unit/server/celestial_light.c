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
#include <celestial_override.h>
#include <celestial_structure.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <commands.h>
#include <initialization.h>
#include <light.h>
#include <map.h>
#include <object.h>
#include <region.h>
#include <time.h>

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

START_TEST(test_celestial_open_field_matches_daylight_anchor) {
    mapstruct *map = open_fixture(5, 5);
    ck_assert(celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 12));

    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
            ck_assert_int_eq(space->celestial_light_value, 1281);
            ck_assert_int_eq(space->celestial_light_rgb[0], 1281);
            ck_assert_int_eq(space->celestial_light_rgb[1], 1281);
            ck_assert_int_eq(space->celestial_light_rgb[2], 1281);
        }
    }
}
END_TEST

START_TEST(test_derived_horizontal_neighbors_preserve_day_and_lunar_light) {
    mapstruct *map = open_fixture(3, 3);
    FREE_AND_COPY_HASH(map->path, "/celestial/world_1_1");
    /* The normal coordinate loader synthesizes these travel paths without
     * an authored celestial boundary, even while neighbors are unloaded. */
    for (size_t i = 0; i < TILED_UP; i++) {
        FREE_AND_COPY_HASH(map->tile_path[i], "/celestial/unloaded-neighbor");
    }
    uint64_t saved_hour = todtick;
    todtick = 5 * HOURS_PER_MONTH + 12;
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
    int daylight = map_get_darkness(map, 1, 1, NULL);
    uint64_t generation = celestial_light_generation(map);
    ck_assert_int_gt(daylight, 1000);

    todtick = HOURS_PER_MONTH / 2;
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
    int moonlight = map_get_darkness(map, 1, 1, NULL);
    ck_assert_int_gt(moonlight, 0);
    ck_assert_int_lt(moonlight, daylight);
    ck_assert_uint_gt(celestial_light_generation(map), generation);

    ck_assert(celestial_override_set_phase(CELESTIAL_LUNAR_NEW));
    celestial_light_invalidate_all();
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
    ck_assert_int_lt(map_get_darkness(map, 1, 1, NULL), moonlight);
    ck_assert(celestial_override_clear());
    todtick = saved_hour;

    /* Authored unresolved seams and vertical stacks still fail closed. */
    map->celestial_tile_path_seen[TILED_EAST] = true;
    map->celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    ck_assert(!celestial_light_rebuild(map, (uint64_t)todtick));
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 1, 1)->celestial_light_value, 0);
    map->celestial_tile_path_seen[TILED_EAST] = false;
    map->celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_UNSET;
    map->celestial_sky_above = CELESTIAL_SKY_LINKED;
    FREE_AND_COPY_HASH(map->tile_path[TILED_UP], "/celestial/missing-upper");
    ck_assert(!celestial_light_rebuild(map, (uint64_t)todtick));
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 1, 1)->celestial_light_value, 0);
}
END_TEST

START_TEST(test_discontinuous_horizontal_neighbor_is_not_a_local_light_dependency) {
    mapstruct *map = open_fixture(3, 3);
    FREE_AND_COPY_HASH(map->path, "/independent-west");
    FREE_AND_COPY_HASH(map->tile_path[TILED_EAST], "/unloaded-east");
    map->celestial_tile_path_seen[TILED_EAST] = true;
    map->celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_DISCONTINUOUS;
    char error[HUGE_BUF];
    ck_assert(!celestial_structure_validate_topology(map, VS(error)));
    ck_assert_msg(celestial_structure_validate_light_dependencies(map, VS(error)), "%s", error);
    ck_assert(celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 15));
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 1, 1)->celestial_light_value, 100);

    /* Missing policies and continuous dependencies must still fail closed. */
    map->celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_UNSET;
    ck_assert(!celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 15));
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 1, 1)->celestial_light_value, 0);
    map->celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    ck_assert(!celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 15));
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 1, 1)->celestial_light_value, 0);

    /* Discontinuity never authorizes light through unresolved upper cover. */
    map->celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_DISCONTINUOUS;
    map->celestial_sky_above = CELESTIAL_SKY_LINKED;
    FREE_AND_COPY_HASH(map->tile_path[TILED_UP], "/unloaded-cover");
    map->celestial_tile_path_seen[TILED_UP] = true;
    map->celestial_boundary[TILED_UP] = CELESTIAL_BOUNDARY_DISCONTINUOUS;
    ck_assert(!celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 15));
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 1, 1)->celestial_light_value, 0);
}
END_TEST

START_TEST(test_unloaded_lower_map_does_not_gate_upper_light) {
    mapstruct *map = open_fixture(3, 3);
    FREE_AND_COPY_HASH(map->path, "/surface");
    FREE_AND_COPY_HASH(map->tile_path[TILED_DOWN], "/unloaded-sewer");
    map->celestial_tile_path_seen[TILED_DOWN] = true;
    map->celestial_boundary[TILED_DOWN] = CELESTIAL_BOUNDARY_CONTINUOUS;
    char error[HUGE_BUF];
    ck_assert(!celestial_structure_validate_topology(map, VS(error)));
    ck_assert_msg(celestial_structure_validate_light_dependencies(map, VS(error)), "%s", error);
    ck_assert(celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 15));
    ck_assert_int_gt(GET_MAP_SPACE_PTR(map, 1, 1)->celestial_light_value, 100);

    /* Resident lower maps must not get a field or cache state from this solve. */
    mapstruct *lower = open_fixture(3, 3);
    map->tile_map[TILED_DOWN] = lower;
    lower->tile_map[TILED_UP] = map;
    ck_assert(celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 15));
    ck_assert(!lower->celestial_light_valid);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(lower, 1, 1)->celestial_light_value, 0);
    map->tile_map[TILED_DOWN] = NULL;
    lower->tile_map[TILED_UP] = NULL;

    /* An unresolved upper map still removes all celestial light. */
    map->celestial_sky_above = CELESTIAL_SKY_LINKED;
    FREE_AND_COPY_HASH(map->tile_path[TILED_UP], "/unloaded-roof");
    map->celestial_tile_path_seen[TILED_UP] = true;
    map->celestial_boundary[TILED_UP] = CELESTIAL_BOUNDARY_CONTINUOUS;
    ck_assert(!celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 15));
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 1, 1)->celestial_light_value, 0);
}
END_TEST

START_TEST(test_brynknot_and_outside_fields_follow_settime) {
    const char *paths[] = {
        "/shattered_islands/world_0_67", "/shattered_islands/world_1_67",
        "/shattered_islands/world_2_67", "/shattered_islands/world_0_68",
        "/shattered_islands/world_1_68", "/shattered_islands/world_2_68",
        "/shattered_islands/world_0_69", "/shattered_islands/world_1_69",
        "/shattered_islands/world_2_69", "/shattered_islands/world_0_70",
        "/shattered_islands/world_1_70", "/shattered_islands/world_2_70",
        "/shattered_islands/world_3_67",
    };
    unsigned long saved_hour = todtick;
    (void)celestial_override_clear();
    for (size_t i = 0; i < arraysize(paths); i++) {
        mapstruct *map = ready_map_name(paths[i], NULL, MAP_NO_DYNAMIC);
        ck_assert_msg(map != NULL, "Could not load %s", paths[i]);
        /* Resolve the same bounded upward chain as MAP2's camera traversal.
         * Occluded lower maps and horizontal seams are deliberately not warmed. */
        mapstruct *upper = map;
        for (size_t depth = 0; upper->tile_path[TILED_UP] != NULL; depth++) {
            ck_assert_uint_lt(depth, MAP2_MAX_DEPTH);
            upper = get_map_from_tiled(upper, TILED_UP);
            ck_assert_ptr_nonnull(upper);
        }
        int x = -1, y = -1;
        for (int row = 0; row < map->height && x < 0; row++) {
            for (int col = 0; col < map->width; col++) {
                if (celestial_structure_cell_exposed(map, col, row)) {
                    x = col;
                    y = row;
                    break;
                }
            }
        }
        ck_assert_msg(x >= 0, "%s has no exposed sample", paths[i]);
        todtick = 5 * HOURS_PER_MONTH;
        char afternoon[] = "15";
        command_settime(NULL, "settime", afternoon);
        ck_assert_uint_eq(todtick, 5 * HOURS_PER_MONTH + 15);
        char dependency_error[HUGE_BUF];
        ck_assert_msg(celestial_structure_validate_light_dependencies(map, VS(dependency_error)),
                      "%s afternoon dependency: %s", paths[i], dependency_error);
        ck_assert_msg(celestial_light_keyframe_ensure(map, (uint64_t)todtick),
                      "%s afternoon field failed", paths[i]);
        int day = GET_MAP_SPACE_PTR(map, x, y)->celestial_light_value;
        uint64_t generation = celestial_light_generation(map);
        ck_assert_msg(day > 100, "%s afternoon radiance is %d", paths[i], day);
        char midnight[] = "0";
        command_settime(NULL, "settime", midnight);
        ck_assert_uint_eq(todtick, 5 * HOURS_PER_MONTH + HOURS_PER_DAY);
        ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
        ck_assert_int_lt(GET_MAP_SPACE_PTR(map, x, y)->celestial_light_value, day);
        ck_assert_uint_gt(celestial_light_generation(map), generation);
    }
    todtick = saved_hour;
}
END_TEST

START_TEST(test_real_wilderness_midnight_full_and_new_moon_samples) {
    mapstruct *map = ready_map_name("/shattered_islands/world_3_67", NULL, MAP_NO_DYNAMIC);
    ck_assert_ptr_nonnull(map);
    unsigned long saved_hour = todtick;
    todtick = 5 * HOURS_PER_MONTH + 15;
    (void)celestial_override_clear();
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
    uint64_t day_generation = celestial_light_generation(map);
    char midnight[] = "0";
    command_settime(NULL, "settime", midnight);
    ck_assert(celestial_override_set_phase(CELESTIAL_LUNAR_FULL));
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
    ck_assert_uint_gt(celestial_light_generation(map), day_generation);
    MapSpace *sample = NULL;
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
            /* No solar field may survive the afternoon-to-midnight jump. */
            ck_assert_int_le(space->celestial_light_value, 22);
            ck_assert_int_le(space->celestial_light_next_value, 22);
            if (space->celestial_light_value == 22 &&
                map_get_darkness(map, x, y, NULL) == 22) {
                sample = space;
            }
        }
    }
    ck_assert_ptr_nonnull(sample);
    ck_assert_int_eq(sample->celestial_light_rgb[0], 11);
    ck_assert_int_eq(sample->celestial_light_rgb[1], 14);
    ck_assert_int_eq(sample->celestial_light_rgb[2], 22);
    uint16_t scalar, rgb[3];
    light_radiance_from_raw(sample, sample->celestial_light_value, &scalar, rgb);
    ck_assert_uint_eq(scalar, 35);
    ck_assert_uint_eq(rgb[0], 18);
    ck_assert_uint_eq(rgb[1], 22);
    ck_assert_uint_eq(rgb[2], 35);
    ck_assert(celestial_override_set_phase(CELESTIAL_LUNAR_NEW));
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
    ck_assert_int_eq(sample->celestial_light_value, 2);
    ck_assert_int_eq(sample->celestial_light_rgb[0], 0);
    ck_assert_int_eq(sample->celestial_light_rgb[1], 1);
    ck_assert_int_eq(sample->celestial_light_rgb[2], 2);
    light_radiance_from_raw(sample, sample->celestial_light_value, &scalar, rgb);
    ck_assert_uint_eq(scalar, 3);
    ck_assert_uint_eq(rgb[0], 0);
    ck_assert_uint_eq(rgb[1], 2);
    ck_assert_uint_eq(rgb[2], 3);
    ck_assert(celestial_override_clear());
    todtick = saved_hour;
}
END_TEST

START_TEST(test_celestial_uses_directional_shadow_and_reseeds_after_bound) {
    mapstruct *map = open_fixture(40, 1);
    object *wall = arch_get("wall_wood_1");
    ck_assert_ptr_nonnull(wall);
    wall->x = 35;
    wall->y = 0;
    ck_assert_ptr_nonnull(object_insert_map(wall, map, NULL, 0));

    ck_assert(celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 7));
    for (int x = 39; x >= 35; x--) {
        ck_assert_int_eq(GET_MAP_SPACE_PTR(map, x, 0)->celestial_light_value, 378);
    }
    for (int x = 34; x >= 3; x--) {
        ck_assert_int_eq(GET_MAP_SPACE_PTR(map, x, 0)->celestial_light_value, 130);
    }
    for (int x = 2; x >= 0; x--) {
        ck_assert_int_eq(GET_MAP_SPACE_PTR(map, x, 0)->celestial_light_value, 378);
    }
}
END_TEST

START_TEST(test_celestial_lunar_and_starlight_are_additive) {
    mapstruct *map = open_fixture(3, 3);
    ck_assert(celestial_light_rebuild(map, HOURS_PER_MONTH / 2));
    MapSpace *space = GET_MAP_SPACE_PTR(map, 1, 1);
    ck_assert_int_eq(space->celestial_light_value, 22);
    ck_assert_int_eq(space->celestial_light_rgb[0], 11);
    ck_assert_int_eq(space->celestial_light_rgb[1], 14);
    ck_assert_int_eq(space->celestial_light_rgb[2], 22);
}
END_TEST

START_TEST(test_celestial_invalid_topology_fails_closed) {
    mapstruct *map = open_fixture(3, 3);
    map->celestial_sky_above = CELESTIAL_SKY_LINKED;
    map->celestial_tile_path_seen[TILED_UP] = true;
    map->celestial_boundary[TILED_UP] = CELESTIAL_BOUNDARY_CONTINUOUS;
    map->tile_path[TILED_UP] = add_string("/missing-upper");

    ck_assert(!celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 12));
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 1, 1)->celestial_light_value, 0);
    ck_assert_int_eq(GET_MAP_SPACE_PTR(map, 1, 1)->celestial_light_rgb[0], 0);
}
END_TEST

START_TEST(test_celestial_rebuild_reaches_existing_radiance_resolver) {
    mapstruct *map = open_fixture(1, 1);
    ck_assert(celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 12));
    MapSpace *space = GET_MAP_SPACE_PTR(map, 0, 0);
    uint16_t scalar;
    uint16_t rgb[3];
    light_radiance_from_raw(space, space->celestial_light_value, &scalar, rgb);
    ck_assert_uint_eq(scalar, 2050);
    ck_assert_uint_eq(rgb[0], 2050);
    ck_assert_uint_eq(rgb[1], 2050);
    ck_assert_uint_eq(rgb[2], 2050);
}
END_TEST

START_TEST(test_celestial_map_darkness_uses_current_cached_field) {
    mapstruct *map = open_fixture(2, 2);
    uint64_t hour = (uint64_t)todtick;
    ck_assert(celestial_light_rebuild(map, hour));
    MapSpace *space = GET_MAP_SPACE_PTR(map, 0, 0);
    int expected = map->light_value + space->light_value + space->light_source_value +
                  space->celestial_light_value;
    ck_assert_int_eq(map_get_darkness(map, 0, 0, NULL), expected);
}
END_TEST

START_TEST(test_celestial_invalidation_rebuilds_only_after_revision_change) {
    mapstruct *map = open_fixture(2, 2);
    ck_assert(celestial_light_rebuild(map, (uint64_t)todtick));
    uint64_t revision = map->celestial_structure_revision;
    ck_assert(map->celestial_light_valid);

    celestial_light_invalidate(map);
    ck_assert_uint_eq(map->celestial_structure_revision, revision + 1);
    ck_assert(!map->celestial_light_valid);
    ck_assert(celestial_light_rebuild(map, (uint64_t)todtick));
    ck_assert(map->celestial_light_valid);
}
END_TEST

START_TEST(test_celestial_keyframe_preserves_current_and_stages_next_field) {
    mapstruct *map = open_fixture(2, 2);
    uint64_t hour = (uint64_t)todtick;
    ck_assert(celestial_light_rebuild(map, hour));
    MapSpace *space = GET_MAP_SPACE_PTR(map, 0, 0);
    int32_t current = space->celestial_light_value;
    uint64_t generation = celestial_light_generation(map);
    ck_assert_uint_gt(generation, 0);

    ck_assert(celestial_light_keyframe_ensure(map, hour));
    ck_assert(map->celestial_light_keyframe_valid);
    ck_assert_uint_eq(celestial_light_generation(map), generation);
    ck_assert_int_eq(space->celestial_light_value, current);
    ck_assert_uint_gt(space->celestial_light_next_value, 0);
    ck_assert(celestial_light_keyframe_ensure(map, hour));

    celestial_light_invalidate(map);
    ck_assert(!map->celestial_light_keyframe_valid);
}
END_TEST

START_TEST(test_celestial_override_replaces_lunar_age_and_invalidates_cache) {
    mapstruct *map = open_fixture(3, 3);
    MapSpace *space = GET_MAP_SPACE_PTR(map, 1, 1);
    uint64_t initial_generation;
    uint64_t initial_revision;

    (void)celestial_override_clear();
    ck_assert(celestial_light_rebuild(map, 0));
    ck_assert_int_eq(space->celestial_light_value, 2);
    initial_generation = celestial_light_generation(map);
    initial_revision = celestial_override_revision();

    ck_assert(celestial_override_set_phase(CELESTIAL_LUNAR_FULL));
    ck_assert_uint_gt(celestial_override_revision(), initial_revision);
    celestial_light_invalidate_all();
    ck_assert(celestial_light_rebuild(map, 0));
    ck_assert_int_eq(space->celestial_light_value, 22);
    ck_assert_uint_gt(celestial_light_generation(map), initial_generation);

    ck_assert(celestial_override_set_age(0, HOURS_PER_MONTH));
    celestial_light_invalidate_all();
    ck_assert(celestial_light_rebuild(map, 0));
    ck_assert_int_eq(space->celestial_light_value, 2);
    ck_assert_uint_gt(celestial_light_generation(map), initial_generation + 1);

    ck_assert(celestial_override_clear());
    ck_assert(!celestial_override_clear());
}
END_TEST

START_TEST(test_celestial_override_named_phases_scale_to_effective_period) {
    static const celestial_lunar_phase phases[] = {
        CELESTIAL_LUNAR_NEW,
        CELESTIAL_LUNAR_WAXING_CRESCENT,
        CELESTIAL_LUNAR_FIRST_QUARTER,
        CELESTIAL_LUNAR_WAXING_GIBBOUS,
        CELESTIAL_LUNAR_FULL,
        CELESTIAL_LUNAR_WANING_GIBBOUS,
        CELESTIAL_LUNAR_LAST_QUARTER,
        CELESTIAL_LUNAR_WANING_CRESCENT,
    };

    (void)celestial_override_clear();
    for (size_t index = 0; index < arraysize(phases); index++) {
        uint16_t age = UINT16_MAX;
        ck_assert(celestial_override_set_phase(phases[index]));
        ck_assert(celestial_override_apply(HOURS_PER_MONTH, &age));
        ck_assert_uint_eq(age, index * (HOURS_PER_MONTH / 8));
        ck_assert(celestial_override_apply(HOURS_PER_MONTH / 4, &age));
        ck_assert_uint_eq(age, index * ((HOURS_PER_MONTH / 4) / 8));
    }

    ck_assert(celestial_override_set_age(HOURS_PER_MONTH / 2, HOURS_PER_MONTH));
    uint16_t age = 0;
    ck_assert(celestial_override_apply(HOURS_PER_MONTH / 4, &age));
    ck_assert_uint_eq(age, HOURS_PER_MONTH / 8);
    ck_assert(!celestial_override_set_age(HOURS_PER_MONTH, HOURS_PER_MONTH));
    ck_assert(!celestial_override_set_age(0, 167));
    ck_assert(!celestial_override_set_phase((celestial_lunar_phase)CELESTIAL_LUNAR_PHASE_COUNT));
    ck_assert(celestial_override_clear());
}
END_TEST

START_TEST(test_celestial_override_parser_is_strict_and_names_are_stable) {
    static const char *const names[] = {
        "new",
        "waxing-crescent",
        "first-quarter",
        "waxing-gibbous",
        "full",
        "waning-gibbous",
        "last-quarter",
        "waning-crescent",
    };
    uint16_t age;
    celestial_lunar_phase phase;

    for (size_t index = 0; index < arraysize(names); index++) {
        ck_assert(celestial_override_parse_phase(names[index], &phase));
        ck_assert_uint_eq(phase, index);
        ck_assert_str_eq(celestial_override_phase_name(phase), names[index]);
    }
    ck_assert(celestial_override_parse_phase("FULL", &phase));
    ck_assert_int_eq(phase, CELESTIAL_LUNAR_FULL);
    ck_assert(!celestial_override_parse_phase("full-moon", &phase));
    ck_assert(!celestial_override_parse_phase("full extra", &phase));

    ck_assert(celestial_override_parse_age("0", &age));
    ck_assert_uint_eq(age, 0);
    ck_assert(celestial_override_parse_age("65535", &age));
    ck_assert_uint_eq(age, UINT16_MAX);
    ck_assert(!celestial_override_parse_age("65536", &age));
    ck_assert(!celestial_override_parse_age("-1", &age));
    ck_assert(!celestial_override_parse_age("+1", &age));
    ck_assert(!celestial_override_parse_age("1x", &age));
    ck_assert(!celestial_override_parse_age("", &age));
}
END_TEST

START_TEST(test_celestial_override_invalidates_loaded_keyframes) {
    mapstruct *map = open_fixture(2, 2);
    uint64_t hour = (uint64_t)todtick;
    uint64_t key;
    uint64_t generation;

    (void)celestial_override_clear();
    ck_assert(celestial_light_keyframe_ensure(map, hour));
    ck_assert(map->celestial_light_valid);
    ck_assert(map->celestial_light_keyframe_valid);
    key = map->celestial_light_key;
    generation = celestial_light_generation(map);
    ck_assert_uint_gt(generation, 0);

    ck_assert(celestial_override_set_phase(CELESTIAL_LUNAR_FULL));
    celestial_light_invalidate_all();
    ck_assert(map->celestial_light_valid);
    ck_assert_uint_eq(map->celestial_light_key, key);
    ck_assert_uint_eq(celestial_light_generation(map), generation);
    ck_assert(!map->celestial_light_keyframe_valid);
    ck_assert_uint_eq(map->celestial_light_next_key, 0);

    ck_assert(celestial_light_keyframe_ensure(map, hour));
    ck_assert(map->celestial_light_keyframe_valid);
    ck_assert_uint_gt(celestial_light_generation(map), generation);
    ck_assert(celestial_override_clear());
}
END_TEST

START_TEST(test_celestial_command_lifecycle_and_permission_gate) {
    mapstruct *map;
    object *pl;
    const region_celestial_profile_t *profile;
    char saved_default_groups[MAX_BUF];
    celestial_override_state_t state;
    uint64_t revision;
    char command[MAX_BUF];

    check_setup_env_pl(&map, &pl);
    profile = region_celestial_for_map(map);
    ck_assert_ptr_nonnull(profile);
    (void)celestial_override_clear();
    memcpy(saved_default_groups, settings.default_permission_groups, sizeof(saved_default_groups));
    settings.default_permission_groups[0] = '\0';

    snprintf(command, sizeof(command), "/celestial phase full");
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert(!state.active);
    ck_assert(!commands_check_permission(CONTR(pl), "/celestial"));

    snprintf(settings.default_permission_groups,
             sizeof(settings.default_permission_groups),
             "[DEV]");
    ck_assert(commands_check_permission(CONTR(pl), "celestial"));

    snprintf(command, sizeof(command), "/celestial phase full");
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert(state.active);
    ck_assert_int_eq(state.mode, CELESTIAL_OVERRIDE_MODE_PHASE);
    ck_assert_int_eq(state.value, CELESTIAL_LUNAR_FULL);
    revision = state.revision;

    snprintf(command, sizeof(command), "/celestial phase full");
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert_uint_gt(state.revision, revision);
    revision = state.revision;

    snprintf(command, sizeof(command), "/celestial phase full extra");
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert_int_eq(state.mode, CELESTIAL_OVERRIDE_MODE_PHASE);
    ck_assert_int_eq(state.value, CELESTIAL_LUNAR_FULL);
    ck_assert_uint_eq(state.revision, revision);

    snprintf(command, sizeof(command), "/celestial phase full-moon");
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert_uint_eq(state.revision, revision);

    snprintf(command, sizeof(command), "/celestial status");
    commands_handle(pl, command);
    snprintf(command, sizeof(command), "/celestial");
    commands_handle(pl, command);

    snprintf(command, sizeof(command), "/celestial age 0");
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert_int_eq(state.mode, CELESTIAL_OVERRIDE_MODE_AGE);
    ck_assert_uint_eq(state.value, 0);
    ck_assert_uint_eq(state.period, profile->lunar_period);
    revision = state.revision;

    snprintf(command,
             sizeof(command),
             "/celestial age %u",
             profile->lunar_period);
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert_uint_eq(state.revision, revision);

    snprintf(command, sizeof(command), "/celestial age -1");
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert_uint_eq(state.revision, revision);

    snprintf(command, sizeof(command), "/celestial clear extra");
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert(state.active);

    snprintf(command, sizeof(command), "/celestial clear");
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert(!state.active);
    revision = state.revision;

    snprintf(command, sizeof(command), "/celestial clear");
    commands_handle(pl, command);
    celestial_override_get(&state);
    ck_assert(!state.active);
    ck_assert_uint_eq(state.revision, revision);

    memcpy(settings.default_permission_groups,
           saved_default_groups,
           sizeof(settings.default_permission_groups));
}
END_TEST

START_TEST(test_celestial_64x64_build_is_bounded) {
    mapstruct *map = open_fixture(64, 64);
    struct timespec start;
    struct timespec finish;
    ck_assert_int_eq(clock_gettime(CLOCK_MONOTONIC, &start), 0);
    ck_assert(celestial_light_rebuild(map, (uint64_t)todtick));
    ck_assert_int_eq(clock_gettime(CLOCK_MONOTONIC, &finish), 0);
    int64_t elapsed_ns = (int64_t)(finish.tv_sec - start.tv_sec) * INT64_C(1000000000) +
                         finish.tv_nsec - start.tv_nsec;
    ck_assert_int_ge(elapsed_ns, 0);
    ck_assert_int_lt(elapsed_ns, INT64_C(5000000000));
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("celestial_light");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    TCase *tc_real_maps = tcase_create("Real maps");
    tcase_add_unchecked_fixture(tc_real_maps, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_real_maps, check_test_setup, check_test_teardown);
    tcase_set_timeout(tc_real_maps, 30);
    suite_add_tcase(s, tc_real_maps);
    tcase_add_test(tc_core, test_celestial_open_field_matches_daylight_anchor);
    tcase_add_test(tc_core, test_discontinuous_horizontal_neighbor_is_not_a_local_light_dependency);
    tcase_add_test(tc_real_maps, test_brynknot_and_outside_fields_follow_settime);
    tcase_add_test(tc_core, test_unloaded_lower_map_does_not_gate_upper_light);
    tcase_add_test(tc_core, test_real_wilderness_midnight_full_and_new_moon_samples);
    tcase_add_test(tc_core, test_derived_horizontal_neighbors_preserve_day_and_lunar_light);
    tcase_add_test(tc_core, test_celestial_uses_directional_shadow_and_reseeds_after_bound);
    tcase_add_test(tc_core, test_celestial_lunar_and_starlight_are_additive);
    tcase_add_test(tc_core, test_celestial_invalid_topology_fails_closed);
    tcase_add_test(tc_core, test_celestial_rebuild_reaches_existing_radiance_resolver);
    tcase_add_test(tc_core, test_celestial_map_darkness_uses_current_cached_field);
    tcase_add_test(tc_core, test_celestial_invalidation_rebuilds_only_after_revision_change);
    tcase_add_test(tc_core, test_celestial_keyframe_preserves_current_and_stages_next_field);
    tcase_add_test(tc_core, test_celestial_override_replaces_lunar_age_and_invalidates_cache);
    tcase_add_test(tc_core, test_celestial_override_named_phases_scale_to_effective_period);
    tcase_add_test(tc_core, test_celestial_override_parser_is_strict_and_names_are_stable);
    tcase_add_test(tc_core, test_celestial_override_invalidates_loaded_keyframes);
    tcase_add_test(tc_core, test_celestial_command_lifecycle_and_permission_gate);
    tcase_add_test(tc_core, test_celestial_64x64_build_is_bounded);
    return s;
}

void check_server_celestial_light(void) {
    check_run_suite(suite(), __FILE__);
}
