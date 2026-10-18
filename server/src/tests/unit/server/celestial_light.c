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
#include <signal.h>
#include <inttypes.h>

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

/** What one view update does to a map: gameplay gating ensures its current
 * field, and the view queues the next hour's on the budgeted solver. */
static void request_next(mapstruct *map) {
    celestial_light_ensure(map);
    celestial_light_prepare_next(map);
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
    ck_assert_int_gt(daylight, 1000);

    todtick = HOURS_PER_MONTH / 2;
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
    int moonlight = map_get_darkness(map, 1, 1, NULL);
    ck_assert_int_gt(moonlight, 0);
    ck_assert_int_lt(moonlight, daylight);

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
        ck_assert_msg(day > 100, "%s afternoon radiance is %d", paths[i], day);
        char midnight[] = "0";
        command_settime(NULL, "settime", midnight);
        ck_assert_uint_eq(todtick, 5 * HOURS_PER_MONTH + HOURS_PER_DAY);
        ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
        ck_assert_int_lt(GET_MAP_SPACE_PTR(map, x, y)->celestial_light_value, day);
    }
    todtick = saved_hour;
}
END_TEST

/* Field digests of the authored Brynknot stacks, recorded from the solver
 * before it moved to libatrinik: the shared solver must reproduce the
 * gameplay field bit for bit. */
static const char *const golden_paths[] = {
    "/shattered_islands/world_0_67", "/shattered_islands/world_1_67",
    "/shattered_islands/world_2_67", "/shattered_islands/world_0_68",
    "/shattered_islands/world_1_68", "/shattered_islands/world_2_68",
    "/shattered_islands/world_0_69", "/shattered_islands/world_1_69",
    "/shattered_islands/world_2_69", "/shattered_islands/world_0_70",
    "/shattered_islands/world_1_70", "/shattered_islands/world_2_70",
    "/shattered_islands/world_3_67",
};
static const uint64_t golden_hours[] = {
    5 * HOURS_PER_MONTH + 6, 5 * HOURS_PER_MONTH + 9, 5 * HOURS_PER_MONTH + 15,
    5 * HOURS_PER_MONTH + 19, 2 * HOURS_PER_MONTH + 12, 11 * HOURS_PER_MONTH + 1,
};
static const uint64_t golden_digests[arraysize(golden_paths)][arraysize(golden_hours)] = {
    {UINT64_C(0x4225708f321ded64), UINT64_C(0x454ae0a5a6f917e6), UINT64_C(0xaa24e93f24740727), UINT64_C(0xfe89bbaed6905aa2), UINT64_C(0x451aa037a6f40b27), UINT64_C(0xfe0dc959e1dff2b3)},
    {UINT64_C(0x458ed99dd2d4ea5b), UINT64_C(0x68aada83ad8b826a), UINT64_C(0xd16c38a2d6207e82), UINT64_C(0xeb84daac8d5e8405), UINT64_C(0x151505011c47810c), UINT64_C(0xdab6a9cc5c150dc2)},
    {UINT64_C(0x6f408806f9dbda43), UINT64_C(0xa152cfa3c43b96d7), UINT64_C(0x49b5aedf710a5ae5), UINT64_C(0xefaa86c856f34c25), UINT64_C(0x19c539f528926746), UINT64_C(0xc19d99774b5917d3)},
    {UINT64_C(0xbc7c54f3c3704eab), UINT64_C(0xceea5b74da7b09ed), UINT64_C(0x1f794a8e865f7f7c), UINT64_C(0xabc5d4dd793d4373), UINT64_C(0xf126635723eb302a), UINT64_C(0x78700ee1b6154e12)},
    {UINT64_C(0x5c6cd06ba8850d98), UINT64_C(0xcb4fe3b0d2918cd3), UINT64_C(0xef19064e866151cb), UINT64_C(0xacbae3c5bc380436), UINT64_C(0x1a34323fad05a9d0), UINT64_C(0xba76396fe3aa7383)},
    {UINT64_C(0x81b9afd9c315dd1a), UINT64_C(0x9018a91ff49544c0), UINT64_C(0xd7d369f37a25155b), UINT64_C(0x382e91071670c400), UINT64_C(0x34f5efd0faac881e), UINT64_C(0x84841700ba716463)},
    {UINT64_C(0x2b586d96ca9e6133), UINT64_C(0xb905f6c6a2494ffc), UINT64_C(0x9efdcb7d905bd517), UINT64_C(0x685d11a499c64f03), UINT64_C(0xca5e0527a66f57fb), UINT64_C(0x8bb8ef4a6d9ecbc3)},
    {UINT64_C(0x8aab6fd251e47013), UINT64_C(0xd99d2a21489ecf57), UINT64_C(0x0502e087eedc3254), UINT64_C(0x0863a764d67ebab5), UINT64_C(0x9c560b8fcfc2d74d), UINT64_C(0x22b5c51cb5a905f3)},
    {UINT64_C(0x0e2211a931cfeb83), UINT64_C(0x4c0ef20da3bf3483), UINT64_C(0xc0216f5f7ddebe83), UINT64_C(0x6b11637c07750f83), UINT64_C(0xd3405ecfbe3bd883), UINT64_C(0x916d40fdd3f6d383)},
    {UINT64_C(0x0e2211a931cfeb83), UINT64_C(0x4c0ef20da3bf3483), UINT64_C(0xc0216f5f7ddebe83), UINT64_C(0x6b11637c07750f83), UINT64_C(0xd3405ecfbe3bd883), UINT64_C(0x916d40fdd3f6d383)},
    {UINT64_C(0x0e2211a931cfeb83), UINT64_C(0x4c0ef20da3bf3483), UINT64_C(0xc0216f5f7ddebe83), UINT64_C(0x6b11637c07750f83), UINT64_C(0xd3405ecfbe3bd883), UINT64_C(0x916d40fdd3f6d383)},
    {UINT64_C(0x0e2211a931cfeb83), UINT64_C(0x4c0ef20da3bf3483), UINT64_C(0xc0216f5f7ddebe83), UINT64_C(0x6b11637c07750f83), UINT64_C(0xd3405ecfbe3bd883), UINT64_C(0x916d40fdd3f6d383)},
    {UINT64_C(0x0e2211a931cfeb83), UINT64_C(0x4c0ef20da3bf3483), UINT64_C(0xc0216f5f7ddebe83), UINT64_C(0x6b11637c07750f83), UINT64_C(0xd3405ecfbe3bd883), UINT64_C(0x916d40fdd3f6d383)},
};

