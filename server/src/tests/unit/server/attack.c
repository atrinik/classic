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
#include <attack.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <monster_data.h>
#include <object_methods.h>
#include <player.h>
#include <skills.h>
#include <toolkit/packet.h>

static object *attack_test_target(mapstruct *map, object *pl) {
    if (map->path == NULL) {
        FREE_AND_COPY_HASH(map->path, "/tests/attack");
    }

    object *target = arch_get("kobold");
    target->x = pl->x + 1;
    target->y = pl->y;
    target = object_insert_map(target, map, NULL, INS_NO_MERGE);
    monster_data_init(target);
    target = HEAD(target);
    set_mobile_speed(target, 0);
    target->stats.hp = 1000;
    target->stats.maxhp = 1000;
    target->block = 0;
    target->absorb = 0;
    memset(target->protection, 0, sizeof(target->protection));
    return target;
}

static size_t attack_test_messages(object *pl, const char *expected) {
    size_t count = 0;

    for (packet_struct *packet = CONTR(pl)->cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type != CLIENT_CMD_DRAWINFO) {
            continue;
        }

        packet_reader_t reader;
        char color[64];
        char message[HUGE_BUF];
        packet_reader_init(&reader, packet->data, packet->len);
        (void)packet_reader_read_uint8(&reader);
        ck_assert(packet_reader_read_string(&reader, VS(color)));
        ck_assert(packet_reader_read_string(&reader, VS(message)));
        ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_NONE);

        if (strcmp(message, expected) == 0) {
            count++;
        }
    }

    return count;
}

START_TEST(test_attack_is_melee_range) {
    mapstruct *map;
    object *pl, *tmp, *tmp2;

    check_setup_env_pl(&map, &pl);
    ck_assert(attack_is_melee_range(pl, pl));

    tmp = arch_get("gazer_dread");
    ck_assert(!attack_is_melee_range(tmp, tmp));
    ck_assert(!attack_is_melee_range(pl, tmp));
    ck_assert(!attack_is_melee_range(tmp, pl));

    tmp->x = pl->x + 1;
    tmp->y = pl->y + 1;
    tmp = object_insert_map(tmp, pl->map, NULL, 0);
    ck_assert(attack_is_melee_range(tmp, tmp));
    ck_assert(attack_is_melee_range(pl, tmp));
    ck_assert(attack_is_melee_range(tmp, pl));

    tmp2 = arch_get("raas");
    ck_assert(!attack_is_melee_range(tmp2, tmp2));
    ck_assert(!attack_is_melee_range(pl, tmp2));
    ck_assert(!attack_is_melee_range(tmp2, pl));

    tmp2->x = pl->x + 2;
    tmp2->y = pl->y + 2;
    tmp2 = object_insert_map(tmp2, pl->map, NULL, 0);
    ck_assert(attack_is_melee_range(tmp2, tmp2));
    ck_assert(!attack_is_melee_range(pl, tmp2));
    ck_assert(!attack_is_melee_range(tmp2, pl));
    ck_assert(attack_is_melee_range(tmp, tmp2));
    ck_assert(attack_is_melee_range(tmp2, tmp));
}
END_TEST

