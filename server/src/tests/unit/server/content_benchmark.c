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

#include <check.h>
#include <check_utils.h>
#include <checkstd.h>
#include <content_benchmark.h>
#include <walking_route.h>
#include <arch.h>
#include <map.h>
#include <object.h>
#include <object_methods.h>
#include <player.h>
#include <toolkit/path.h>
#include <toolkit/string.h>

START_TEST(test_map_list_accepts_canonical_unique_logical_ids) {
    ck_assert(
        content_benchmark_maps_valid("/start,/shattered_islands/world_1_1,/plane/map-name.2"));
}
END_TEST

START_TEST(test_map_list_rejects_ambiguous_or_unsafe_ids) {
    static const char *invalid[] = {
        NULL,
        "",
        "relative",
        "/",
        "/trailing/",
        "/double//separator",
        "/dot/./component",
        "/parent/../component",
        "/windows\\separator",
        "/windows:drive",
        "/space in/name",
        "/duplicate,/duplicate",
        ",/leading",
        "/trailing,",
        "/empty,,/component",
    };

    for (size_t i = 0; i < arraysize(invalid); i++) {
        ck_assert(!content_benchmark_maps_valid(invalid[i]));
    }
}
END_TEST

START_TEST(test_map_list_enforces_count_and_component_bounds) {
    ck_assert(!content_benchmark_maps_valid("/a,/b,/c,/d,/e,/f,/g,/h,/i,/j,/k,/l,/m,/n,/o,/p,/q"));

    char oversized[MAX_BUF + 2];
    oversized[0] = '/';
    memset(oversized + 1, 'a', MAX_BUF);
    oversized[MAX_BUF + 1] = '\0';
    ck_assert(!content_benchmark_maps_valid(oversized));
}
END_TEST

/* Real map storage and normal collision flags, with explicit native tiled
 * links. Fixtures deliberately avoid account/player persistence and sockets. */
static object *route_fixture(mapstruct *maps[WALKING_ROUTE_MAPS]) {
    static const int dx[] = {0, 1, 0, -1, 1, 1, -1, -1};
    static const int dy[] = {-1, 0, 1, 0, -1, 1, 1, -1};
    for (int i = 0; i < WALKING_ROUTE_MAPS; i++) {
        maps[i] = get_empty_map(24, 24);
        ck_assert_ptr_nonnull(maps[i]);
        /* Normal spawned-monster base info retains its home map identity. */
        char path[MAX_BUF];
        snprintf(VS(path), "/tests/walking-route/world_%d_%d", i % 4, i / 4 + 66);
        maps[i]->path = add_string(path);
        for (int y = 0; y < 24; y++) {
            for (int x = 0; x < 24; x++) {
                object *floor = arch_get("floor_cave1");
                ck_assert_int_eq(floor->layer, LAYER_FLOOR);
                floor->x = x;
                floor->y = y;
                ck_assert_ptr_nonnull(object_insert_map(floor, maps[i], NULL, 0));
            }
        }
    }
    for (int i = 0; i < WALKING_ROUTE_MAPS; i++) {
        for (int dir = 0; dir < 8; dir++) {
            int x = i % 4 + dx[dir], y = i / 4 + dy[dir];
            if (x >= 0 && x < 4 && y >= 0 && y < 5) {
                maps[i]->tile_map[dir] = maps[y * 4 + x];
            }
        }
    }
    object *human = walking_route_candidate_create(arch_find("human_male"));
    ck_assert_ptr_nonnull(human);
    return human;
}

static void route_fixture_destroy(mapstruct *maps[WALKING_ROUTE_MAPS], object *human) {
    object_destroy(human);
    for (int i = 0; i < WALKING_ROUTE_MAPS; i++) {
        memset(maps[i]->tile_map, 0, sizeof(maps[i]->tile_map));
    }
    for (int i = 0; i < WALKING_ROUTE_MAPS; i++) {
        delete_map(maps[i]);
    }
}

