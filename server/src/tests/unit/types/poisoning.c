/*************************************************************************
 * Atrinik server poison regression tests.
 ************************************************************************/

#include <global.h>
#include <object_methods.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <attack.h>
#include <object.h>
#include <player.h>
#include <poisoning.h>
#include <spells.h>

static object *find_poison(object *victim) {
    return object_find_arch(victim, arch_find("poisoning"));
}

START_TEST(test_duration_and_refresh_are_bounded) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    object *hitter = arch_get("kobold");

    attack_perform_poison(pl, hitter, 2);
    object *poison = find_poison(pl);
    ck_assert_ptr_nonnull(poison);
    ck_assert_int_eq(poison->stats.food, POISON_BASE_PULSES + 1);

    for (int i = 0; i < 100; i++) {
        attack_perform_poison(pl, hitter, 2);
    }
    ck_assert_int_eq(poison->stats.food, POISON_REFRESH_MAX_PULSES + 1);

    object_destroy(hitter);
}
END_TEST

START_TEST(test_full_immunity_creates_no_effect) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    object *hitter = arch_get("kobold");
    pl->protection[ATNR_POISON] = 100;

    attack_perform_poison(pl, hitter, 2);

    ck_assert_ptr_null(find_poison(pl));
    object_destroy(hitter);
}
END_TEST

START_TEST(test_stat_depletion_is_bounded_and_protection_scaled) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    ck_assert_int_eq(poisoning_stat_depletion_limit(-100), 3);
    ck_assert_int_eq(poisoning_stat_depletion_limit(0), 3);
    ck_assert_int_eq(poisoning_stat_depletion_limit(34), 2);
    ck_assert_int_eq(poisoning_stat_depletion_limit(67), 1);
    ck_assert_int_eq(poisoning_stat_depletion_limit(100), 0);
    ck_assert_int_eq(poisoning_stat_depletion_limit(200), 0);

    object *poison = arch_get("poisoning");
    SET_FLAG(poison, FLAG_APPLIED);
    poison = object_insert_into(poison, pl, 0);

    pl->protection[ATNR_POISON] = 0;
    for (int pulse = 0; pulse < 100; pulse++) {
        poisoning_apply_stat_depletion(poison, pl);
    }
    for (int stat = 0; stat < NUM_STATS; stat++) {
        ck_assert_int_ge(get_attr_value(&poison->stats, stat), -POISON_MAX_STAT_DEPLETION);
        ck_assert_int_le(get_attr_value(&poison->stats, stat), 0);
    }

    pl->protection[ATNR_POISON] = 67;
    poisoning_apply_stat_depletion(poison, pl);
    for (int stat = 0; stat < NUM_STATS; stat++) {
        ck_assert_int_ge(get_attr_value(&poison->stats, stat), -1);
    }

    pl->protection[ATNR_POISON] = 100;
    poisoning_apply_stat_depletion(poison, pl);
    for (int stat = 0; stat < NUM_STATS; stat++) {
        ck_assert_int_eq(get_attr_value(&poison->stats, stat), 0);
    }
}
END_TEST

START_TEST(test_monsters_do_not_receive_poison_stat_depletion) {
    object *monster = arch_get("kobold");
    object *poison = arch_get("poisoning");

    for (int pulse = 0; pulse < 100; pulse++) {
        poisoning_apply_stat_depletion(poison, monster);
    }
    for (int stat = 0; stat < NUM_STATS; stat++) {
        ck_assert_int_eq(get_attr_value(&poison->stats, stat), 0);
    }

    object_destroy(poison);
    object_destroy(monster);
}
END_TEST

START_TEST(test_expiry_and_cure_restore_stats_and_speed) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    int base_str = pl->arch->clone.stats.Str;
    double base_speed = pl->speed;

    object *poison = arch_get("poisoning");
    poison->stats.Str = -3;
    poison->stats.Dex = -3;
    poison->stats.food = POISON_BASE_PULSES + 1;
    SET_FLAG(poison, FLAG_APPLIED);
    poison = object_insert_into(poison, pl, 0);
    living_update_player(pl);
    ck_assert_int_eq(pl->stats.Str, base_str - 3);
    ck_assert_double_le(pl->speed, base_speed);

    for (int pulse = 0; pulse < POISON_BASE_PULSES; pulse++) {
        ck_assert_int_eq(common_object_process_pre(poison), 0);
    }
    ck_assert_int_eq(common_object_process_pre(poison), 1);
    ck_assert_ptr_null(find_poison(pl));
    ck_assert_int_eq(pl->stats.Str, base_str);
    ck_assert(fabs(pl->speed - base_speed) < 0.000001);

    poison = arch_get("poisoning");
    poison->stats.Str = -3;
    poison->stats.Dex = -3;
    poison->stats.food = POISON_BASE_PULSES + 1;
    SET_FLAG(poison, FLAG_APPLIED);
    poison = object_insert_into(poison, pl, 0);
    living_update_player(pl);

    ck_assert(cast_heal(pl, pl, MAXLEVEL, pl, SP_CURE_POISON));
    ck_assert_int_eq(poison->stats.food, 1);
    ck_assert_int_eq(common_object_process_pre(poison), 1);
    ck_assert_ptr_null(find_poison(pl));
    ck_assert_int_eq(pl->stats.Str, base_str);
    ck_assert(fabs(pl->speed - base_speed) < 0.000001);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("poisoning");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_duration_and_refresh_are_bounded);
    tcase_add_test(tc_core, test_full_immunity_creates_no_effect);
    tcase_add_test(tc_core, test_stat_depletion_is_bounded_and_protection_scaled);
    tcase_add_test(tc_core, test_monsters_do_not_receive_poison_stat_depletion);
    tcase_add_test(tc_core, test_expiry_and_cure_restore_stats_and_speed);

    return s;
}

void check_types_poisoning(void) {
    check_run_suite(suite(), __FILE__);
}
