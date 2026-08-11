/*************************************************************************
 * Atrinik server living-system regression tests.
 ************************************************************************/

#include <global.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <attack.h>
#include <commands.h>
#include <disease.h>
#include <object.h>
#include <player.h>
#include <poisoning.h>

static void configure_speed_player(object *pl, int str, int dex, double speed) {
    set_attr_value(&pl->arch->clone.stats, STR, str);
    set_attr_value(&pl->arch->clone.stats, DEX, dex);
    pl->arch->clone.speed = speed;
}

static double player_speed_at_weight(object *pl, uint32_t carrying) {
    pl->carrying = carrying;
    living_update_player(pl);
    return pl->speed;
}

static object *insert_speed_disease(object *pl, int reduction_percent) {
    object *disease = arch_get("smallpox");

    for (int stat = 0; stat < NUM_STATS; stat++) {
        set_attr_value(&disease->stats, stat, 0);
    }
    disease->last_sp = reduction_percent;
    SET_FLAG(disease, FLAG_APPLIED);
    return object_insert_into(disease, pl, 0);
}

START_TEST(test_depletion_tooltip_lists_current_stats) {
    object *depletion = arch_get("depletion");
    set_attr_value(&depletion->stats, 0, -1);
    set_attr_value(&depletion->stats, 2, -3);

    char *tooltip = stringbuffer_finish(depletion_get_tooltip(depletion, NULL));
    ck_assert_ptr_nonnull(strstr(tooltip, "use the remove depletion spell"));
    ck_assert_ptr_null(strstr(tooltip, "restoration"));
    ck_assert_ptr_nonnull(strstr(tooltip, "or see a priest"));
    ck_assert_ptr_nonnull(strstr(tooltip, "Currently depleted: strength (1), constitution (3)."));

    free(tooltip);
    object_destroy(depletion);
}
END_TEST

START_TEST(test_player_encumbrance_curve_has_one_final_floor) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    configure_speed_player(pl, 13, 13, 1.0);

    const uint32_t threshold = weight_limit[13] * 65 / 100;
    double speeds[] = {
        player_speed_at_weight(pl, threshold - 1),
        player_speed_at_weight(pl, threshold),
        player_speed_at_weight(pl, threshold + 1),
        player_speed_at_weight(pl, weight_limit[13] - 1),
        player_speed_at_weight(pl, weight_limit[13]),
        player_speed_at_weight(pl, weight_limit[13] + 1),
    };

    ck_assert(fabs(speeds[0] - speeds[1]) < 0.000001);
    for (size_t i = 0; i < arraysize(speeds); i++) {
        ck_assert_double_ge(speeds[i], PLAYER_MIN_SPEED);
        if (i > 0) {
            ck_assert_double_le(speeds[i], speeds[i - 1]);
        }
    }
    ck_assert(fabs(speeds[3] - PLAYER_MIN_SPEED) < 0.000001);
    ck_assert(fabs(speeds[4] - PLAYER_MIN_SPEED) < 0.000001);
    ck_assert(fabs(speeds[5] - PLAYER_MIN_SPEED) < 0.000001);
}
END_TEST

START_TEST(test_player_speed_floor_follows_all_ordinary_modifiers) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    configure_speed_player(pl, 13, 13, 1.0);

    object *force = arch_get("force");
    force->stats.exp = -9;
    SET_FLAG(force, FLAG_APPLIED);
    force = object_insert_into(force, pl, 0);

    insert_speed_disease(pl, 10);

    object *poison = arch_get("poisoning");
    poison->stats.Str = -3;
    poison->stats.Dex = -3;
    SET_FLAG(poison, FLAG_APPLIED);
    poison = object_insert_into(poison, pl, 0);

    pl->carrying = weight_limit[13] * 65 / 100;
    living_update_player(pl);

    ck_assert(fabs(pl->speed - PLAYER_MIN_SPEED) < 0.000001);
    ck_assert_int_eq((int)ceil(1.0 / PLAYER_MIN_SPEED), 10);
    ck_assert_int_eq((int)(ceil(1.0 / PLAYER_MIN_SPEED) * MAX_TIME / 1000), 1250);
}
END_TEST

