/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
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
#include <object_methods.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <stdarg.h>
#include <arch.h>
#include <player.h>
#include <object.h>
#include <light.h>
#include <object_methods.h>
#include <toolkit/string.h>

static object *insert_applied_light(object *pl, int radius, uint32_t color) {
    object *light = arch_get("torch");
    light->glow_radius = radius;
    light->last_sp = radius;
    light->light_color = color;
    light = object_insert_into(light, pl, INS_NO_MERGE);
    SET_FLAG(light, FLAG_APPLIED);
    return light;
}

static void assert_player_light(const object *pl, int radius, uint32_t color) {
    ck_assert_int_eq(pl->glow_radius, radius);
    ck_assert_uint_eq(pl->light_color, color);
}

/*
 * Player applies a torch on the ground. Ensure the torch is lit and not
 * applied.
 */
START_TEST(test_light_apply_apply_1) {
    mapstruct *map;
    object *pl, *torch;

    check_setup_env_pl(&map, &pl);

    torch = object_insert_map(arch_get("torch"), map, NULL, 0);
    player_apply(pl, torch, 0, 0);
    ck_assert(!QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_int_ne(torch->glow_radius, 0);
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], NULL);
}
END_TEST

/*
 * Player applies a lit torch on the ground Ensure the torch is extinguished and
 * not applied.
 */
START_TEST(test_light_apply_apply_2) {
    mapstruct *map;
    object *pl, *torch;

    check_setup_env_pl(&map, &pl);

    torch = object_insert_map(arch_get("torch"), map, NULL, 0);
    manual_apply(torch, torch, 0);
    player_apply(pl, torch, 0, 0);
    ck_assert(!QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_int_eq(torch->glow_radius, 0);
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], NULL);
}
END_TEST

/*
 * Player applies a torch in his inventory. Ensure it's lit and applied.
 */
START_TEST(test_light_apply_apply_3) {
    mapstruct *map;
    object *pl, *torch;

    check_setup_env_pl(&map, &pl);

    torch = object_insert_into(arch_get("torch"), pl, 0);
    player_apply(pl, torch, 0, 0);
    ck_assert(QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_int_ne(torch->glow_radius, 0);
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], torch);
}
END_TEST

/*
 * Player applies a lit torch in his inventory. Ensure it was extinguished and
 * not applied.
 */
START_TEST(test_light_apply_apply_4) {
    mapstruct *map;
    object *pl, *torch;

    check_setup_env_pl(&map, &pl);

    torch = object_insert_map(arch_get("torch"), map, NULL, 0);
    manual_apply(torch, torch, 0);
    ck_assert(!QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_int_ne(torch->glow_radius, 0);
    object_remove(torch, 0);
    torch = object_insert_into(torch, pl, 0);
    ck_assert(!QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_int_ne(torch->glow_radius, 0);
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], NULL);

    player_apply(pl, torch, 0, 0);

    ck_assert(QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_int_ne(torch->glow_radius, 0);
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], torch);
}
END_TEST

/*
 * Player applies a torch in his inventory, then a torch on the ground. Ensure
 * both torches are lit, but only the one in inventory is applied.
 */
START_TEST(test_light_apply_apply_5) {
    mapstruct *map;
    object *pl, *torch, *torch2;

    check_setup_env_pl(&map, &pl);

    torch = object_insert_into(arch_get("torch"), pl, 0);
    ck_assert_ptr_ne(torch, NULL);
    torch2 = object_insert_map(arch_get("torch"), map, NULL, 0);
    ck_assert_ptr_ne(torch2, NULL);

    player_apply(pl, torch, 0, 0);
    ck_assert(QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], torch);
    ck_assert_int_ne(torch->glow_radius, 0);
    ck_assert(!QUERY_FLAG(torch2, FLAG_APPLIED));
    ck_assert_int_eq(torch2->glow_radius, 0);

    player_apply(pl, torch2, 0, 0);
    ck_assert(QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], torch);
    ck_assert_int_ne(torch->glow_radius, 0);
    ck_assert(!QUERY_FLAG(torch2, FLAG_APPLIED));
    ck_assert_int_ne(torch2->glow_radius, 0);
}
END_TEST

/*
 * Player applies stacked torches, ensure a new torch is created and lit and
 * the original is not lit. Then apply the original and ensure it's lit and
 * applied, and that the old one gets extinguished and not applied.
 */
START_TEST(test_light_apply_apply_6) {
    mapstruct *map;
    object *pl, *torch, *torch2;

    check_setup_env_pl(&map, &pl);

    torch = arch_get("torch");
    torch->nrof = 2;
    torch = object_insert_into(torch, pl, 0);
    ck_assert_ptr_ne(torch, NULL);

    player_apply(pl, torch, 0, 0);
    torch2 = torch;
    torch = CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT];

    ck_assert_ptr_ne(torch, torch2);
    ck_assert_ptr_ne(torch, NULL);
    ck_assert(QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_int_ne(torch->glow_radius, 0);
    ck_assert_uint_eq(torch->nrof, 1);
    ck_assert(!QUERY_FLAG(torch2, FLAG_APPLIED));
    ck_assert_int_eq(torch2->glow_radius, 0);

    player_apply(pl, torch2, 0, 0);
    ck_assert(!QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_int_eq(torch->glow_radius, 0);
    ck_assert(QUERY_FLAG(torch2, FLAG_APPLIED));
    ck_assert_int_ne(torch2->glow_radius, 0);
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], torch2);
}
END_TEST

