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
#include <celestial_lunar.h>
#include <tod.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>

static celestial_lunar_sample root_sample(uint64_t absolute_hour) {
    celestial_lunar_input input;
    celestial_lunar_sample sample;
    celestial_lunar_root_input(absolute_hour, &input);
    ck_assert(celestial_lunar_evaluate(&input, &sample));
    return sample;
}

START_TEST(test_exact_phase_anchors) {
    static const struct {
        uint16_t age;
        celestial_lunar_phase phase;
        uint16_t illumination;
        uint8_t transit;
        uint8_t strength;
    } vectors[] = {
        {0, CELESTIAL_LUNAR_NEW, 0, 12, 0},
        {84, CELESTIAL_LUNAR_WAXING_CRESCENT, 16384, 15, 5},
        {168, CELESTIAL_LUNAR_FIRST_QUARTER, 32768, 18, 10},
        {252, CELESTIAL_LUNAR_WAXING_GIBBOUS, 49151, 21, 15},
        {336, CELESTIAL_LUNAR_FULL, 65535, 0, 20},
        {420, CELESTIAL_LUNAR_WANING_GIBBOUS, 49151, 3, 15},
        {504, CELESTIAL_LUNAR_LAST_QUARTER, 32768, 6, 10},
        {588, CELESTIAL_LUNAR_WANING_CRESCENT, 16384, 9, 5},
    };

    for (size_t i = 0; i < arraysize(vectors); i++) {
        celestial_lunar_input input;
        celestial_lunar_sample sample;
        celestial_lunar_root_input(vectors[i].age, &input);
        input.solar_hour = vectors[i].transit;
        ck_assert(celestial_lunar_evaluate(&input, &sample));
        ck_assert_int_eq(sample.phase, vectors[i].phase);
        ck_assert_uint_eq(sample.illumination, vectors[i].illumination);
        ck_assert_uint_eq(sample.moon_hour, 12);
        ck_assert_int_eq(sample.elevation, 32768);
        ck_assert_uint_eq(sample.azimuth, SOUTH);
        ck_assert(sample.visible);
        ck_assert_uint_eq(sample.moon_strength, vectors[i].strength);
    }
}
END_TEST

START_TEST(test_orbit_rise_overhead_set_and_below_horizon) {
    celestial_lunar_input input;
    celestial_lunar_sample sample;
    celestial_lunar_root_input(336, &input);

    input.solar_hour = 18;
    ck_assert(celestial_lunar_evaluate(&input, &sample));
    ck_assert_uint_eq(sample.moon_hour, 6);
    ck_assert_int_eq(sample.elevation, 0);
    ck_assert(!sample.visible);
    ck_assert_uint_eq(sample.moon_strength, 0);

    input.solar_hour = 0;
    ck_assert(celestial_lunar_evaluate(&input, &sample));
    ck_assert_uint_eq(sample.moon_hour, 12);
    ck_assert_int_eq(sample.elevation, 32768);
    ck_assert(sample.visible);
    ck_assert_uint_eq(sample.moon_strength, 20);
    ck_assert_uint_eq(sample.moon_radiance[0], 11);
    ck_assert_uint_eq(sample.moon_radiance[1], 13);
    ck_assert_uint_eq(sample.moon_radiance[2], 20);

    input.solar_hour = 6;
    ck_assert(celestial_lunar_evaluate(&input, &sample));
    ck_assert_uint_eq(sample.moon_hour, 18);
    ck_assert_int_eq(sample.elevation, 0);
    ck_assert(!sample.visible);

    input.solar_hour = 12;
    ck_assert(celestial_lunar_evaluate(&input, &sample));
    ck_assert_uint_eq(sample.moon_hour, 0);
    ck_assert_int_eq(sample.elevation, -32768);
    ck_assert_uint_eq(sample.azimuth, NORTH);
    ck_assert(!sample.visible);
    ck_assert_uint_eq(sample.moon_strength, 0);
}
END_TEST

