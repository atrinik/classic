/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <initialization.h>
#include <server_main.h>
#include <player.h>
#include <object.h>
#include <los.h>
#include <arch.h>

static void fixture(mapstruct **map, object **pl) {
    check_setup_env_pl(map, pl);
    object_remove(*pl, 0);
    (*pl)->x = 12;
    (*pl)->y = 12;
    *pl = object_insert_map(*pl, *map, NULL, 0);
    ck_assert_ptr_nonnull(*pl);
    CONTR(*pl)->tls = 0;
    CLEAR_FLAG(*pl, FLAG_BLIND);
    CLEAR_FLAG(*pl, FLAG_XRAYS);
}

static object *place(mapstruct *map, const char *name, int x, int y) {
    object *op = arch_get(name);
    ck_assert_ptr_nonnull(op);
    op->x = x;
    op->y = y;
    op = object_insert_map(op, map, NULL, 0);
    ck_assert_ptr_nonnull(op);
    return op;
}

static mapstruct *add_level(mapstruct *base, int depth, int top) {
    mapstruct *upper = get_empty_map(24, 24);
    base->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = base;
    upper->coords[2] = depth;
    upper->level_min = 0;
    upper->level_max = top;
    return upper;
}

START_TEST(test_roof_only_cover_prevents_fresh_gameplay_visibility) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->level_min = 0;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    ck_assert(los_player_can_see_cell(pl, map, 13, 12));
    object *roof = place(upper, "roof_thatch", 13, 12);
    ck_assert(object_is_roof_surface(roof));
    ck_assert(los_player_has_geometric_sight(pl, map, 13, 12));
    ck_assert(!los_player_can_see_cell(pl, map, 13, 12));
    ck_assert_int_eq(los_player_cell_camera(pl, map, 13, 12), LOS_CAMERA_WALL_BOUNDARY);
    /* Stale player masks cannot restore action authority. */
    memset(CONTR(pl)->blocked_los_depth, 0, sizeof(CONTR(pl)->blocked_los_depth));
    ck_assert(!los_player_can_see_cell(pl, map, 13, 12));
    object_remove(roof, 0);
    ck_assert(los_player_can_see_cell(pl, map, 13, 12));
}
END_TEST

START_TEST(test_full_stack_cover_is_not_clipped_to_displayed_depths) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->level_min = 0;
    map->level_max = 12;
    mapstruct *top = map;
    for (int z = 1; z <= 12; z++) {
        top = add_level(top, z, 12);
    }
    place(top, "roof_thatch", 13, 12);
    ck_assert(!los_player_can_see_cell(pl, map, 13, 12));
    update_los(pl);
    ck_assert_int_eq(CONTR(pl)->camera_visibility[MAP2_DEPTH_INDEX(0)]
                                               [CONTR(pl)->cs->mapx_2 + 1]
                                               [CONTR(pl)->cs->mapy_2],
                     LOS_CAMERA_WALL_BOUNDARY);
}
END_TEST

START_TEST(test_depth_masks_use_each_ray_instead_of_base_silhouette) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->level_min = 0;
    map->level_max = 3;
    mapstruct *middle = add_level(map, 1, 3);
    mapstruct *second = add_level(middle, 2, 3);
    mapstruct *upper = add_level(second, 3, 3);
    place(map, "wall_wood_1", 13, 12);
    ck_assert(!los_player_has_geometric_sight(pl, map, 14, 12));
    /* dz=3 crosses UP before EAST; dz=2 would graze the base wall
     * at an X/Z tie and must remain blocked by conservative supercover. */
    ck_assert(!los_player_has_geometric_sight(pl, second, 14, 12));
    ck_assert(los_player_has_geometric_sight(pl, upper, 14, 12));
    update_los(pl);
    int ax = CONTR(pl)->cs->mapx_2 + 2, ay = CONTR(pl)->cs->mapy_2;
    ck_assert(CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(0)][ax][ay] & BLOCKED_LOS_BLOCKED);
    ck_assert(!(CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(3)][ax][ay] & BLOCKED_LOS_BLOCKED));
    ck_assert_int_eq(CONTR(pl)->blocked_los[ax][ay],
                     CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(0)][ax][ay]);
}
END_TEST

START_TEST(test_cutaway_does_not_open_physical_slabs) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->level_min = 0;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    place(upper, "water_still", 12, 12);
    ck_assert_int_eq(los_player_cell_camera(pl, map, 12, 12), LOS_CAMERA_VISIBLE);
    ck_assert_int_eq(los_player_cell_camera(pl, upper, 12, 12), LOS_CAMERA_HIDDEN);
    ck_assert(!los_player_has_geometric_sight(pl, upper, 12, 12));
    SET_FLAG(pl, FLAG_XRAYS);
    ck_assert(!los_player_has_geometric_sight(pl, upper, 12, 12));
    ck_assert(!los_player_can_see_cell(pl, upper, 12, 12));
}
END_TEST

START_TEST(test_blind_and_xray_exceptions_are_bounded_and_same_story) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    place(map, "wall_wood_1", 13, 12);
    SET_FLAG(pl, FLAG_XRAYS);
    ck_assert(los_player_can_see_cell(pl, map, 15, 12));
    ck_assert(!los_player_can_see_cell(pl, map, 16, 12));
    SET_FLAG(pl, FLAG_BLIND);
    ck_assert(los_player_can_see_cell(pl, map, 12, 12));
    ck_assert(!los_player_can_see_cell(pl, map, 13, 12));
    update_los(pl);
    ck_assert(CONTR(pl)->blocked_los[CONTR(pl)->cs->mapx_2 + 1][CONTR(pl)->cs->mapy_2] &
              BLOCKED_LOS_BLOCKED);
}
END_TEST

