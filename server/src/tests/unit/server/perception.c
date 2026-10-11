/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <server_main.h>
#include <server.h>
#include <attack.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <monster.h>
#include <monster_data.h>
#include <object.h>
#include <object_methods.h>
#include <perception.h>
#include <player.h>

static object *perception_monster(mapstruct *map, int x, int y) {
    object *npc = arch_get("kobold");
    npc->x = x;
    npc->y = y;
    npc->item_power = 6;
    CLEAR_FLAG(npc, FLAG_UNAGGRESSIVE);
    CLEAR_FLAG(npc, FLAG_SLEEP);
    npc = object_insert_map(npc, map, NULL, INS_NO_MERGE);
    monster_data_init(npc);
    return npc;
}

static void perception_fixture(mapstruct **map, object **pl) {
    check_setup_env_pl(map, pl);
    FREE_AND_COPY_HASH((*map)->path, "/tests/perception/base");
    object_remove(*pl, 0);
    (*pl)->x = 8;
    (*pl)->y = 8;
    *pl = object_insert_map(*pl, *map, NULL, INS_NO_MERGE);
    CONTR(*pl)->cs->state = ST_PLAYING;
    CONTR(*pl)->tls = 0;
    CONTR(*pl)->tli = 0;
    CONTR(*pl)->tsi = 0;
    for (int x = 0; x < MAP_WIDTH(*map); x++) {
        for (int y = 0; y < MAP_HEIGHT(*map); y++) {
            GET_MAP_SPACE_PTR(*map, x, y)->light_value = 1280;
        }
    }
}

static mapstruct *perception_upper(mapstruct *lower) {
    mapstruct *upper = get_empty_map(MAP_WIDTH(lower), MAP_HEIGHT(lower));
    FREE_AND_COPY_HASH(upper->path, "/tests/perception/upper");
    lower->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = lower;
    for (int x = 0; x < MAP_WIDTH(upper); x++) {
        for (int y = 0; y < MAP_HEIGHT(upper); y++) {
            GET_MAP_SPACE_PTR(upper, x, y)->light_value = 1280;
        }
    }
    return upper;
}

static object *perception_slab(mapstruct *map, int x, int y, bool roof) {
    object *slab = arch_get(roof ? "roof_thatch" : "floor_stone1a");
    ck_assert_ptr_nonnull(slab);
    slab->x = x;
    slab->y = y;
    return object_insert_map(slab, map, NULL, INS_NO_MERGE);
}

START_TEST(test_monster_acquires_above_and_below_only_through_holes) {
    mapstruct *lower;
    object *pl;
    perception_fixture(&lower, &pl);
    mapstruct *upper = perception_upper(lower);
    object *npc = perception_monster(upper, pl->x, pl->y);
    rv_vector rv;
    ck_assert(perception_object_visible(npc, pl));
    ck_assert_ptr_eq(find_enemy(npc, &rv), pl);
    ck_assert_int_eq(rv.distance_z, -1);
    ck_assert_uint_eq(rv.distance, 1);
    ck_assert(!attack_is_melee_range(npc, pl));

    set_npc_enemy(npc, NULL, NULL);
    object *floor = perception_slab(upper, pl->x, pl->y, false);
    ck_assert(!perception_object_visible(npc, pl));
    ck_assert_ptr_null(find_enemy(npc, &rv));
    object_remove(floor, 0);
    object_destroy(floor);
    object *roof = perception_slab(upper, pl->x, pl->y, true);
    ck_assert(!perception_object_visible(npc, pl));
    ck_assert_ptr_null(find_enemy(npc, &rv));
    object_remove(roof, 0);
    object_destroy(roof);

    object_remove(npc, 0);
    npc = object_insert_map(npc, lower, NULL, INS_NO_MERGE);
    object_remove(pl, 0);
    pl = object_insert_map(pl, upper, NULL, INS_NO_MERGE);
    ck_assert_ptr_eq(find_enemy(npc, &rv), pl);
    ck_assert_int_eq(rv.distance_z, 1);
    ck_assert(!attack_is_melee_range(npc, pl));
}
END_TEST

START_TEST(test_retained_enemy_loses_sight_without_losing_pursuit_or_casting) {
    mapstruct *map;
    object *pl;
    perception_fixture(&map, &pl);
    object *npc = perception_monster(map, pl->x - 3, pl->y);
    npc->stats.Pow = npc->stats.Con = 0;
    npc->stats.Dex = 1;
    npc->stats.sp = npc->stats.maxsp = 100;
    SET_FLAG(npc, FLAG_STAND_STILL);
    SET_FLAG(npc, FLAG_CAST_SPELL);
    object *ability = object_get();
    ability->type = ABILITY;
    ability->stats.sp = SP_BULLET;
    ability->stats.hp = -1;
    object_insert_into(ability, npc, 0);
    set_npc_enemy(npc, pl, NULL);
    rv_vector rv;
    ck_assert_ptr_eq(check_enemy(npc, &rv), pl);
    GET_MAP_SPACE_PTR(map, pl->x - 1, pl->y)->flags |= P_BLOCKSVIEW;
    ck_assert(!perception_object_visible(npc, pl));
    ck_assert_ptr_eq(check_enemy(npc, &rv), pl);
    int sp = npc->stats.sp;
    object_process(npc);
    ck_assert_ptr_eq(npc->enemy, pl);
    ck_assert_int_eq(npc->stats.sp, sp);
    ck_assert_ptr_ne(pl->attacked_by, npc);
}
END_TEST