START_TEST(test_walking_route_normal_geometry_coverage_and_owned_cleanup) {
    mapstruct *maps[WALKING_ROUTE_MAPS];
    object *human = route_fixture(maps);
    /* Route must go around a no-pass wall, water, closed door and exit. */
    SET_MAP_FLAGS(maps[16], 19, 8, P_NO_PASS);
    GET_MAP_SPACE_PTR(maps[16], 19, 9)->move_flags = TERRAIN_WATERWALK;
    SET_MAP_FLAGS(maps[16], 20, 9, P_DOOR_CLOSED);
    SET_MAP_FLAGS(maps[16], 21, 9, P_IS_EXIT);
    SET_MAP_FLAGS(maps[16], 21, 8, P_WALK_ON);
    SET_MAP_FLAGS(maps[16], 19, 7, P_WALK_OFF);
    object *missing_floor = GET_MAP_OB_LAYER(maps[16], 20, 7, LAYER_FLOOR, 0);
    ck_assert_ptr_nonnull(missing_floor);
    object_remove(missing_floor, REMOVE_NO_WALK_OFF);
    object_destroy(missing_floor);
    walking_route_point *points;
    size_t count;
    ck_assert(walking_route_plan(maps, human, &points, &count));
    ck_assert_uint_gt(count, 1);
    ck_assert_uint_le(count, WALKING_ROUTE_LIMIT);
    ck_assert_int_eq(points[0].map, 16);
    ck_assert_int_eq(points[0].x, 20);
    ck_assert_int_eq(points[0].y, 8);
    ck_assert_int_eq(points[0].direction, 0);
    bool visited[WALKING_ROUTE_MAPS] = {false};
    static const int dx[] = {0, -1, 0, 1, -1, 0, 1, -1, 0, 1};
    static const int dy[] = {0, 1, 1, 1, 0, 0, 0, -1, -1, -1};
    for (size_t i = 0; i < count; i++) {
        walking_route_point p = points[i];
        ck_assert_uint_lt(p.map, WALKING_ROUTE_MAPS);
        ck_assert_uint_lt(p.x, 24);
        ck_assert_uint_lt(p.y, 24);
        visited[p.map] = true;
        ck_assert_int_eq(object_blocked(human, maps[p.map], p.x, p.y), 0);
        ck_assert_int_eq(GET_MAP_FLAGS(maps[p.map], p.x, p.y) &
                             (P_DOOR_CLOSED | P_IS_EXIT | P_WALK_ON | P_WALK_OFF),
                         0);
        ck_assert_ptr_nonnull(GET_MAP_OB_LAYER(maps[p.map], p.x, p.y, LAYER_FLOOR, 0));
        if (i > 0) {
            walking_route_point prev = points[i - 1];
            ck_assert_uint_ge(p.direction, 1);
            ck_assert_uint_le(p.direction, 9);
            ck_assert_int_ne(p.direction, 5);
            ck_assert_int_eq((p.map % 4) * 24 + p.x - ((prev.map % 4) * 24 + prev.x),
                             dx[p.direction]);
            ck_assert_int_eq((p.map / 4) * 24 + p.y - ((prev.map / 4) * 24 + prev.y),
                             dy[p.direction]);
        }
    }
    for (int y = 1; y < 5; y++) {
        for (int x = 0; x < 3; x++) {
            ck_assert(visited[y * 4 + x]);
        }
    }
    ck_assert(visited[0] && visited[1]);
    ck_assert_int_eq(points[count - 1].map, 6);
    ck_assert_int_eq(points[count - 1].x, 7);
    ck_assert_int_eq(points[count - 1].y, 23);
    free(points);
    route_fixture_destroy(maps, human);
}
END_TEST