START_TEST(test_disease_reduction_precedes_player_floor) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    configure_speed_player(pl, 13, 13, 1.0);

    object *disease = insert_speed_disease(pl, 50);
    living_update_player(pl);
    ck_assert(fabs(pl->speed - 0.5) < 0.000001);

    disease->last_sp = 10;
    living_update_player(pl);
    ck_assert(fabs(pl->speed - PLAYER_MIN_SPEED) < 0.000001);

    disease->last_sp = 9;
    living_update_player(pl);
    ck_assert(fabs(pl->speed - PLAYER_MIN_SPEED) < 0.000001);
}
END_TEST

START_TEST(test_negative_force_reduction_precedes_player_floor) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    configure_speed_player(pl, 13, 13, 1.0);

    object *force = arch_get("force");
    force->stats.exp = -1;
    SET_FLAG(force, FLAG_APPLIED);
    force = object_insert_into(force, pl, 0);
    living_update_player(pl);
    ck_assert(fabs(pl->speed - 0.5) < 0.000001);

    force->stats.exp = -9;
    living_update_player(pl);
    ck_assert(fabs(pl->speed - PLAYER_MIN_SPEED) < 0.000001);

    force->stats.exp = -10;
    living_update_player(pl);
    ck_assert(fabs(pl->speed - PLAYER_MIN_SPEED) < 0.000001);
}
END_TEST

START_TEST(test_dexterity_loss_reduces_speed_before_encumbrance) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    configure_speed_player(pl, 13, 13, 1.0);
    living_update_player(pl);
    double base_speed = pl->speed;

    object *poison = arch_get("poisoning");
    poison->stats.Dex = -POISON_MAX_STAT_DEPLETION;
    SET_FLAG(poison, FLAG_APPLIED);
    poison = object_insert_into(poison, pl, 0);
    living_update_player(pl);

    ck_assert_int_eq(pl->stats.Dex, 10);
    ck_assert(fabs(pl->speed - (base_speed + speed_bonus[10] - speed_bonus[13])) < 0.000001);

    pl->carrying = weight_limit[pl->stats.Str] * 65 / 100 + 1;
    living_update_player(pl);
    ck_assert_double_lt(pl->speed, base_speed + speed_bonus[10] - speed_bonus[13]);
    ck_assert_double_ge(pl->speed, PLAYER_MIN_SPEED);
}
END_TEST

START_TEST(test_poison_encumbrance_regression_uses_recoverable_floor) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    configure_speed_player(pl, 13, 13, 1.0);

    pl->carrying = 100000;
    living_update_player(pl);
    ck_assert_double_gt(pl->speed, PLAYER_MIN_SPEED);

    object *poison = arch_get("poisoning");
    poison->stats.Str = -POISON_MAX_STAT_DEPLETION;
    poison->stats.Dex = -POISON_MAX_STAT_DEPLETION;
    SET_FLAG(poison, FLAG_APPLIED);
    poison = object_insert_into(poison, pl, 0);

    living_update_player(pl);
    ck_assert(fabs(pl->speed - PLAYER_MIN_SPEED) < 0.000001);

    /* A load at the pre-poison 65% boundary remains above the floor even at
     * the maximum ordinary poison depletion. */
    pl->carrying = weight_limit[13] * 65 / 100;
    living_update_player(pl);
    ck_assert_double_gt(pl->speed, PLAYER_MIN_SPEED);
}
END_TEST

START_TEST(test_paralysis_timing_remains_in_speed_credit) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    configure_speed_player(pl, 13, 13, 1.0);
    pl->carrying = weight_limit[13];
    living_update_player(pl);
    pl->speed_left = 0.0;

    attack_peform_paralyze(pl, 4.0);
    double paralyzed_until = pl->speed_left;
    ck_assert(fabs(paralyzed_until - -(PLAYER_MIN_SPEED * 12.0)) < 0.000001);

    living_update_player(pl);
    ck_assert_double_eq(pl->speed_left, paralyzed_until);

    for (int tick = 0; tick < 11; tick++) {
        pl->speed_left += fabs(pl->speed);
    }
    ck_assert_double_lt(pl->speed_left, 0.0);
    pl->speed_left += fabs(pl->speed);
    ck_assert(fabs(pl->speed_left) < 0.000001);
}
END_TEST