START_TEST(test_applied_light_propagates_color_and_restores_field) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    MapSpace *space = GET_MAP_SPACE_PTR(map, pl->x, pl->y);
    int32_t initial_scalar = space->light_source_value;
    int64_t initial_color[3];
    memcpy(initial_color, space->light_source_color, sizeof(initial_color));

    object *torch = arch_get("torch");
    torch->light_color = UINT32_C(0xff0000);
    torch = object_insert_into(torch, pl, 0);
    player_apply(pl, torch, 0, 0);
    ck_assert(QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_uint_eq(pl->light_color, UINT32_C(0xff0000));
    ck_assert_int_gt(space->light_source_color[0], space->light_source_color[1]);

    player_apply(pl, torch, 0, 0);
    ck_assert(!QUERY_FLAG(torch, FLAG_APPLIED));
    ck_assert_int_eq(space->light_source_value, initial_scalar);
    ck_assert_mem_eq(space->light_source_color, initial_color, sizeof(initial_color));
}
END_TEST

START_TEST(test_equal_radius_lights_use_stable_inventory_priority) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    object *red = insert_applied_light(pl, 4, UINT32_C(0xff0000));
    object *blue = insert_applied_light(pl, 4, UINT32_C(0x0000ff));
    living_update(pl);

    assert_player_light(pl, 4, UINT32_C(0x0000ff));
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], blue);

    CLEAR_FLAG(blue, FLAG_APPLIED);
    living_update(pl);
    assert_player_light(pl, 4, UINT32_C(0xff0000));
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], red);

    SET_FLAG(blue, FLAG_APPLIED);
    living_update(pl);
    assert_player_light(pl, 4, UINT32_C(0x0000ff));

    object_remove(blue, 0);
    object_destroy(blue);
    assert_player_light(pl, 4, UINT32_C(0xff0000));
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], red);
}
END_TEST

START_TEST(test_same_radius_hue_change_is_idempotent) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    object *torch = insert_applied_light(pl, 4, UINT32_C(0xff0000));
    living_update(pl);
    MapSpace *space = GET_MAP_SPACE_PTR(map, pl->x, pl->y);
    int32_t red_scalar = space->light_source_value;
    int64_t red_field[3];
    memcpy(red_field, space->light_source_color, sizeof(red_field));

    torch->light_color = UINT32_C(0x0000ff);
    living_update(pl);
    assert_player_light(pl, 4, UINT32_C(0x0000ff));
    ck_assert_int_eq(space->light_source_value, red_scalar);
    ck_assert(memcmp(space->light_source_color, red_field, sizeof(red_field)) != 0);

    int32_t blue_scalar = space->light_source_value;
    int64_t blue_field[3];
    memcpy(blue_field, space->light_source_color, sizeof(blue_field));
    living_update(pl);
    ck_assert_int_eq(space->light_source_value, blue_scalar);
    ck_assert_mem_eq(space->light_source_color, blue_field, sizeof(blue_field));
}
END_TEST

START_TEST(test_extinguish_relight_and_burnout_select_fallback) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    object *red = insert_applied_light(pl, 3, UINT32_C(0xff0000));
    object *blue = insert_applied_light(pl, 5, UINT32_C(0x0000ff));
    living_update(pl);
    assert_player_light(pl, 5, UINT32_C(0x0000ff));

    blue->glow_radius = 0;
    living_update(pl);
    assert_player_light(pl, 3, UINT32_C(0xff0000));

    blue->glow_radius = 5;
    living_update(pl);
    assert_player_light(pl, 5, UINT32_C(0x0000ff));

    tag_t blue_tag = blue->count;
    blue->stats.food = 0;
    blue->state = 0;
    SET_FLAG(blue, FLAG_CHANGING);
    ck_assert_int_eq(common_object_process_pre(blue), 1);
    ck_assert(OBJECT_DESTROYED(blue, blue_tag));
    assert_player_light(pl, 3, UINT32_C(0xff0000));
    ck_assert_ptr_eq(CONTR(pl)->equipment[PLAYER_EQUIP_LIGHT], red);
}
END_TEST

