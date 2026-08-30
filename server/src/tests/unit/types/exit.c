/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <object.h>
#include <object_methods.h>
#include <plugin.h>
#include <player.h>
#include <exit.h>

static object *exit_test_insert_floor(mapstruct *map, int x, int y) {
    object *floor = arch_get("floor_orange1");
    ck_assert_ptr_nonnull(floor);
    floor->x = x;
    floor->y = y;
    floor = object_insert_map(floor, map, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(floor);
    return floor;
}

static object *exit_test_insert_explicit(mapstruct *source,
                                         mapstruct *destination,
                                         int source_x,
                                         int source_y,
                                         int destination_x,
                                         int destination_y) {
    object *exit = arch_get("stairs_down");
    ck_assert_ptr_nonnull(exit);
    exit->x = source_x;
    exit->y = source_y;
    FREE_AND_ADD_REF_HASH(EXIT_PATH(exit), destination->path);
    EXIT_X(exit) = destination_x;
    EXIT_Y(exit) = destination_y;
    exit = object_insert_map(exit, source, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(exit);
    return exit;
}

static void exit_test_block(mapstruct *map, int x, int y) {
    SET_MAP_FLAGS(map, x, y, GET_MAP_FLAGS(map, x, y) | P_NO_PASS);
}

START_TEST(test_exit_rejects_missing_floor_without_removing_applier) {
    mapstruct *source;
    object *pl;
    check_setup_env_pl(&source, &pl);
    FREE_AND_COPY_HASH(source->path, "/tests/exit-missing-floor-source");
    object_remove(pl, 0);
    pl->x = 2;
    pl->y = 2;
    pl = object_insert_map(pl, source, NULL, 0);

    mapstruct *destination = get_empty_map(24, 24);
    ck_assert_ptr_nonnull(destination);
    FREE_AND_COPY_HASH(destination->path, "/tests/exit-missing-floor-destination");
    object *exit = exit_test_insert_explicit(source, destination, 4, 4, 6, 6);

    ck_assert(!exit_has_usable_destination(exit));
    ck_assert_int_eq(object_apply(exit, pl, 0), OBJECT_METHOD_OK);
    ck_assert_ptr_eq(pl->map, source);
    ck_assert_int_eq(pl->x, 2);
    ck_assert_int_eq(pl->y, 2);
    ck_assert(!QUERY_FLAG(pl, FLAG_REMOVED));
}
END_TEST

START_TEST(test_exit_rejects_invalid_destination_coordinates) {
    mapstruct *source;
    object *pl;
    check_setup_env_pl(&source, &pl);
    FREE_AND_COPY_HASH(source->path, "/tests/exit-invalid-coordinate-source");
    object_remove(pl, 0);
    pl->x = 2;
    pl->y = 2;
    pl = object_insert_map(pl, source, NULL, 0);

    mapstruct *destination = get_empty_map(24, 24);
    ck_assert_ptr_nonnull(destination);
    FREE_AND_COPY_HASH(destination->path, "/tests/exit-invalid-coordinate-destination");
    object *exit = exit_test_insert_explicit(source, destination, 4, 4, -1, 6);

    ck_assert(!exit_has_usable_destination(exit));
    ck_assert_int_eq(object_apply(exit, pl, 0), OBJECT_METHOD_OK);
    ck_assert_ptr_eq(pl->map, source);
    ck_assert_int_eq(pl->x, 2);
    ck_assert_int_eq(pl->y, 2);
    ck_assert(!QUERY_FLAG(pl, FLAG_REMOVED));
}
END_TEST

START_TEST(test_exit_cache_tracks_floor_lifecycle_and_accepts_direct_landing) {
    mapstruct *source;
    object *pl;
    check_setup_env_pl(&source, &pl);
    FREE_AND_COPY_HASH(source->path, "/tests/exit-direct-source");
    object_remove(pl, 0);
    pl->x = 2;
    pl->y = 2;
    pl = object_insert_map(pl, source, NULL, 0);

    mapstruct *destination = get_empty_map(24, 24);
    ck_assert_ptr_nonnull(destination);
    FREE_AND_COPY_HASH(destination->path, "/tests/exit-direct-destination");
    object *floor = exit_test_insert_floor(destination, 6, 6);
    object *exit = exit_test_insert_explicit(source, destination, 4, 4, 6, 6);

    ck_assert(exit_has_usable_destination(exit));
    exit_destination_cache_test_reset();
    for (int i = 0; i < 32; i++) {
        ck_assert(exit_has_usable_destination(exit));
    }
    ck_assert_uint_eq(exit_destination_cache_test_recompute_count(), 0);

    CLEAR_FLAG(floor, FLAG_IS_FLOOR);
    object_update(floor, UP_OBJ_FLAGS);
    ck_assert(!exit_has_usable_destination(exit));
    SET_FLAG(floor, FLAG_IS_FLOOR);
    object_update(floor, UP_OBJ_FLAGS);
    ck_assert(exit_has_usable_destination(exit));

    object_remove(floor, 0);
    object_destroy(floor);
    ck_assert(!exit_has_usable_destination(exit));

    floor = exit_test_insert_floor(destination, 6, 6);
    ck_assert(exit_has_usable_destination(exit));

    ck_assert_int_eq(object_apply(exit, pl, 0), OBJECT_METHOD_OK);
    ck_assert_ptr_eq(pl->map, destination);
    ck_assert_int_eq(pl->x, 6);
    ck_assert_int_eq(pl->y, 6);
}
END_TEST

START_TEST(test_exit_rejects_fully_blocked_destination_and_uses_adjacent_fallback) {
    mapstruct *source;
    object *pl;
    check_setup_env_pl(&source, &pl);
    FREE_AND_COPY_HASH(source->path, "/tests/exit-blocked-source");
    object_remove(pl, 0);
    pl->x = 2;
    pl->y = 2;
    pl = object_insert_map(pl, source, NULL, 0);

    mapstruct *blocked_destination = get_empty_map(24, 24);
    ck_assert_ptr_nonnull(blocked_destination);
    FREE_AND_COPY_HASH(blocked_destination->path, "/tests/exit-blocked-destination");
    for (int y = 5; y <= 7; y++) {
        for (int x = 5; x <= 7; x++) {
            exit_test_insert_floor(blocked_destination, x, y);
            exit_test_block(blocked_destination, x, y);
        }
    }
    object *blocked_exit = exit_test_insert_explicit(source, blocked_destination, 4, 4, 6, 6);
    exit_destination_cache_map_changed(blocked_destination);

    ck_assert(!exit_has_usable_destination(blocked_exit));
    ck_assert_int_eq(object_apply(blocked_exit, pl, 0), OBJECT_METHOD_OK);
    ck_assert_ptr_eq(pl->map, source);
    ck_assert_int_eq(pl->x, 2);
    ck_assert_int_eq(pl->y, 2);

    mapstruct *fallback_destination = get_empty_map(24, 24);
    ck_assert_ptr_nonnull(fallback_destination);
    FREE_AND_COPY_HASH(fallback_destination->path, "/tests/exit-fallback-destination");
    exit_test_insert_floor(fallback_destination, 6, 6);
    exit_test_insert_floor(fallback_destination, 7, 6);
    exit_test_block(fallback_destination, 6, 6);
    object *fallback_exit = exit_test_insert_explicit(source, fallback_destination, 8, 4, 6, 6);
    exit_destination_cache_map_changed(fallback_destination);

    ck_assert(exit_has_usable_destination(fallback_exit));
    ck_assert_int_eq(object_apply(fallback_exit, pl, 0), OBJECT_METHOD_OK);
    ck_assert_ptr_eq(pl->map, fallback_destination);
    ck_assert_int_eq(pl->x, 7);
    ck_assert_int_eq(pl->y, 6);
}
END_TEST

START_TEST(test_exit_rejects_roof_only_tiled_destination) {
    mapstruct *source;
    object *pl;
    check_setup_env_pl(&source, &pl);
    FREE_AND_COPY_HASH(source->path, "/tests/exit-roof-source");
    object_remove(pl, 0);
    pl->x = 2;
    pl->y = 2;
    pl = object_insert_map(pl, source, NULL, 0);

    mapstruct *destination = get_empty_map(24, 24);
    ck_assert_ptr_nonnull(destination);
    FREE_AND_COPY_HASH(destination->path, "/tests/exit-roof-destination");
    map_set_tile(source, TILED_EAST, destination->path);
    object *roof = arch_get("roof_thatch");
    ck_assert_ptr_nonnull(roof);
    roof->x = 4;
    roof->y = 4;
    ck_assert_ptr_nonnull(object_insert_map(roof, destination, NULL, INS_NO_MERGE));

    object *exit = arch_get("stairs_down");
    ck_assert_ptr_nonnull(exit);
    exit->x = 4;
    exit->y = 4;
    exit->last_heal = TILED_EAST + 1;
    exit = object_insert_map(exit, source, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(exit);

    ck_assert(!exit_has_usable_destination(exit));
    ck_assert_int_eq(object_apply(exit, pl, 0), OBJECT_METHOD_OK);
    ck_assert_ptr_eq(pl->map, source);
    ck_assert_int_eq(pl->x, 2);
    ck_assert_int_eq(pl->y, 2);
}
END_TEST

START_TEST(test_exit_cache_tracks_tiled_route_changes) {
    mapstruct *source;
    object *pl;
    check_setup_env_pl(&source, &pl);
    FREE_AND_COPY_HASH(source->path, "/tests/exit-tiled-route-source");
    object_remove(pl, 0);
    pl->x = 2;
    pl->y = 2;
    pl = object_insert_map(pl, source, NULL, 0);

    mapstruct *destination = get_empty_map(24, 24);
    ck_assert_ptr_nonnull(destination);
    FREE_AND_COPY_HASH(destination->path, "/tests/exit-tiled-route-destination");
    exit_test_insert_floor(destination, 4, 4);

    object *exit = arch_get("stairs_down");
    ck_assert_ptr_nonnull(exit);
    exit->x = 4;
    exit->y = 4;
    exit->last_heal = TILED_EAST + 1;
    FREE_AND_CLEAR_HASH(EXIT_PATH(exit));
    EXIT_X(exit) = -1;
    EXIT_Y(exit) = -1;
    exit = object_insert_map(exit, source, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(exit);
    ck_assert(!exit_has_usable_destination(exit));

    map_set_tile(source, TILED_EAST, destination->path);
    ck_assert_ptr_eq(EXIT_PATH(exit), source->tile_path[TILED_EAST]);
    ck_assert_int_eq(EXIT_X(exit), exit->x);
    ck_assert_int_eq(EXIT_Y(exit), exit->y);
    ck_assert(exit_has_usable_destination(exit));

    map_set_tile(source, TILED_EAST, "/tests/exit-tiled-route-missing");
    ck_assert(!exit_has_usable_destination(exit));

    map_set_tile(source, TILED_EAST, destination->path);
    ck_assert(exit_has_usable_destination(exit));
}
END_TEST

START_TEST(test_asteria_roof_stair_is_fail_closed) {
    mapstruct *source = ready_map_name("/shattered_islands/world_7_46", NULL, 0);
    ck_assert_ptr_nonnull(source);

    mapstruct *roof = get_map_from_tiled(source, TILED_UP);
    ck_assert_ptr_nonnull(roof);
    ck_assert_str_eq(roof->path, "/shattered_islands/world_7_46_1");

    object *exit = NULL;
    for (object *tmp = GET_MAP_OB(source, 14, 5); tmp != NULL; tmp = tmp->above) {
        if (tmp->type == EXIT && tmp->last_heal == TILED_UP + 1) {
            exit = tmp;
            break;
        }
    }
    ck_assert_ptr_nonnull(exit);
    ck_assert_str_eq(EXIT_PATH(exit), roof->path);
    ck_assert(!exit_has_usable_destination(exit));

    object *pl = player_get_dummy("Asteria exit regression", NULL);
    ck_assert_ptr_nonnull(pl);
    object_remove(pl, 0);
    pl->x = 13;
    pl->y = 5;
    pl = object_insert_map(pl, source, NULL, INS_NO_WALK_ON);
    ck_assert_ptr_nonnull(pl);

    ck_assert_int_eq(object_apply(exit, pl, 0), OBJECT_METHOD_OK);
    ck_assert_ptr_eq(pl->map, source);
    ck_assert_int_eq(pl->x, 13);
    ck_assert_int_eq(pl->y, 5);
    ck_assert(!QUERY_FLAG(pl, FLAG_REMOVED));
}
END_TEST

START_TEST(test_automatic_exit_cache_and_dynamic_exit_activation) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    FREE_AND_COPY_HASH(map->path, "/tests/exit-automatic-source");
    object_remove(pl, 0);
    pl->x = 2;
    pl->y = 2;
    pl = object_insert_map(pl, map, NULL, 0);

    exit_test_insert_floor(map, 8, 8);
    exit_test_insert_floor(map, 9, 8);
    object *peer = arch_get("stairs_down");
    ck_assert_ptr_nonnull(peer);
    peer->sub_type = 73;
    peer->x = 8;
    peer->y = 8;
    peer = object_insert_map(peer, map, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(peer);

    object *automatic = arch_get("stairs_down");
    ck_assert_ptr_nonnull(automatic);
    automatic->sub_type = 73;
    automatic->x = 4;
    automatic->y = 4;
    automatic = object_insert_map(automatic, map, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(automatic);

    ck_assert(exit_has_usable_destination(automatic));
    exit_destination_cache_test_reset();
    ck_assert_int_eq(object_apply(automatic, pl, 0), OBJECT_METHOD_OK);
    ck_assert_uint_eq(exit_destination_cache_test_recompute_count(), 0);
    ck_assert_ptr_eq(pl->map, map);
    ck_assert_int_eq(pl->x, 9);
    ck_assert_int_eq(pl->y, 8);

    mapstruct *dynamic_destination = get_empty_map(24, 24);
    ck_assert_ptr_nonnull(dynamic_destination);
    FREE_AND_COPY_HASH(dynamic_destination->path, "/tests/exit-dynamic-destination");
    exit_test_insert_floor(dynamic_destination, 6, 6);
    object *dynamic = exit_test_insert_explicit(map, dynamic_destination, 12, 4, 6, 6);
    dynamic->event_flags = EVENT_FLAG(EVENT_TRIGGER);
    exit_destination_cache_refresh(dynamic);

    ck_assert(!exit_has_usable_destination(dynamic));
    ck_assert_int_eq(object_apply(dynamic, pl, 0), OBJECT_METHOD_OK);
    ck_assert_ptr_eq(pl->map, dynamic_destination);
    ck_assert_int_eq(pl->x, 6);
    ck_assert_int_eq(pl->y, 6);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("exit");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_exit_rejects_missing_floor_without_removing_applier);
    tcase_add_test(tc_core, test_exit_rejects_invalid_destination_coordinates);
    tcase_add_test(tc_core, test_exit_cache_tracks_floor_lifecycle_and_accepts_direct_landing);
    tcase_add_test(tc_core, test_exit_rejects_fully_blocked_destination_and_uses_adjacent_fallback);
    tcase_add_test(tc_core, test_exit_rejects_roof_only_tiled_destination);
    tcase_add_test(tc_core, test_exit_cache_tracks_tiled_route_changes);
    tcase_add_test(tc_core, test_asteria_roof_stair_is_fail_closed);
    tcase_add_test(tc_core, test_automatic_exit_cache_and_dynamic_exit_activation);

    return s;
}

void check_types_exit(void) {
    check_run_suite(suite(), __FILE__);
}