START_TEST(test_walking_route_rejects_blocked_start_finish_and_missing_coverage) {
    mapstruct *maps[WALKING_ROUTE_MAPS];
    object *human = route_fixture(maps);
    walking_route_point *points = (void *)1;
    size_t count = 99;
    SET_MAP_FLAGS(maps[16], 20, 8, P_NO_PASS);
    ck_assert(!walking_route_plan(maps, human, &points, &count));
    ck_assert_ptr_null(points);
    ck_assert_uint_eq(count, 0);
    SET_MAP_FLAGS(maps[16], 20, 8, 0);
    SET_MAP_FLAGS(maps[6], 7, 23, P_NO_PASS);
    ck_assert(!walking_route_plan(maps, human, &points, &count));
    ck_assert_ptr_null(points);
    ck_assert_uint_eq(count, 0);
    SET_MAP_FLAGS(maps[6], 7, 23, 0);
    for (int y = 0; y < 24; y++) {
        for (int x = 0; x < 24; x++) {
            SET_MAP_FLAGS(maps[8], x, y, P_NO_PASS);
        }
    }
    ck_assert(!walking_route_plan(maps, human, &points, &count));
    ck_assert_ptr_null(points);
    ck_assert_uint_eq(count, 0);
    route_fixture_destroy(maps, human);
}
END_TEST

START_TEST(test_walking_route_rejects_false_world_seams_and_invalid_dimensions) {
    mapstruct *maps[WALKING_ROUTE_MAPS];
    object *human = route_fixture(maps);
    walking_route_point *points;
    size_t count;
    /* Isolate required chunk 8 by pointing every inbound seam at the wrong
     * loaded map. A graph based only on filenames would incorrectly pass. */
    for (int i = 0; i < WALKING_ROUTE_MAPS; i++) {
        for (int dir = 0; dir < 8; dir++) {
            if (maps[i]->tile_map[dir] == maps[8]) {
                maps[i]->tile_map[dir] = maps[i];
            }
        }
    }
    ck_assert(!walking_route_plan(maps, human, &points, &count));
    ck_assert_ptr_null(points);
    ck_assert_uint_eq(count, 0);
    maps[0]->width = 23;
    ck_assert(!walking_route_plan(maps, human, &points, &count));
    maps[0]->width = 24;
    SET_FLAG(human, FLAG_FLYING);
    ck_assert(!walking_route_plan(maps, human, &points, &count));
    route_fixture_destroy(maps, human);
}
END_TEST

START_TEST(test_walking_route_rejects_floorless_and_walkoff_start) {
    mapstruct *maps[WALKING_ROUTE_MAPS];
    object *human = route_fixture(maps);
    walking_route_point *points;
    size_t count;
    SET_MAP_FLAGS(maps[16], 20, 8, P_WALK_OFF);
    ck_assert_int_eq(object_blocked(human, maps[16], 20, 8), 0);
    ck_assert(!walking_route_plan(maps, human, &points, &count));
    ck_assert_ptr_null(points);
    ck_assert_uint_eq(count, 0);
    SET_MAP_FLAGS(maps[16], 20, 8, 0);
    object *floor = GET_MAP_OB_LAYER(maps[16], 20, 8, LAYER_FLOOR, 0);
    ck_assert_ptr_nonnull(floor);
    object_remove(floor, REMOVE_NO_WALK_OFF);
    object_destroy(floor);
    maps[16]->tile_map[TILED_DOWN] = maps[0];
    ck_assert_ptr_nonnull(GET_MAP_OB_LAYER(maps[0], 20, 8, LAYER_FLOOR, 0));
    ck_assert_int_eq(object_blocked(human, maps[16], 20, 8), 0);
    ck_assert(!walking_route_plan(maps, human, &points, &count));
    ck_assert_ptr_null(points);
    ck_assert_uint_eq(count, 0);
    route_fixture_destroy(maps, human);
}
END_TEST

