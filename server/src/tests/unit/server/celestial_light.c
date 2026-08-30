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
    tcase_add_test(tc_core, test_celestial_open_field_matches_daylight_anchor);
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