static uint64_t golden_stack_digest(mapstruct *map) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (mapstruct *level = map; level != NULL; level = level->tile_map[TILED_UP]) {
        for (int i = 0; i < level->width * level->height; i++) {
            const MapSpace *space = &level->spaces[i];
            int32_t words[4] = {space->celestial_light_value, space->celestial_light_rgb[0],
                                space->celestial_light_rgb[1], space->celestial_light_rgb[2]};
            for (size_t w = 0; w < 4; w++) {
                for (size_t b = 0; b < 4; b++) {
                    hash ^= (uint8_t)((uint32_t)words[w] >> (8 * b));
                    hash *= UINT64_C(1099511628211);
                }
            }
        }
    }
    return hash;
}

START_TEST(test_brynknot_fields_match_golden_digests) {
    (void)celestial_override_clear();
    bool print = getenv("ATRINIK_CELESTIAL_GOLDEN_PRINT") != NULL;
    for (size_t i = 0; i < arraysize(golden_paths); i++) {
        mapstruct *map = ready_map_name(golden_paths[i], NULL, MAP_NO_DYNAMIC);
        ck_assert_msg(map != NULL, "Could not load %s", golden_paths[i]);
        mapstruct *upper = map;
        while (upper->tile_path[TILED_UP] != NULL) {
            upper = get_map_from_tiled(upper, TILED_UP);
            ck_assert_ptr_nonnull(upper);
        }
        for (size_t h = 0; h < arraysize(golden_hours); h++) {
            ck_assert_msg(celestial_light_rebuild(map, golden_hours[h]), "%s hour %d failed",
                          golden_paths[i], (int)h);
            uint64_t digest = golden_stack_digest(map);
            if (print) {
                fprintf(stderr, "GOLDEN %zu %zu UINT64_C(0x%016" PRIx64 ")\n", i, h, digest);
            } else {
                ck_assert_msg(digest == golden_digests[i][h], "%s hour %zu digest %016" PRIx64,
                              golden_paths[i], h, digest);
            }
        }
    }
}
END_TEST