START_TEST(test_administrative_freeze_keeps_requested_tick_duration) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    configure_speed_player(pl, 13, 13, 1.0);
    pl->carrying = weight_limit[13];
    living_update_player(pl);

    char params[MAX_BUF];
    snprintf(params, sizeof(params), "%s 10", pl->name);
    command_freeze(pl, "freeze", params);
    ck_assert(fabs(pl->speed_left - -(PLAYER_MIN_SPEED * 10.0)) < 0.000001);

    living_update_player(pl);
    for (int tick = 0; tick < 9; tick++) {
        pl->speed_left += fabs(pl->speed);
    }
    ck_assert_double_lt(pl->speed_left, 0.0);
    pl->speed_left += fabs(pl->speed);
    ck_assert(fabs(pl->speed_left) < 0.000001);
}
END_TEST

START_TEST(test_non_player_speed_is_not_clamped) {
    object *poison = arch_get("poisoning");
    poison->speed = -0.015;

    object_update_speed(poison);

    ck_assert(fabs(poison->speed - -0.015) < 0.000001);
    object_destroy(poison);
}
END_TEST

START_TEST(test_depletion_force_is_applied_before_stat_updates) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    drain_specific_stat(pl, 0);

    object *depletion = object_find_arch(pl, arch_find("depletion"));
    ck_assert_ptr_nonnull(depletion);
    ck_assert(QUERY_FLAG(depletion, FLAG_APPLIED));
    ck_assert_int_eq(get_attr_value(&depletion->stats, 0), -1);
}
END_TEST

START_TEST(test_reduce_symptoms_ignores_non_progressive_symptoms) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    object *symptom = arch_get("symptom");
    symptom->value = 0;
    symptom->speed_left = 7.0;
    symptom = object_insert_into(symptom, pl, 0);

    ck_assert(!disease_reduce_symptoms(pl, 5));
    ck_assert_int_eq(symptom->value, 0);
    ck_assert_double_eq(symptom->speed_left, 7.0);
}
END_TEST

START_TEST(test_reduce_symptoms_reduces_and_reschedules_progressive_symptoms) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    object *symptom = arch_get("symptom");
    symptom->value = 30;
    symptom->speed_left = 7.0;
    symptom = object_insert_into(symptom, pl, 0);

    ck_assert(disease_reduce_symptoms(pl, 5));
    ck_assert_int_eq(symptom->value, 20);
    ck_assert_double_eq(symptom->speed_left, 0.0);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("living");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_depletion_tooltip_lists_current_stats);
    tcase_add_test(tc_core, test_depletion_force_is_applied_before_stat_updates);
    tcase_add_test(tc_core, test_reduce_symptoms_ignores_non_progressive_symptoms);
    tcase_add_test(tc_core, test_reduce_symptoms_reduces_and_reschedules_progressive_symptoms);
    tcase_add_test(tc_core, test_player_encumbrance_curve_has_one_final_floor);
    tcase_add_test(tc_core, test_player_speed_floor_follows_all_ordinary_modifiers);
    tcase_add_test(tc_core, test_disease_reduction_precedes_player_floor);
    tcase_add_test(tc_core, test_negative_force_reduction_precedes_player_floor);
    tcase_add_test(tc_core, test_dexterity_loss_reduces_speed_before_encumbrance);
    tcase_add_test(tc_core, test_poison_encumbrance_regression_uses_recoverable_floor);
    tcase_add_test(tc_core, test_paralysis_timing_remains_in_speed_credit);
    tcase_add_test(tc_core, test_administrative_freeze_keeps_requested_tick_duration);
    tcase_add_test(tc_core, test_non_player_speed_is_not_clamped);

    return s;
}

void check_server_living(void) {
    check_run_suite(suite(), __FILE__);
}