START_TEST(test_walking_route_reserves_authored_spawn_cells_before_live_activation) {
    mapstruct *maps[WALKING_ROUTE_MAPS];
    object *human = route_fixture(maps);
    object *spawn = arch_get("spawn_point");
    ck_assert_int_eq(spawn->type, SPAWN_POINT);
    spawn->x = 12;
    spawn->y = 12;
    ck_assert_ptr_nonnull(object_insert_map(spawn, maps[16], NULL, 0));
    object *template = arch_get("guard");
    template->type = SPAWN_POINT_MOB;
    ck_assert_ptr_nonnull(object_insert_into(template, spawn, 0));
    /* The offline tile is geometrically passable, but will hold a living
     * guard after normal spawn processing. The route must reserve it now. */
    ck_assert_int_eq(object_blocked(human, maps[16], 12, 12), 0);
    walking_route_point *points;
    size_t count;
    ck_assert(walking_route_plan(maps, human, &points, &count));
    for (size_t i = 0; i < count; i++) {
        ck_assert(!(points[i].map == 16 && points[i].x == 12 && points[i].y == 12));
    }
    free(points);
    object_process(spawn);
    ck_assert_ptr_nonnull(spawn->enemy);
    ck_assert_int_eq(spawn->enemy->x, 12);
    ck_assert_int_eq(spawn->enemy->y, 12);
    ck_assert_int_ne(object_blocked(human, maps[16], 12, 12), 0);
    /* A scenario starting on a reserved spawn cell must also fail honestly. */
    object *start_spawn = arch_get("spawn_point");
    start_spawn->x = 20;
    start_spawn->y = 8;
    ck_assert_ptr_nonnull(object_insert_map(start_spawn, maps[16], NULL, 0));
    ck_assert(!walking_route_plan(maps, human, &points, &count));
    ck_assert_ptr_null(points);
    ck_assert_uint_eq(count, 0);
    route_fixture_destroy(maps, human);
}
END_TEST

START_TEST(test_walking_route_candidate_rejects_missing_and_wrong_archetype) {
    ck_assert_ptr_null(walking_route_candidate_create(NULL));
    archetype_t *sword = arch_find("sword");
    ck_assert_ptr_nonnull(sword);
    ck_assert_ptr_null(walking_route_candidate_create(sword));
    object *human = walking_route_candidate_create(arch_find("human_male"));
    ck_assert_ptr_nonnull(human);
    ck_assert_ptr_nonnull(CONTR(human));
    ck_assert_ptr_eq(CONTR(human)->ob, human);
    ck_assert_ptr_null(human->map);
    ck_assert(QUERY_FLAG(human, FLAG_REMOVED));
    object_destroy(human);
}
END_TEST

static void map_list_setup(void) {
    /* The preceding route case shuts the server and toolkit down. Keep these
     * parser-only tests independent of test-case ordering and server state. */
    toolkit_import(string);
    toolkit_import(path);
}

static Suite *suite(void) {
    Suite *s = suite_create("content_benchmark");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, map_list_setup, toolkit_deinit);
    tcase_add_test(tc_core, test_map_list_accepts_canonical_unique_logical_ids);
    tcase_add_test(tc_core, test_map_list_rejects_ambiguous_or_unsafe_ids);
    tcase_add_test(tc_core, test_map_list_enforces_count_and_component_bounds);
    TCase *tc_route = tcase_create("Walking route");
    tcase_add_unchecked_fixture(tc_route, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_route, check_test_setup, check_test_teardown);
    tcase_add_test(tc_route, test_walking_route_normal_geometry_coverage_and_owned_cleanup);
    tcase_add_test(tc_route, test_walking_route_rejects_blocked_start_finish_and_missing_coverage);
    tcase_add_test(tc_route, test_walking_route_rejects_false_world_seams_and_invalid_dimensions);
    tcase_add_test(tc_route, test_walking_route_rejects_floorless_and_walkoff_start);
    tcase_add_test(tc_route, test_walking_route_candidate_rejects_missing_and_wrong_archetype);
    tcase_add_test(tc_route,
                   test_walking_route_reserves_authored_spawn_cells_before_live_activation);
    suite_add_tcase(s, tc_route);
    suite_add_tcase(s, tc_core);
    return s;
}

void check_server_content_benchmark(void) {
    check_run_suite(suite(), __FILE__);
}