START_TEST(test_player_retained_target_revalidates_geometry_and_light) {
    mapstruct *map;
    object *pl;
    perception_fixture(&map, &pl);
    object *npc = perception_monster(map, pl->x + 2, pl->y);
    ck_assert(perception_player_authorized(CONTR(pl), npc));
    CONTR(pl)->target_object = npc;
    CONTR(pl)->target_object_count = npc->count;
    CONTR(pl)->combat = 1;
    ck_assert(!target_player_authorized(CONTR(pl), npc, NULL));
    OBJECT_METHODS(pl->type)->process_func(pl);
    ck_assert_ptr_eq(CONTR(pl)->target_object, pl);
    ck_assert_ptr_null(pl->enemy);
    draw_client_map(pl);
    ck_assert(target_player_authorized(CONTR(pl), npc, NULL));
    CONTR(pl)->target_object = npc;
    CONTR(pl)->target_object_count = npc->count;
    OBJECT_METHODS(pl->type)->process_func(pl);
    ck_assert_ptr_eq(CONTR(pl)->target_object, npc);
    /* Cached 2D LOS is deliberately permissive: it cannot authorize a target. */
    memset(CONTR(pl)->blocked_los, 0, sizeof(CONTR(pl)->blocked_los));
    GET_MAP_SPACE_PTR(map, pl->x + 1, pl->y)->flags |= P_BLOCKSVIEW;
    ck_assert(!perception_player_authorized(CONTR(pl), npc));
    object_process(pl);
    ck_assert_ptr_eq(CONTR(pl)->target_object, pl);
    ck_assert_ptr_null(pl->enemy);

    GET_MAP_SPACE_PTR(map, pl->x + 1, pl->y)->flags &= ~P_BLOCKSVIEW;
    /* Use a sufficiently negative aggregate to overcome the legacy ambient. */
    GET_MAP_SPACE_PTR(map, npc->x, npc->y)->light_value = -100000;
    ck_assert(!perception_player_authorized(CONTR(pl), npc));
    SET_FLAG(pl, FLAG_XRAYS);
    ck_assert(perception_player_authorized(CONTR(pl), npc));
    SET_FLAG(npc, FLAG_IS_INVISIBLE);
    ck_assert(!perception_player_authorized(CONTR(pl), npc));
    SET_FLAG(pl, FLAG_SEE_INVISIBLE);
    ck_assert(perception_player_authorized(CONTR(pl), npc));
}
END_TEST

START_TEST(test_camera_cover_does_not_hide_player_from_monster) {
    mapstruct *map;
    object *pl;
    perception_fixture(&map, &pl);
    mapstruct *upper = perception_upper(map);
    perception_slab(upper, pl->x + 1, pl->y, true);
    object *npc = perception_monster(map, pl->x + 1, pl->y);
    /* Camera cutaway grants no power to alter another actor's physical sight. */
    ck_assert(perception_object_visible(npc, pl));
    ck_assert(perception_object_visible(pl, npc));
    ck_assert(!perception_player_authorized(CONTR(pl), npc));
    rv_vector rv;
    ck_assert_ptr_eq(find_enemy(npc, &rv), pl);
}
END_TEST

START_TEST(test_acquisition_large_authored_radius_remains_bounded) {
    mapstruct *map;
    object *pl;
    perception_fixture(&map, &pl);
    object *npc = perception_monster(map, pl->x, pl->y + 1);
    npc->item_power = INT8_MAX;
    SET_FLAG(pl, FLAG_IS_INVISIBLE);
    rv_vector rv;
    ck_assert_ptr_null(find_enemy(npc, &rv));
}
END_TEST

void check_server_perception(void) {
    Suite *s = suite_create("server.perception");
    TCase *tc = tcase_create("physical-perception");
    tcase_add_unchecked_fixture(tc, check_setup, check_teardown);
    tcase_add_checked_fixture(tc, check_test_setup, check_test_teardown);
    tcase_add_test(tc, test_monster_acquires_above_and_below_only_through_holes);
    tcase_add_test(tc, test_retained_enemy_loses_sight_without_losing_pursuit_or_casting);
    tcase_add_test(tc, test_player_retained_target_revalidates_geometry_and_light);
    tcase_add_test(tc, test_camera_cover_does_not_hide_player_from_monster);
    tcase_add_test(tc, test_acquisition_large_authored_radius_remains_bounded);
    suite_add_tcase(s, tc);
    check_run_suite(s, __FILE__);
}