START_TEST(test_attack_roll_adjust_describes_moved_target_penalty) {
    mapstruct *map;
    object *pl, *monster;

    check_setup_env_pl(&map, &pl);
    monster = arch_get("kobold");
    monster->x = pl->x + 1;
    monster->y = pl->y;
    monster = object_insert_map(monster, map, NULL, 0);
    monster_data_init(monster);

    CLEAR_FLAG(monster, FLAG_BLIND);
    CLEAR_FLAG(monster, FLAG_SCARED);
    CLEAR_FLAG(monster, FLAG_CONFUSED);
    CLEAR_FLAG(pl, FLAG_SCARED);
    CLEAR_FLAG(pl, FLAG_UNAGGRESSIVE);
    CLEAR_FLAG(pl, FLAG_CONFUSED);
    CLEAR_MULTI_FLAG(pl, FLAG_IS_INVISIBLE);
    CLEAR_MULTI_FLAG(pl, FLAG_FLYING);
    CLEAR_MULTI_FLAG(monster, FLAG_FLYING);

    monster_data_enemy_update(monster, pl);

    object_remove(pl, 0);
    pl->x = monster->x;
    pl->y = monster->y + 1;
    pl = object_insert_map(pl, map, NULL, 0);

    rv_vector rv;
    ck_assert(get_rangevector(monster, pl, &rv, 0));
    monster->direction = rv.direction;
    pl->direction = absdir(monster->direction + 4);

    StringBuffer *modifiers = stringbuffer_new();
    ck_assert_int_eq(attack_roll_adjust(pl, monster, modifiers), -6);
    ck_assert_str_eq(stringbuffer_data(modifiers), "target moved -6");
    stringbuffer_free(modifiers);
}
END_TEST

START_TEST(test_attack_roll_adjust_describes_positional_bonuses) {
    mapstruct *map;
    object *pl, *target;

    check_setup_env_pl(&map, &pl);
    target = arch_get("kobold");
    target->x = pl->x + 1;
    target->y = pl->y;
    target = object_insert_map(target, map, NULL, 0);

    CLEAR_FLAG(pl, FLAG_BLIND);
    CLEAR_FLAG(pl, FLAG_SCARED);
    CLEAR_FLAG(pl, FLAG_CONFUSED);
    CLEAR_FLAG(target, FLAG_SCARED);
    CLEAR_FLAG(target, FLAG_UNAGGRESSIVE);
    CLEAR_FLAG(target, FLAG_CONFUSED);
    CLEAR_MULTI_FLAG(target, FLAG_IS_INVISIBLE);
    CLEAR_MULTI_FLAG(pl, FLAG_FLYING);
    CLEAR_MULTI_FLAG(target, FLAG_FLYING);

    rv_vector rv;
    ck_assert(get_rangevector(pl, target, &rv, 0));
    pl->direction = rv.direction;

    target->direction = pl->direction;
    StringBuffer *modifiers = stringbuffer_new();
    ck_assert_int_eq(attack_roll_adjust(target, pl, modifiers), 5);
    ck_assert_str_eq(stringbuffer_data(modifiers), "backstab +5");
    stringbuffer_free(modifiers);

    target->direction = absdir(pl->direction + 1);
    modifiers = stringbuffer_new();
    ck_assert_int_eq(attack_roll_adjust(target, pl, modifiers), 2);
    ck_assert_str_eq(stringbuffer_data(modifiers), "sidestab +2");
    stringbuffer_free(modifiers);
}
END_TEST