START_TEST(test_removed_player_recomputes_before_reinsertion) {
    mapstruct *old_map;
    object *pl;
    check_setup_env_pl(&old_map, &pl);
    MapSpace *old_space = GET_MAP_SPACE_PTR(old_map, pl->x, pl->y);
    int32_t initial_scalar = old_space->light_source_value;
    int64_t initial_color[3];
    memcpy(initial_color, old_space->light_source_color, sizeof(initial_color));

    object *torch = insert_applied_light(pl, 4, UINT32_C(0x00ff00));
    living_update(pl);
    object_remove(pl, 0);
    ck_assert_int_eq(old_space->light_source_value, initial_scalar);
    ck_assert_mem_eq(old_space->light_source_color, initial_color, sizeof(initial_color));

    torch->light_color = UINT32_C(0x0000ff);
    living_update(pl);
    assert_player_light(pl, 4, UINT32_C(0x0000ff));
    ck_assert_int_eq(old_space->light_source_value, initial_scalar);
    ck_assert_mem_eq(old_space->light_source_color, initial_color, sizeof(initial_color));

    mapstruct *new_map = get_empty_map(24, 24);
    pl = object_insert_map(pl, new_map, NULL, 0);
    MapSpace *new_space = GET_MAP_SPACE_PTR(new_map, pl->x, pl->y);
    ck_assert_int_gt(new_space->light_source_color[2], new_space->light_source_color[0]);
    assert_player_light(pl, 4, UINT32_C(0x0000ff));
}
END_TEST

START_TEST(test_serialized_inventory_reconstructs_derived_light) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    insert_applied_light(pl, 4, UINT32_C(0xff0000));
    insert_applied_light(pl, 4, UINT32_C(0x0000ff));
    living_update(pl);
    assert_player_light(pl, 4, UINT32_C(0x0000ff));

    StringBuffer *sb = stringbuffer_new();
    object_dump_rec(pl, sb);
    char *dump = stringbuffer_finish(sb);
    char *nested = strstr(dump + 1, "\narch ");
    ck_assert_ptr_ne(nested, NULL);
    char saved = *nested;
    *nested = '\0';
    ck_assert_ptr_eq(strstr(dump, "glow_radius "), NULL);
    ck_assert_ptr_eq(strstr(dump, "light_color "), NULL);
    *nested = saved;

    object *loaded = object_load_str(dump);
    free(dump);
    ck_assert_ptr_ne(loaded, NULL);
    /* player_load() restores the saved canonical inventory order after the
     * generic nested-object loader prepends each object. */
    object_reverse_inventory(loaded);
    ck_assert_int_eq(loaded->glow_radius, loaded->arch->clone.glow_radius);
    ck_assert_uint_eq(loaded->light_color, loaded->arch->clone.light_color);

    object *lights[2] = {NULL, NULL};
    size_t num_lights = 0;
    for (object *tmp = loaded->inv; tmp != NULL; tmp = tmp->below) {
        if (tmp->type == LIGHT_APPLY && QUERY_FLAG(tmp, FLAG_APPLIED) && num_lights < 2) {
            lights[num_lights++] = tmp;
        }
    }
    ck_assert_uint_eq(num_lights, 2);
    ck_assert_uint_eq(lights[0]->light_color, UINT32_C(0x0000ff));
    ck_assert_uint_eq(lights[1]->light_color, UINT32_C(0xff0000));

    mapstruct *restored_map;
    object *restored;
    check_setup_env_pl(&restored_map, &restored);
    object_remove(restored, 0);
    SET_FLAG(restored, FLAG_NO_FIX_PLAYER);
    for (size_t i = num_lights; i > 0; i--) {
        object_remove(lights[i - 1], 0);
        object_insert_into(lights[i - 1], restored, INS_NO_MERGE);
    }
    CLEAR_FLAG(restored, FLAG_NO_FIX_PLAYER);

    living_update(restored);
    assert_player_light(restored, 4, UINT32_C(0x0000ff));
    restored = object_insert_map(restored, restored_map, NULL, 0);
    assert_player_light(restored, 4, UINT32_C(0x0000ff));
    MapSpace *space = GET_MAP_SPACE_PTR(restored_map, restored->x, restored->y);
    ck_assert_int_gt(space->light_source_color[2], space->light_source_color[0]);

    object_destroy(loaded);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("light_apply");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);

    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_light_apply_apply_1);
    tcase_add_test(tc_core, test_light_apply_apply_2);
    tcase_add_test(tc_core, test_light_apply_apply_3);
    tcase_add_test(tc_core, test_light_apply_apply_4);
    tcase_add_test(tc_core, test_light_apply_apply_5);
    tcase_add_test(tc_core, test_light_apply_apply_6);
    tcase_add_test(tc_core, test_applied_light_propagates_color_and_restores_field);
    tcase_add_test(tc_core, test_equal_radius_lights_use_stable_inventory_priority);
    tcase_add_test(tc_core, test_same_radius_hue_change_is_idempotent);
    tcase_add_test(tc_core, test_extinguish_relight_and_burnout_select_fallback);
    tcase_add_test(tc_core, test_removed_player_recomputes_before_reinsertion);
    tcase_add_test(tc_core, test_serialized_inventory_reconstructs_derived_light);

    return s;
}

void check_types_light_apply(void) {
    check_run_suite(suite(), __FILE__);
}