START_TEST(test_new_moon_and_moonless_night_retain_only_starlight) {
    celestial_lunar_sample sample = root_sample(0);
    ck_assert(sample.visible == false);
    ck_assert_uint_eq(sample.moon_strength, 0);
    ck_assert_uint_eq(sample.starlight_strength, 2);
    ck_assert_uint_eq(sample.starlight_radiance[0], 0);
    ck_assert_uint_eq(sample.starlight_radiance[1], 1);
    ck_assert_uint_eq(sample.starlight_radiance[2], 2);

    celestial_lunar_input input;
    celestial_lunar_root_input(336, &input);
    input.solar_hour = 12;
    ck_assert(celestial_lunar_evaluate(&input, &sample));
    ck_assert_uint_eq(sample.illumination, 65535);
    ck_assert(!sample.visible);
    ck_assert_uint_eq(sample.moon_strength, 0);
    ck_assert_uint_eq(sample.starlight_strength, 0);

    celestial_lunar_root_input(84, &input);
    input.solar_hour = 0;
    ck_assert(celestial_lunar_evaluate(&input, &sample));
    ck_assert(!sample.visible);
    ck_assert_uint_eq(sample.moon_strength, 0);
    ck_assert_uint_eq(sample.starlight_strength, 2);

    sample = root_sample(336 + 5);
    ck_assert_uint_eq(sample.starlight_strength, 0);
    sample = root_sample(336 + 19);
    ck_assert_uint_eq(sample.starlight_strength, 0);
}
END_TEST

START_TEST(test_phase_strength_is_monotonic_and_bounded) {
    uint16_t previous = 0;
    for (uint16_t age = 0; age <= HOURS_PER_MONTH / 2; age++) {
        celestial_lunar_input input;
        celestial_lunar_sample sample;
        celestial_lunar_root_input(age, &input);
        input.solar_hour = (12 + (age * HOURS_PER_DAY) / HOURS_PER_MONTH) % HOURS_PER_DAY;
        ck_assert(celestial_lunar_evaluate(&input, &sample));
        ck_assert_uint_ge(sample.illumination, previous);
        ck_assert_uint_le(sample.moon_strength, 20);
        previous = sample.illumination;
    }
    for (uint16_t age = HOURS_PER_MONTH / 2 + 1; age < HOURS_PER_MONTH; age++) {
        celestial_lunar_input input;
        celestial_lunar_sample sample;
        celestial_lunar_root_input(age, &input);
        input.solar_hour = (12 + (age * HOURS_PER_DAY) / HOURS_PER_MONTH) % HOURS_PER_DAY;
        ck_assert(celestial_lunar_evaluate(&input, &sample));
        ck_assert_uint_le(sample.illumination, previous);
        previous = sample.illumination;
    }

    ck_assert_uint_lt(2, 20);
    ck_assert_uint_lt(20, 1280);
}
END_TEST

START_TEST(test_absolute_clock_modulo_boundaries_and_maximum) {
    const uint64_t boundaries[] = {
        HOURS_PER_DAY - 1,
        HOURS_PER_DAY,
        HOURS_PER_MONTH - 1,
        HOURS_PER_MONTH,
        (HOURS_PER_YEAR / SEASONS_PER_YEAR) - 1,
        HOURS_PER_YEAR / SEASONS_PER_YEAR,
        HOURS_PER_YEAR - 1,
        HOURS_PER_YEAR,
        UINT64_MAX,
    };

    for (size_t i = 0; i < arraysize(boundaries); i++) {
        celestial_lunar_input input;
        celestial_lunar_root_input(boundaries[i], &input);
        ck_assert_uint_eq(input.solar_hour, boundaries[i] % HOURS_PER_DAY);
        ck_assert_uint_eq(input.season_phase, boundaries[i] % HOURS_PER_YEAR);
        ck_assert_uint_eq(input.lunar_age, boundaries[i] % HOURS_PER_MONTH);
        ck_assert_uint_eq(input.lunar_period, HOURS_PER_MONTH);
    }
}
END_TEST