START_TEST(test_supercover_corners_do_not_expand_gameplay_sight) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    place(map, "wall_wood_1", 13, 12);
    place(map, "wall_wood_1", 12, 13);
    ck_assert(!los_player_can_see_cell(pl, map, 13, 13));
    ck_assert(!los_player_can_see_cell(pl, map, 14, 14));
}
END_TEST

START_TEST(test_floor_top_does_not_hide_occupants_from_camera) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->level_min = 0;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    place(upper, "water_still", 14, 12);
    ck_assert_int_eq(los_player_cell_camera(pl, upper, 14, 12), LOS_CAMERA_VISIBLE);
    ck_assert_int_eq(los_player_cell_camera(pl, map, 14, 12), LOS_CAMERA_WALL_BOUNDARY);
}
END_TEST

START_TEST(test_unavailable_link_and_tls_exception_fail_deliberately) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    FREE_AND_COPY_HASH(map->tile_path[TILED_UP], "/unavailable-upper");
    ck_assert(!los_player_can_see_cell(pl, map, 13, 12));
    CONTR(pl)->tls = 1;
    SET_FLAG(pl, FLAG_BLIND);
    ck_assert(los_player_can_see_cell(pl, map, 13, 12));
}
END_TEST

START_TEST(test_camera_hidden_depths_are_pruned_before_sight_checks) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    cs->mapx = cs->mapy = 5;
    cs->mapx_2 = cs->mapy_2 = 2;
    map->level_min = -1;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    upper->level_min = -1;
    mapstruct *lower = get_empty_map(24, 24);
    map->tile_map[TILED_DOWN] = lower;
    lower->tile_map[TILED_UP] = map;
    lower->coords[2] = -1;
    lower->level_min = -1;
    lower->level_max = 1;
    object *center_roof = NULL, *center_floor = NULL;
    for (int x = 10; x <= 14; x++) {
        for (int y = 10; y <= 14; y++) {
            object *roof = place(upper, "roof_thatch", x, y);
            object *floor = place(map, "water_still", x, y);
            if (x == 12 && y == 12) {
                center_roof = roof;
                center_floor = floor;
            }
        }
    }
    update_los(pl);
    size_t cells = (size_t)cs->mapx * cs->mapy;
    /* The connected overhead cutaway and current floor completely hide
     * their upper/lower depths. Each resolved cell is classified once. */
    ck_assert_uint_eq(los_view_camera_pruned_for_test(1), cells);
    ck_assert_uint_eq(los_view_camera_pruned_for_test(-1), cells);
    ck_assert_uint_eq(los_view_sight_checks_for_test(1), 0);
    ck_assert_uint_eq(los_view_sight_checks_for_test(-1), 0);
    ck_assert_uint_eq(los_view_sight_checks_for_test(0), cells);
    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        ck_assert_uint_le(los_view_sight_checks_for_test(depth) +
                          los_view_camera_pruned_for_test(depth), cells);
    }
    ck_assert(CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(1)][2][2] & BLOCKED_LOS_BLOCKED);
    ck_assert(CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(-1)][2][2] & BLOCKED_LOS_BLOCKED);
    /* A cutaway affects camera eligibility alone. The fresh physical query
     * remains authoritative and is separately blocked by the current slab. */
    ck_assert(!los_player_has_geometric_sight(pl, upper, 12, 12));
    ck_assert(!los_player_can_see_cell(pl, upper, 12, 12));
    object_remove(center_roof, 0);
    object_remove(center_floor, 0);
    /* Geometry changed after the cached prepass. Physical queries and fresh
     * player actions see the new opening without consulting its stale mask. */
    ck_assert(los_player_has_geometric_sight(pl, upper, 12, 12));
    ck_assert(los_player_can_see_cell(pl, upper, 12, 12));
    ck_assert_uint_eq(CONTR(pl)->camera_visibility[MAP2_DEPTH_INDEX(1)][2][2], LOS_CAMERA_HIDDEN);
    ck_assert_uint_eq(los_view_sight_checks_for_test(1), 0);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("los");
    TCase *tc = tcase_create("Core");
    tcase_add_unchecked_fixture(tc, check_setup, check_teardown);
    tcase_add_checked_fixture(tc, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc);
    tcase_add_test(tc, test_roof_only_cover_prevents_fresh_gameplay_visibility);
    tcase_add_test(tc, test_full_stack_cover_is_not_clipped_to_displayed_depths);
    tcase_add_test(tc, test_depth_masks_use_each_ray_instead_of_base_silhouette);
    tcase_add_test(tc, test_cutaway_does_not_open_physical_slabs);
    tcase_add_test(tc, test_blind_and_xray_exceptions_are_bounded_and_same_story);
    tcase_add_test(tc, test_supercover_corners_do_not_expand_gameplay_sight);
    tcase_add_test(tc, test_floor_top_does_not_hide_occupants_from_camera);
    tcase_add_test(tc, test_unavailable_link_and_tls_exception_fail_deliberately);
    tcase_add_test(tc, test_camera_hidden_depths_are_pruned_before_sight_checks);
    return s;
}

void check_server_los(void) {
    check_run_suite(suite(), __FILE__);
}