START_TEST(test_kill_experience_follows_damage_skill_participation) {
    mapstruct *map;
    object *pl, *monster;

    check_setup_env_pl(&map, &pl);
    monster = arch_get("goblin");
    monster->x = pl->x + 1;
    monster->y = pl->y;
    monster->level = 1;
    monster->stats.hp = 100;
    monster->stats.maxhp = 100;
    monster->stats.exp = 1000;
    memset(monster->protection, 0, sizeof(monster->protection));
    monster = object_insert_map(monster, map, NULL, 0);
    monster_data_init(monster);
    monster->enemy = pl;
    monster->enemy_count = pl->count;

    memset(pl->attack, 0, sizeof(pl->attack));
    pl->attack[ATNR_IMPACT] = 100;
    object *saved_chosen_skill = pl->chosen_skill;
    object *saved_unarmed = CONTR(pl)->skill_ptr[SK_UNARMED];
    object *saved_find_traps = CONTR(pl)->skill_ptr[SK_FIND_TRAPS];
    object *unarmed = arch_get("skill_unarmed");
    unarmed->stats.sp = SK_UNARMED;
    object *other_skill = arch_get("skill_find_traps");
    other_skill->stats.sp = SK_FIND_TRAPS;
    CONTR(pl)->skill_ptr[SK_UNARMED] = unarmed;
    CONTR(pl)->skill_ptr[SK_FIND_TRAPS] = other_skill;

    int64_t unarmed_before = unarmed->stats.exp;
    int64_t other_before = other_skill->stats.exp;
    int64_t unarmed_full = calc_skill_exp(pl, monster, unarmed->level);
    int64_t other_full = calc_skill_exp(pl, monster, other_skill->level);

    pl->chosen_skill = unarmed;
    int unarmed_damage = attack_hit(monster, pl, 25);
    ck_assert_int_gt(unarmed_damage, 0);
    ck_assert_int_gt(monster->stats.hp, 0);

    pl->chosen_skill = other_skill;
    int other_damage = attack_hit(monster, pl, 100);
    ck_assert_int_gt(other_damage, unarmed_damage);

    int total_damage = unarmed_damage + other_damage;
    int64_t expected_unarmed =
        (int64_t)((long double)unarmed_full * unarmed_damage / total_damage + 0.5L);
    int64_t expected_other =
        (int64_t)((long double)other_full * other_damage / total_damage + 0.5L);
    ck_assert_int_eq(unarmed->stats.exp - unarmed_before, expected_unarmed);
    ck_assert_int_eq(other_skill->stats.exp - other_before, expected_other);
    ck_assert_int_gt(other_skill->stats.exp - other_before, unarmed->stats.exp - unarmed_before);
    pl->chosen_skill = saved_chosen_skill;
    CONTR(pl)->skill_ptr[SK_UNARMED] = saved_unarmed;
    CONTR(pl)->skill_ptr[SK_FIND_TRAPS] = saved_find_traps;
    object_destroy(unarmed);
    object_destroy(other_skill);
}
END_TEST

START_TEST(test_targeted_melee_gets_one_unaware_opening_bonus) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;

    object *target = attack_test_target(map, pl);
    memset(pl->attack, 0, sizeof(pl->attack));
    pl->attack[ATNR_IMPACT] = 100;
    pl->stats.dam = 4;
    pl->stats.wc_range = 1;
    pl->weapon_speed = 1.0f;
    CONTR(pl)->target_object = target;
    CONTR(pl)->target_object_count = target->count;
    CONTR(pl)->combat = 1;
    CONTR(pl)->action_attack = global_round_tag;

    target->last_damage = 0;
    target->damage_round_tag = global_round_tag;
    object_process(pl);
    ck_assert_int_eq(target->last_damage, 5);
    ck_assert(OBJECT_VALID(target->enemy, target->enemy_count));
    ck_assert_ptr_eq(target->enemy, pl);
    ck_assert_uint_eq(
        attack_test_messages(pl, "Sneak damage bonus: +25% (+1 base damage) — unaware target."),
        1);

    target->last_damage = 0;
    target->damage_round_tag = global_round_tag;
    CONTR(pl)->action_attack = global_round_tag;
    object_process(pl);
    ck_assert_int_eq(target->last_damage, 4);
    ck_assert_uint_eq(
        attack_test_messages(pl, "Sneak damage bonus: +25% (+1 base damage) — unaware target."),
        1);
}
END_TEST