START_TEST(test_explicit_fixed_and_scaled_effective_inputs_are_stable) {
    celestial_lunar_input fixed;
    celestial_lunar_sample first;
    celestial_lunar_sample second;
    celestial_lunar_root_input(UINT64_MAX, &fixed);
    fixed.solar_hour = 12;
    fixed.season_phase = 3360;
    fixed.lunar_age = 336;

    ck_assert(celestial_lunar_evaluate(&fixed, &first));
    (void)root_sample(12345);
    ck_assert(celestial_lunar_evaluate(&fixed, &second));
    ck_assert_mem_eq(&first, &second, sizeof(first));
    ck_assert_uint_eq(first.revision.absolute_hour, UINT64_MAX);
    ck_assert_uint_eq(first.moon_hour, 0);
    ck_assert_uint_eq(first.moon_strength, 0);

    celestial_lunar_input scaled = fixed;
    scaled.absolute_hour = 18;
    scaled.solar_hour = 9;
    scaled.season_phase = 18;
    scaled.lunar_age = 18;
    ck_assert(celestial_lunar_evaluate(&scaled, &first));
    ck_assert_uint_eq(first.revision.solar_hour, 9);
    ck_assert_uint_eq(first.revision.season_phase, 18);
    ck_assert_uint_eq(first.revision.lunar_age, 18);
}
END_TEST

START_TEST(test_all_phase_orbit_rows_are_deterministic) {
    const uint16_t anchors[] = {0, 84, 168, 252, 336, 420, 504, 588};
    for (size_t phase = 0; phase < arraysize(anchors); phase++) {
        for (uint16_t solar_hour = 0; solar_hour < HOURS_PER_DAY; solar_hour++) {
            celestial_lunar_input input;
            celestial_lunar_sample cold;
            celestial_lunar_sample warm;
            celestial_lunar_root_input(anchors[phase], &input);
            input.solar_hour = solar_hour;
            ck_assert(celestial_lunar_evaluate(&input, &cold));
            (void)root_sample(UINT64_MAX - solar_hour);
            ck_assert(celestial_lunar_evaluate(&input, &warm));
            ck_assert_mem_eq(&cold, &warm, sizeof(cold));
            ck_assert_uint_lt(cold.moon_hour, HOURS_PER_DAY);
            ck_assert_uint_ge(cold.azimuth, NORTH);
            ck_assert_uint_le(cold.azimuth, NORTHWEST);
            ck_assert_int_eq(cold.visible, cold.elevation > 0);
        }
    }
}
END_TEST

START_TEST(test_invalid_profiles_fail_closed) {
    celestial_lunar_input input;
    celestial_lunar_sample sample;
    celestial_lunar_root_input(0, &input);

    ck_assert(!celestial_lunar_evaluate(NULL, &sample));
    ck_assert(!celestial_lunar_evaluate(&input, NULL));
    input.lunar_period = 167;
    ck_assert(!celestial_lunar_evaluate(&input, &sample));
    ck_assert_mem_eq(&sample, &(celestial_lunar_sample){0}, sizeof(sample));

    celestial_lunar_root_input(0, &input);
    input.lunar_age = input.lunar_period;
    ck_assert(!celestial_lunar_evaluate(&input, &sample));
    celestial_lunar_root_input(0, &input);
    input.solar_hour = HOURS_PER_DAY;
    ck_assert(!celestial_lunar_evaluate(&input, &sample));
    celestial_lunar_root_input(0, &input);
    input.season_phase = HOURS_PER_YEAR;
    ck_assert(!celestial_lunar_evaluate(&input, &sample));
    celestial_lunar_root_input(0, &input);
    input.moon_max = 21;
    ck_assert(!celestial_lunar_evaluate(&input, &sample));
    celestial_lunar_root_input(0, &input);
    input.starlight_strength = 3;
    ck_assert(!celestial_lunar_evaluate(&input, &sample));
    celestial_lunar_root_input(0, &input);
    input.moon_color[2] = 65534;
    ck_assert(!celestial_lunar_evaluate(&input, &sample));
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("celestial_lunar");
    TCase *tc = tcase_create("fixed_point_orbit");
    tcase_add_test(tc, test_exact_phase_anchors);
    tcase_add_test(tc, test_orbit_rise_overhead_set_and_below_horizon);
    tcase_add_test(tc, test_new_moon_and_moonless_night_retain_only_starlight);
    tcase_add_test(tc, test_phase_strength_is_monotonic_and_bounded);
    tcase_add_test(tc, test_absolute_clock_modulo_boundaries_and_maximum);
    tcase_add_test(tc, test_explicit_fixed_and_scaled_effective_inputs_are_stable);
    tcase_add_test(tc, test_all_phase_orbit_rows_are_deterministic);
    tcase_add_test(tc, test_invalid_profiles_fail_closed);
    suite_add_tcase(s, tc);
    return s;
}

void check_server_celestial_lunar(void) {
    check_run_suite(suite(), __FILE__);
}
