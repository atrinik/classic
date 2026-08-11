/*************************************************************************
 * Atrinik server disease symptom regression tests.
 ************************************************************************/

#include <global.h>
#include <object_methods.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <attack.h>
#include <object.h>

static object *insert_symptom(object *victim, int damage, int mana_drain) {
    object *symptom = arch_get("symptom");
    symptom->stats.dam = damage;
    symptom->stats.maxsp = mana_drain;
    symptom->attack[ATNR_INTERNAL] = 100;
    FREE_AND_COPY_HASH(symptom->msg, "You feel ill.");
    return object_insert_into(symptom, victim, 0);
}

static void process_symptom(object *symptom) {
    ck_assert_ptr_nonnull(OBJECT_METHODS(SYMPTOM)->process_func);
    OBJECT_METHODS(SYMPTOM)->process_func(symptom);
}

START_TEST(test_zero_damage_and_mana_are_noops) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    object *symptom = insert_symptom(pl, 0, 0);
    pl->stats.hp = 83;
    pl->stats.maxhp = 100;
    pl->stats.sp = 47;
    pl->stats.maxsp = 100;

    process_symptom(symptom);

    ck_assert_int_eq(pl->stats.hp, 83);
    ck_assert_int_eq(pl->stats.sp, 47);
}
END_TEST

START_TEST(test_positive_values_are_fixed_amounts) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    object *symptom = insert_symptom(pl, 12, 9);
    pl->stats.hp = 83;
    pl->stats.maxhp = 100;
    pl->stats.sp = 7;
    pl->stats.maxsp = 100;

    process_symptom(symptom);

    ck_assert_int_eq(pl->stats.hp, 71);
    ck_assert_int_eq(pl->stats.sp, 0);
}
END_TEST

START_TEST(test_negative_values_use_maximum_resource_percentages) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    object *symptom = insert_symptom(pl, -10, -25);
    pl->stats.hp = 100;
    pl->stats.maxhp = 200;
    pl->stats.sp = 80;
    pl->stats.maxsp = 200;

    process_symptom(symptom);

    ck_assert_int_eq(pl->stats.hp, 80);
    ck_assert_int_eq(pl->stats.sp, 30);
}
END_TEST

START_TEST(test_percentage_damage_is_nonlethal_and_mana_is_bounded) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    object *symptom = insert_symptom(pl, -10, -10);
    pl->stats.hp = 5;
    pl->stats.maxhp = 100;
    pl->stats.sp = 3;
    pl->stats.maxsp = 100;

    process_symptom(symptom);

    ck_assert_int_eq(pl->stats.hp, 1);
    ck_assert_int_eq(pl->stats.sp, 0);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("symptom");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_zero_damage_and_mana_are_noops);
    tcase_add_test(tc_core, test_positive_values_are_fixed_amounts);
    tcase_add_test(tc_core, test_negative_values_use_maximum_resource_percentages);
    tcase_add_test(tc_core, test_percentage_damage_is_nonlethal_and_mana_is_bounded);

    return s;
}

void check_types_symptom(void) {
    check_run_suite(suite(), __FILE__);
}