START_TEST(test_situational_bonus_excludes_living_pets_and_plugin_damage_api) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    object *skill = arch_get("skill_bow_archery");
    skill->stats.sp = SK_BOW_ARCHERY;
    pl->chosen_skill = skill;

    object *target = attack_test_target(map, pl);
    object *pet = arch_get("goblin");
    pet->x = pl->x;
    pet->y = pl->y + 1;
    pet = object_insert_map(pet, map, NULL, INS_NO_MERGE);
    monster_data_init(pet);
    object_owner_set(pet, pl);
    ck_assert_ptr_eq(pet->chosen_skill, skill);
    memset(pet->attack, 0, sizeof(pet->attack));
    pet->attack[ATNR_IMPACT] = 100;

    ck_assert_int_eq(attack_hit_situational(target, pet, 4), 4);
    ck_assert_uint_eq(
        attack_test_messages(pl, "Sneak damage bonus: +25% (+1 base damage) — unaware target."),
        0);

    target = attack_test_target(map, pl);
    ck_assert_int_eq(attack_hit(target, pl, 4), 4);
    ck_assert_uint_eq(
        attack_test_messages(pl, "Sneak damage bonus: +25% (+1 base damage) — unaware target."),
        0);

    target = attack_test_target(map, pl);
    object *periodic = arch_get("poisoning");
    memset(periodic->attack, 0, sizeof(periodic->attack));
    periodic->attack[ATNR_INTERNAL] = 100;
    object_owner_set(periodic, pl);
    ck_assert_ptr_eq(periodic->chosen_skill, skill);
    periodic = object_insert_into(periodic, target, 0);
    ck_assert_ptr_nonnull(periodic);
    ck_assert_int_eq(attack_hit_situational(target, periodic, 4), 4);
    ck_assert_uint_eq(
        attack_test_messages(pl, "Sneak damage bonus: +25% (+1 base damage) — unaware target."),
        0);
}
END_TEST

START_TEST(test_unaware_bonus_does_not_increase_status_effect_strength) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    memset(pl->attack, 0, sizeof(pl->attack));
    pl->attack[ATNR_IMPACT] = 50;
    pl->attack[ATNR_BLIND] = 50;
    attack_status_effect_test_override = 1;

    /* The raw compatibility path retains the legacy minimum strength for
     * low, split status attacks. */
    object *raw_target = attack_test_target(map, pl);
    raw_target->speed = 0.1f;
    ck_assert_int_eq(attack_hit(raw_target, pl, 1), 1);
    object *raw_blindness = object_find_arch(raw_target, arch_find("blindness"));
    ck_assert_ptr_nonnull(raw_blindness);
    ck_assert_int_eq(raw_blindness->stats.food, 1);

    object *aware_target = attack_test_target(map, pl);
    aware_target->speed = 0.1f;
    aware_target->enemy = pl;
    aware_target->enemy_count = pl->count;
    ck_assert_int_eq(attack_hit_situational(aware_target, pl, 8), 4);
    object *aware_blindness = object_find_arch(aware_target, arch_find("blindness"));
    ck_assert_ptr_nonnull(aware_blindness);
    ck_assert_int_eq(aware_blindness->stats.food, 4);

    object *unaware_target = attack_test_target(map, pl);
    unaware_target->speed = 0.1f;
    ck_assert_int_eq(attack_hit_situational(unaware_target, pl, 8), 5);
    attack_status_effect_test_override = -1;
    object *unaware_blindness = object_find_arch(unaware_target, arch_find("blindness"));
    ck_assert_ptr_nonnull(unaware_blindness);
    ck_assert_int_eq(unaware_blindness->stats.food, aware_blindness->stats.food);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("attack");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);

    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_attack_is_melee_range);
    tcase_add_test(tc_core, test_attack_roll_adjust_describes_positional_bonuses);
    tcase_add_test(tc_core, test_attack_roll_adjust_describes_moved_target_penalty);
    tcase_add_test(tc_core, test_kill_experience_follows_damage_skill_participation);
    tcase_add_test(tc_core, test_targeted_melee_gets_one_unaware_opening_bonus);
    tcase_add_test(tc_core, test_situational_bonus_excludes_living_pets_and_plugin_damage_api);
    tcase_add_test(tc_core, test_unaware_bonus_does_not_increase_status_effect_strength);

    return s;
}

void check_server_attack(void) {
    check_run_suite(suite(), __FILE__);
}
