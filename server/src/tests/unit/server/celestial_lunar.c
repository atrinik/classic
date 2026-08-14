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
    celestial_lunar_input next_hour;
    celestial_lunar_sample first;
    celestial_lunar_sample second;
    celestial_lunar_root_input(100, &fixed);
    fixed.solar_hour = 12;
    fixed.season_phase = 3360;
    fixed.lunar_age = 336;
    celestial_lunar_root_input(101, &next_hour);
    next_hour.solar_hour = 12;
    next_hour.season_phase = 3360;
    next_hour.lunar_age = 336;

    ck_assert(celestial_lunar_evaluate(&fixed, &first));
    (void)root_sample(12345);
    ck_assert(celestial_lunar_evaluate(&next_hour, &second));
    ck_assert_mem_eq(&first, &second, sizeof(first));
    ck_assert_uint_eq(first.moon_hour, 0);
    ck_assert_uint_eq(first.moon_strength, 0);

    celestial_lunar_input scaled = fixed;
    scaled.solar_hour = 9;
    scaled.season_phase = 18;
    scaled.lunar_age = 18;
    ck_assert(celestial_lunar_evaluate(&scaled, &first));
    ck_assert_uint_eq(first.revision.solar_hour, 9);
    ck_assert_uint_eq(first.revision.season_phase, 18);
    ck_assert_uint_eq(first.revision.lunar_age, 18);
}
END_TEST

START_TEST(test_semantically_equal_inputs_ignore_object_padding) {
    celestial_lunar_input first_input;
    celestial_lunar_input second_input;
    celestial_lunar_sample first;
    celestial_lunar_sample second;
    memset(&first_input, 0xaa, sizeof(first_input));
    memset(&second_input, 0x55, sizeof(second_input));

    celestial_lunar_input root;
    celestial_lunar_root_input(336, &root);
    first_input.solar_hour = second_input.solar_hour = root.solar_hour;
    first_input.season_phase = second_input.season_phase = root.season_phase;
    first_input.lunar_age = second_input.lunar_age = root.lunar_age;
    first_input.lunar_period = second_input.lunar_period = root.lunar_period;
    memcpy(first_input.moon_color, root.moon_color, sizeof(root.moon_color));
    memcpy(second_input.moon_color, root.moon_color, sizeof(root.moon_color));
    memcpy(first_input.starlight_color, root.starlight_color, sizeof(root.starlight_color));
    memcpy(second_input.starlight_color, root.starlight_color, sizeof(root.starlight_color));
    first_input.moon_max = second_input.moon_max = root.moon_max;
    first_input.starlight_strength = second_input.starlight_strength = root.starlight_strength;

    ck_assert(celestial_lunar_evaluate(&first_input, &first));
    ck_assert(celestial_lunar_evaluate(&second_input, &second));
    ck_assert_mem_eq(&first, &second, sizeof(first));
}
END_TEST

START_TEST(test_all_phase_orbit_rows_are_deterministic) {
    const uint16_t anchors[] = {0, 84, 168, 252, 336, 420, 504, 588};
    const int32_t expected_elevation[HOURS_PER_DAY] = {
        -32768, -31651, -28378, -23170, -16384, -8481, 0, 8481,  16384,  23170,  28378,  31651,
        32768,  31651,  28378,  23170,  16384,  8481,  0, -8481, -16384, -23170, -28378, -31651,
    };
    const uint8_t expected_azimuth[HOURS_PER_DAY] = {
        NORTH,     NORTH,     NORTHEAST, NORTHEAST, NORTHEAST, EAST,      EAST,      EAST,
        SOUTHEAST, SOUTHEAST, SOUTH,     SOUTH,     SOUTH,     SOUTH,     SOUTH,     SOUTHWEST,
        SOUTHWEST, WEST,      WEST,      WEST,      NORTHWEST, NORTHWEST, NORTHWEST, NORTH,
    };
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
            ck_assert_int_eq(cold.elevation, expected_elevation[cold.moon_hour]);
            ck_assert_uint_eq(cold.azimuth, expected_azimuth[cold.moon_hour]);
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
    celestial_lunar_root_input(0, &input);
    memset(input.moon_color, 0, sizeof(input.moon_color));
    ck_assert(!celestial_lunar_evaluate(&input, &sample));
    input.moon_max = 0;
    ck_assert(celestial_lunar_evaluate(&input, &sample));
    celestial_lunar_root_input(0, &input);
    memset(input.starlight_color, 0, sizeof(input.starlight_color));
    ck_assert(!celestial_lunar_evaluate(&input, &sample));
    input.starlight_strength = 0;
    ck_assert(celestial_lunar_evaluate(&input, &sample));
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
    tcase_add_test(tc, test_semantically_equal_inputs_ignore_object_padding);
    tcase_add_test(tc, test_all_phase_orbit_rows_are_deterministic);
    tcase_add_test(tc, test_invalid_profiles_fail_closed);
    suite_add_tcase(s, tc);
    return s;
}

void check_server_celestial_lunar(void) {
    check_run_suite(suite(), __FILE__);
}