START_TEST(test_real_wilderness_midnight_full_and_new_moon_samples) {
    mapstruct *map = ready_map_name("/shattered_islands/world_3_67", NULL, MAP_NO_DYNAMIC);
    ck_assert_ptr_nonnull(map);
    unsigned long saved_hour = todtick;
    todtick = 5 * HOURS_PER_MONTH + 15;
    (void)celestial_override_clear();
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
    char midnight[] = "0";
    command_settime(NULL, "settime", midnight);
    ck_assert(celestial_override_set_phase(CELESTIAL_LUNAR_FULL));
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
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

START_TEST(test_celestial_elevation_bounds_shadow_and_requires_proven_sky) {
    mapstruct *map = open_fixture(40, 1);
    object *wall = arch_get("wall_wood_1");
    ck_assert_ptr_nonnull(wall);
    wall->x = 35;
    wall->y = 0;
    ck_assert_ptr_nonnull(object_insert_map(wall, map, NULL, 0));

    ck_assert(celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 7));
    /* tan(elevation)=8481/sqrt(32768^2-8481^2): two horizontal
     * crossings precede the top face. The receiving wall is illuminated. */
    for (int x = 35; x <= 37; x++) {
        ck_assert_int_eq(GET_MAP_SPACE_PTR(map, x, 0)->celestial_light_value, 378);
    }
    for (int x = 33; x <= 34; x++) {
        ck_assert_int_eq(GET_MAP_SPACE_PTR(map, x, 0)->celestial_light_value, 130);
    }
    for (int x = 0; x <= 32; x++) {
        ck_assert_int_eq(GET_MAP_SPACE_PTR(map, x, 0)->celestial_light_value, 378);
    }
    /* Missing horizontal neighbors cannot manufacture a sky exit. */
    for (int x = 38; x <= 39; x++) {
        ck_assert_int_eq(GET_MAP_SPACE_PTR(map, x, 0)->celestial_light_value, 130);
    }
    /* Zenith has no horizontal crossing, so the same wall casts no
     * sideways shadow and the map edge remains a proven upward exit. */
    ck_assert(celestial_light_rebuild(map, 5 * HOURS_PER_MONTH + 12));
    for (int x = 0; x < 40; x++) {
        ck_assert_int_eq(GET_MAP_SPACE_PTR(map, x, 0)->celestial_light_value, 1281);
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

    ck_assert(celestial_light_keyframe_ensure(map, hour));
    ck_assert(map->celestial_light_keyframe_valid);
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
    uint64_t initial_revision;

    (void)celestial_override_clear();
    ck_assert(celestial_light_rebuild(map, 0));
    ck_assert_int_eq(space->celestial_light_value, 2);
    initial_revision = celestial_override_revision();

    ck_assert(celestial_override_set_phase(CELESTIAL_LUNAR_FULL));
    ck_assert_uint_gt(celestial_override_revision(), initial_revision);
    celestial_light_invalidate_all();
    ck_assert(celestial_light_rebuild(map, 0));
    ck_assert_int_eq(space->celestial_light_value, 22);

    ck_assert(celestial_override_set_age(0, HOURS_PER_MONTH));
    celestial_light_invalidate_all();
    ck_assert(celestial_light_rebuild(map, 0));
    ck_assert_int_eq(space->celestial_light_value, 2);

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

    (void)celestial_override_clear();
    ck_assert(celestial_light_keyframe_ensure(map, hour));
    ck_assert(map->celestial_light_valid);
    ck_assert(map->celestial_light_keyframe_valid);
    key = map->celestial_light_key;

    ck_assert(celestial_override_set_phase(CELESTIAL_LUNAR_FULL));
    celestial_light_invalidate_all();
    ck_assert(map->celestial_light_valid);
    ck_assert_uint_eq(map->celestial_light_key, key);
    ck_assert(!map->celestial_light_keyframe_valid);
    ck_assert_uint_eq(map->celestial_light_next_key, 0);

    ck_assert(celestial_light_keyframe_ensure(map, hour));
    ck_assert(map->celestial_light_keyframe_valid);
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

START_TEST(test_celestial_geometry_signature_is_exact_and_uses_existing_padding) {
    mapstruct *map = open_fixture(1, 1);
    ck_assert(!celestial_light_geometry_update(map, 0, 0));
    object *wall = arch_get("wall_wood_1");
    ck_assert_ptr_nonnull(wall);
    ck_assert_ptr_nonnull(object_insert_map(wall, map, NULL, 0));
    /* The production object_update may already have cached this signature. */
    celestial_light_geometry_update(map, 0, 0);
    uint16_t opaque = map->spaces[0].celestial_geometry;
    object_set_value(wall, "celestial_transmission", "glass", 1);
    ck_assert(celestial_light_geometry_update(map, 0, 0));
    uint16_t glass = map->spaces[0].celestial_geometry;
    object_set_value(wall, "celestial_transmission", "grate", 1);
    ck_assert(celestial_light_geometry_update(map, 0, 0));
    uint16_t grate = map->spaces[0].celestial_geometry;
    object_set_value(wall, "celestial_transmission", "open", 1);
    ck_assert(celestial_light_geometry_update(map, 0, 0));
    uint16_t open = map->spaces[0].celestial_geometry;
    ck_assert_uint_ne(opaque, glass);
    ck_assert_uint_ne(glass, grate);
    ck_assert_uint_ne(grate, open);
    ck_assert(!celestial_light_geometry_update(map, 0, 0));
    object_set_value(wall, "celestial_transmission", "opaque", 1);
    ck_assert(celestial_light_geometry_update(map, 0, 0));
    /* Same opaque coefficient, but removing its aperture identity matters. */
    object_set_value(wall, "celestial_transmission", NULL, 0);
    ck_assert(celestial_light_geometry_update(map, 0, 0));
    ck_assert_uint_eq(map->spaces[0].celestial_geometry, opaque);
    ck_assert(!celestial_light_geometry_update(map, 0, 0));
}
END_TEST

START_TEST(test_celestial_pending_memory_is_bounded_and_released) {
    mapstruct *map = open_fixture(64, 64);
    celestial_light_ensure(map);
    int32_t current = map->spaces[0].celestial_light_value;
    map->spaces[0].celestial_light_next_value = 123;
    celestial_light_pending_memory_limit_for_test(1);
    request_next(map);
    ck_assert_uint_eq(celestial_light_pending_memory_for_test(), 0);
    ck_assert(!map->celestial_light_keyframe_valid);
    ck_assert_int_eq(map->spaces[0].celestial_light_value, current);
    ck_assert_int_eq(map->spaces[0].celestial_light_next_value, 123);
    celestial_light_pending_memory_limit_for_test(1923584);
    request_next(map);
    size_t required = celestial_light_pending_memory_for_test();
    ck_assert_uint_gt(required, 45 * 4096);
    ck_assert_uint_le(required, 1923584);
    celestial_light_forget(map);
    ck_assert_uint_eq(celestial_light_pending_memory_for_test(), 0);
    celestial_light_pending_memory_limit_for_test(required - 1);
    request_next(map);
    ck_assert_uint_eq(celestial_light_pending_memory_for_test(), 0);
    celestial_light_pending_memory_limit_for_test(required);
    request_next(map);
    mapstruct *second = open_fixture(64, 64);
    celestial_light_ensure(second);
    int32_t second_current = second->spaces[0].celestial_light_value;
    request_next(second);
    ck_assert_uint_eq(celestial_light_pending_memory_for_test(), required);
    ck_assert_uint_eq(celestial_light_pending_steps_for_test(second), 0);
    ck_assert(!second->celestial_light_keyframe_valid);
    ck_assert_int_eq(second->spaces[0].celestial_light_value, second_current);
    celestial_light_process_steps_for_test(1000000);
    ck_assert(map->celestial_light_keyframe_valid);
    ck_assert_uint_eq(celestial_light_pending_memory_for_test(), 0);
    request_next(second);
    ck_assert_uint_eq(celestial_light_pending_memory_for_test(), required);
    celestial_light_process_steps_for_test(1000000);
    ck_assert(second->celestial_light_keyframe_valid);
    ck_assert_uint_eq(celestial_light_pending_memory_for_test(), 0);
    celestial_light_pending_memory_limit_for_test(1923584);
}
END_TEST

START_TEST(test_celestial_pending_matches_synchronous_oracle) {
    mapstruct *map = open_fixture(40, 7);
    object *wall = arch_get("wall_wood_1");
    ck_assert_ptr_nonnull(wall);
    wall->x = 35;
    wall->y = 3;
    ck_assert_ptr_nonnull(object_insert_map(wall, map, NULL, 0));
    unsigned long saved_hour = todtick;
    /* Exercise all ray directions, daylight, twilight and lunar transport. */
    for (unsigned int hour = 0; hour < 24; hour++) {
        todtick = 5 * HOURS_PER_MONTH + hour;
        celestial_light_invalidate(map);
        request_next(map);
        int32_t current = map->spaces[0].celestial_light_value;
        uint64_t builds = celestial_light_rebuilds_for_test();
        celestial_light_process_steps_for_test(100);
        ck_assert(!map->celestial_light_keyframe_valid);
        ck_assert_int_eq(map->spaces[0].celestial_light_value, current);
        celestial_light_process_steps_for_test(100000);
        request_next(map);
        ck_assert(map->celestial_light_keyframe_valid);
        ck_assert_uint_eq(celestial_light_rebuilds_for_test(), builds);
        ck_assert(celestial_light_rebuild(map, (uint64_t)todtick + 1));
        for (size_t cell = 0; cell < (size_t)map->width * map->height; cell++) {
            ck_assert_int_eq(map->spaces[cell].celestial_light_value,
                             map->spaces[cell].celestial_light_next_value);
            ck_assert_int_eq(memcmp(map->spaces[cell].celestial_light_rgb,
                                    map->spaces[cell].celestial_light_next_rgb,
                                    sizeof(map->spaces[cell].celestial_light_rgb)), 0);
        }
    }
    todtick = saved_hour;
}
END_TEST

START_TEST(test_celestial_pending_promotes_without_rebuild_and_jumps_fall_back) {
    mapstruct *map = open_fixture(8, 8);
    unsigned long saved_hour = todtick;
    todtick = 5 * HOURS_PER_MONTH + 7;
    request_next(map);
    celestial_light_process_steps_for_test(100000);
    ck_assert(map->celestial_light_keyframe_valid);
    int32_t next = map->spaces[0].celestial_light_next_value;
    uint64_t builds = celestial_light_rebuilds_for_test();
    todtick++;
    request_next(map);
    ck_assert_uint_eq(celestial_light_rebuilds_for_test(), builds);
    ck_assert_int_eq(map->spaces[0].celestial_light_value, next);
    /* An unfinished job cannot publish after a discontinuous clock jump. */
    celestial_light_process_steps_for_test(100);
    todtick += 5;
    celestial_light_process_steps_for_test(100000);
    ck_assert(!map->celestial_light_keyframe_valid);
    ck_assert_uint_eq(celestial_light_pending_memory_for_test(), 0);
    request_next(map);
    ck_assert_uint_eq(celestial_light_rebuilds_for_test(), builds + 1);
    celestial_light_forget(map);
    todtick = saved_hour;
}
END_TEST

START_TEST(test_celestial_pending_is_fair_and_geometry_cancels) {
    mapstruct *first = open_fixture(16, 16);
    mapstruct *second = open_fixture(16, 16);
    request_next(first);
    request_next(second);
    celestial_light_process_steps_for_test(20);
    ck_assert_uint_eq(celestial_light_pending_steps_for_test(first), 10);
    ck_assert_uint_eq(celestial_light_pending_steps_for_test(second), 10);
    celestial_light_invalidate(first);
    ck_assert_uint_eq(celestial_light_pending_steps_for_test(first), 0);
    celestial_light_process_steps_for_test(100000);
    ck_assert(!first->celestial_light_keyframe_valid);
    ck_assert(second->celestial_light_keyframe_valid);
    request_next(first);
    celestial_light_process_steps_for_test(20);
    free_map(first, 1);
    celestial_light_process_steps_for_test(100000);
    ck_assert_uint_eq(celestial_light_pending_steps_for_test(first), 0);
    ck_assert_uint_eq(celestial_light_pending_memory_for_test(), 0);
}
END_TEST

START_TEST(test_celestial_pending_stack_exact_and_membership_fenced) {
    mapstruct *lower = open_fixture(5, 5);
    mapstruct *upper = open_fixture(5, 5);
    FREE_AND_COPY_HASH(lower->path, "/pending/lower");
    FREE_AND_COPY_HASH(upper->path, "/pending/upper");
    FREE_AND_COPY_HASH(lower->tile_path[TILED_UP], upper->path);
    FREE_AND_COPY_HASH(upper->tile_path[TILED_DOWN], lower->path);
    lower->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = lower;
    lower->celestial_sky_above = CELESTIAL_SKY_LINKED;
    lower->celestial_boundary[TILED_UP] = CELESTIAL_BOUNDARY_CONTINUOUS;
    upper->celestial_boundary[TILED_DOWN] = CELESTIAL_BOUNDARY_CONTINUOUS;
    object *roof = arch_get("roof_thatch");
    ck_assert_ptr_nonnull(roof);
    roof->x = roof->y = 2;
    ck_assert_ptr_nonnull(object_insert_map(roof, upper, NULL, 0));
    request_next(lower);
    celestial_light_process_steps_for_test(100000);
    ck_assert(lower->celestial_light_keyframe_valid);
    ck_assert(upper->celestial_light_keyframe_valid);
    ck_assert(celestial_light_rebuild(lower, (uint64_t)todtick + 1));
    mapstruct *maps[] = {lower, upper};
    for (size_t level = 0; level < arraysize(maps); level++) {
        for (size_t cell = 0; cell < 25; cell++) {
            ck_assert_int_eq(maps[level]->spaces[cell].celestial_light_value,
                             maps[level]->spaces[cell].celestial_light_next_value);
            ck_assert_int_eq(memcmp(maps[level]->spaces[cell].celestial_light_rgb,
                                    maps[level]->spaces[cell].celestial_light_next_rgb,
                                    sizeof(maps[level]->spaces[cell].celestial_light_rgb)), 0);
        }
    }
    request_next(lower);
    celestial_light_process_steps_for_test(20);
    lower->tile_map[TILED_UP] = NULL;
    upper->tile_map[TILED_DOWN] = NULL;
    celestial_light_process_steps_for_test(100000);
    ck_assert(!lower->celestial_light_keyframe_valid);
    ck_assert_uint_eq(celestial_light_pending_steps_for_test(lower), 0);
    /* Restore a published pair, then unload its required upper dependency. */
    lower->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = lower;
    ck_assert(celestial_light_keyframe_ensure(lower, (uint64_t)todtick));
    free_map(upper, 1);
    request_next(lower);
    for (size_t cell = 0; cell < 25; cell++) {
        ck_assert_int_eq(lower->spaces[cell].celestial_light_value, 0);
        ck_assert_int_eq(lower->spaces[cell].celestial_light_next_value, 0);
    }
}
END_TEST

START_TEST(test_continuous_seam_matches_single_map_spill) {
    mapstruct *joined = open_fixture(8, 5);
    mapstruct *left = open_fixture(4, 5);
    mapstruct *right = open_fixture(4, 5);
    FREE_AND_COPY_HASH(left->path, "/halo/left");
    FREE_AND_COPY_HASH(right->path, "/halo/right");
    FREE_AND_COPY_HASH(left->tile_path[TILED_EAST], right->path);
    FREE_AND_COPY_HASH(right->tile_path[TILED_WEST], left->path);
    left->tile_map[TILED_EAST] = right;
    right->tile_map[TILED_WEST] = left;
    left->celestial_tile_path_seen[TILED_EAST] = true;
    right->celestial_tile_path_seen[TILED_WEST] = true;
    left->celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    right->celestial_boundary[TILED_WEST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    mapstruct *covered[] = {joined, left};
    for (size_t i = 0; i < arraysize(covered); i++) {
        covered[i]->celestial_rectangles = xcalloc(1, sizeof(celestial_rectangle_t));
        covered[i]->celestial_rectangle_count = 1;
        covered[i]->celestial_rectangles[0] = (celestial_rectangle_t){
            .type = CELESTIAL_RECT_SKY_COVERED, .width = 4, .height = 5};
    }
    uint64_t hour = 5 * HOURS_PER_MONTH + 12;
    ck_assert(celestial_light_rebuild(joined, hour));
    ck_assert(celestial_light_rebuild(left, hour));
    ck_assert(celestial_light_rebuild(right, hour));
    /* Covered cells receive only bounded diffuse spill from across the seam. */
    ck_assert_int_gt(GET_MAP_SPACE_PTR(left, 3, 2)->celestial_light_value, 0);
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 8; x++) {
            const MapSpace *expected = GET_MAP_SPACE_PTR(joined, x, y);
            const MapSpace *actual = GET_MAP_SPACE_PTR(x < 4 ? left : right, x % 4, y);
            ck_assert_int_eq(actual->celestial_light_value, expected->celestial_light_value);
            ck_assert_int_eq(memcmp(actual->celestial_light_rgb, expected->celestial_light_rgb,
                                    sizeof(actual->celestial_light_rgb)), 0);
        }
    }
    left->celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_DISCONTINUOUS;
    right->celestial_boundary[TILED_WEST] = CELESTIAL_BOUNDARY_DISCONTINUOUS;
    ck_assert(celestial_light_rebuild(left, hour));
    for (size_t cell = 0; cell < 20; cell++) {
        ck_assert_int_eq(left->spaces[cell].celestial_light_value, 0);
    }
}
END_TEST

START_TEST(test_remote_two_seam_geometry_fences_current_and_pending_fields) {
    mapstruct *maps[] = {open_fixture(2, 5), open_fixture(2, 5), open_fixture(2, 5)};
    mapstruct *unrelated = open_fixture(2, 5);
    const char *paths[] = {"/dependencies/a", "/dependencies/b", "/dependencies/c"};
    for (size_t i = 0; i < 3; i++) FREE_AND_COPY_HASH(maps[i]->path, paths[i]);
    for (size_t i = 0; i < 2; i++) {
        FREE_AND_COPY_HASH(maps[i]->tile_path[TILED_EAST], maps[i + 1]->path);
        FREE_AND_COPY_HASH(maps[i + 1]->tile_path[TILED_WEST], maps[i]->path);
        maps[i]->tile_map[TILED_EAST] = maps[i + 1];
        maps[i + 1]->tile_map[TILED_WEST] = maps[i];
        maps[i]->celestial_tile_path_seen[TILED_EAST] = true;
        maps[i + 1]->celestial_tile_path_seen[TILED_WEST] = true;
        maps[i]->celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_CONTINUOUS;
        maps[i + 1]->celestial_boundary[TILED_WEST] = CELESTIAL_BOUNDARY_CONTINUOUS;
        maps[i]->celestial_rectangles = xcalloc(1, sizeof(celestial_rectangle_t));
        maps[i]->celestial_rectangle_count = 1;
        maps[i]->celestial_rectangles[0] = (celestial_rectangle_t){
            .type = CELESTIAL_RECT_SKY_COVERED, .width = 2, .height = 5};
    }
    unsigned long saved_hour = todtick;
    todtick = 5 * HOURS_PER_MONTH + 12;
    celestial_light_ensure(maps[0]);
    celestial_light_ensure(unrelated);
    ck_assert_int_gt(maps[0]->spaces[5].celestial_light_value, 0);
    request_next(maps[0]);
    celestial_light_process_steps_for_test(200);
    maps[2]->celestial_rectangles = xcalloc(1, sizeof(celestial_rectangle_t));
    maps[2]->celestial_rectangle_count = 1;
    maps[2]->celestial_rectangles[0] = (celestial_rectangle_t){
        .type = CELESTIAL_RECT_SKY_COVERED, .width = 2, .height = 5};
    celestial_light_invalidate(maps[2]);
    celestial_light_process_steps_for_test(100000);
    ck_assert(!maps[0]->celestial_light_keyframe_valid);
    celestial_light_ensure(maps[0]);
    for (size_t cell = 0; cell < 10; cell++) {
        ck_assert_int_eq(maps[0]->spaces[cell].celestial_light_value, 0);
    }
    uint64_t builds = celestial_light_rebuilds_for_test();
    celestial_light_ensure(unrelated);
    ck_assert_uint_eq(celestial_light_rebuilds_for_test(), builds);
    todtick = saved_hour;
}
END_TEST

START_TEST(test_current_reservation_failure_clears_both_published_endpoints) {
    mapstruct *map = open_fixture(9, 9);
    unsigned long saved_hour = todtick;
    todtick = 5 * HOURS_PER_MONTH + 12;
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
    ck_assert_int_gt(map->spaces[40].celestial_light_value, 0);
    ck_assert_int_gt(map->spaces[40].celestial_light_next_value, 0);
    map->celestial_sky_above = CELESTIAL_SKY_SEALED;
    celestial_light_invalidate(map);
    celestial_light_current_reservation_fail_for_test(true);
    celestial_light_ensure(map);
    celestial_light_current_reservation_fail_for_test(false);
    ck_assert(!map->celestial_light_valid);
    ck_assert(!map->celestial_light_keyframe_valid);
    for (size_t i = 0; i < 81; i++) {
        ck_assert_int_eq(map->spaces[i].celestial_light_value, 0);
        ck_assert_int_eq(map->spaces[i].celestial_light_next_value, 0);
        for (size_t channel = 0; channel < 3; channel++) {
            ck_assert_int_eq(map->spaces[i].celestial_light_rgb[channel], 0);
            ck_assert_int_eq(map->spaces[i].celestial_light_next_rgb[channel], 0);
        }
    }
    celestial_light_ensure(map);
    ck_assert(map->celestial_light_valid);
    ck_assert_int_eq(map->spaces[40].celestial_light_value, 0);
    map->celestial_sky_above = CELESTIAL_SKY_OPEN;
    celestial_light_invalidate(map);
    ck_assert(celestial_light_keyframe_ensure(map, (uint64_t)todtick));
    ck_assert_int_gt(map->spaces[40].celestial_light_value, 0);
    ck_assert_int_gt(map->spaces[40].celestial_light_next_value, 0);
    todtick = saved_hour;
}
END_TEST


START_TEST(test_celestial_structure_revision_reaches_max_without_wrapping) {
    mapstruct *map = open_fixture(1, 1), *target = map;
    if (_i == 1) {
        target = open_fixture(1, 1);
        map->tile_map[TILED_DOWN] = target;
        target->tile_map[TILED_UP] = map;
    }
    target->celestial_structure_revision = UINT64_MAX - 1;
    celestial_light_invalidate(map);
    uint64_t revision = target->celestial_structure_revision;
    target->celestial_structure_revision = 0;
    ck_assert_uint_eq(revision, UINT64_MAX);
}
END_TEST

START_TEST(test_celestial_upper_revision_overflow_is_fatal) {
    mapstruct *map = open_fixture(1, 1);
    map->celestial_structure_revision = UINT64_MAX;
    celestial_light_invalidate(map);
    exit(EXIT_SUCCESS);
}
END_TEST

START_TEST(test_celestial_lower_revision_overflow_is_fatal) {
    mapstruct *map = open_fixture(1, 1), *lower = open_fixture(1, 1);
    map->tile_map[TILED_DOWN] = lower;
    lower->tile_map[TILED_UP] = map;
    lower->celestial_structure_revision = UINT64_MAX;
    celestial_light_invalidate(map);
    exit(EXIT_SUCCESS);
}
END_TEST

START_TEST(test_celestial_pending_batch_reaches_max_without_wrapping) {
    uint64_t saved = celestial_light_pending_batch_exchange_for_test(UINT64_MAX - 1);
    celestial_light_process_pending(1);
    uint64_t batch = celestial_light_pending_batch_exchange_for_test(saved);
    ck_assert_uint_eq(batch, UINT64_MAX);
}
END_TEST

START_TEST(test_celestial_pending_batch_overflow_is_fatal) {
    (void)celestial_light_pending_batch_exchange_for_test(UINT64_MAX);
    celestial_light_process_pending(1);
    exit(EXIT_SUCCESS);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("celestial_light");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_loop_test(tc_core, test_celestial_structure_revision_reaches_max_without_wrapping,
                       0, 2);
    tcase_add_test(tc_core, test_celestial_pending_batch_reaches_max_without_wrapping);
#ifdef NDEBUG
#define ADD_OVERFLOW_TEST(name) tcase_add_exit_test(tc_core, name, EXIT_FAILURE)
#else
#define ADD_OVERFLOW_TEST(name) tcase_add_test_raise_signal(tc_core, name, SIGABRT)
#endif
    ADD_OVERFLOW_TEST(test_celestial_upper_revision_overflow_is_fatal);
    ADD_OVERFLOW_TEST(test_celestial_lower_revision_overflow_is_fatal);
    ADD_OVERFLOW_TEST(test_celestial_pending_batch_overflow_is_fatal);
#undef ADD_OVERFLOW_TEST
    TCase *tc_real_maps = tcase_create("Real maps");
    tcase_add_unchecked_fixture(tc_real_maps, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_real_maps, check_test_setup, check_test_teardown);
    /* Keep all 13 authored maps and both clock endpoints under instrumentation. */
    tcase_set_timeout(tc_real_maps, 120);
    suite_add_tcase(s, tc_real_maps);
    TCase *tc_memory = tcase_create("Pending memory");
    tcase_add_unchecked_fixture(tc_memory, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_memory, check_test_setup, check_test_teardown);
    /* Two 64x64 maps complete current and capped pending solves under sanitizers. */
    tcase_set_timeout(tc_memory, 60);
    suite_add_tcase(s, tc_memory);
    TCase *tc_oracle = tcase_create("Pending oracle");
    tcase_add_unchecked_fixture(tc_oracle, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_oracle, check_test_setup, check_test_teardown);
    /* Keep all 24 full-field comparisons under combined coverage/sanitizers. */
    tcase_set_timeout(tc_oracle, 60);
    suite_add_tcase(s, tc_oracle);
    tcase_add_test(tc_core, test_celestial_open_field_matches_daylight_anchor);
    tcase_add_test(tc_core, test_continuous_seam_matches_single_map_spill);
    tcase_add_test(tc_core, test_remote_two_seam_geometry_fences_current_and_pending_fields);
    tcase_add_test(tc_core, test_discontinuous_horizontal_neighbor_is_not_a_local_light_dependency);
    tcase_add_test(tc_real_maps, test_brynknot_and_outside_fields_follow_settime);
    tcase_add_test(tc_real_maps, test_brynknot_fields_match_golden_digests);
    tcase_add_test(tc_core, test_unloaded_lower_map_does_not_gate_upper_light);
    tcase_add_test(tc_core, test_real_wilderness_midnight_full_and_new_moon_samples);
    tcase_add_test(tc_core, test_derived_horizontal_neighbors_preserve_day_and_lunar_light);
    tcase_add_test(tc_core, test_celestial_elevation_bounds_shadow_and_requires_proven_sky);
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
    tcase_add_test(tc_oracle, test_celestial_pending_matches_synchronous_oracle);
    tcase_add_test(tc_core, test_celestial_geometry_signature_is_exact_and_uses_existing_padding);
    tcase_add_test(tc_memory, test_celestial_pending_memory_is_bounded_and_released);
    tcase_add_test(tc_core, test_current_reservation_failure_clears_both_published_endpoints);
    tcase_add_test(tc_core, test_celestial_pending_promotes_without_rebuild_and_jumps_fall_back);
    tcase_add_test(tc_core, test_celestial_pending_is_fair_and_geometry_cancels);
    tcase_add_test(tc_core, test_celestial_pending_stack_exact_and_membership_fenced);
    return s;
}

void check_server_celestial_light(void) {
    check_run_suite(suite(), __FILE__);
}
