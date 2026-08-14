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

/** @file Deterministic fixed-point lunar orbit and starlight evaluation. */

#include <global.h>
#include <celestial_lunar.h>
#include <tod.h>

#define CELESTIAL_SEASON_HOURS HOURS_PER_YEAR
#define CELESTIAL_LUNAR_PERIOD_MIN 168
#define CELESTIAL_LUNAR_PERIOD_MAX HOURS_PER_YEAR
#define CELESTIAL_MOON_MAX 20
#define CELESTIAL_STARLIGHT_MAX 2

_Static_assert(HOURS_PER_MONTH == 672, "celestial-v1 requires a 672-hour root lunar period");
_Static_assert(HOURS_PER_YEAR == 8064, "celestial-v1 requires an 8064-hour seasonal period");

static const int32_t orbit_elevation[HOURS_PER_DAY] = {
    -32768, -31651, -28378, -23170, -16384, -8481, 0, 8481,  16384,  23170,  28378,  31651,
    32768,  31651,  28378,  23170,  16384,  8481,  0, -8481, -16384, -23170, -28378, -31651,
};

static const uint8_t orbit_azimuth[HOURS_PER_DAY] = {
    NORTH,     NORTH,     NORTHEAST, NORTHEAST, NORTHEAST, EAST,      EAST,      EAST,
    SOUTHEAST, SOUTHEAST, SOUTH,     SOUTH,     SOUTH,     SOUTH,     SOUTH,     SOUTHWEST,
    SOUTHWEST, WEST,      WEST,      WEST,      NORTHWEST, NORTHWEST, NORTHWEST, NORTH,
};

static const uint16_t season_factor[MONTHS_PER_YEAR] = {
    24576,
    25600,
    27648,
    29952,
    31744,
    32768,
    32768,
    31744,
    29952,
    27648,
    25600,
    24576,
};

static uint64_t divide_round_half_up(uint64_t numerator, uint64_t denominator) {
    HARD_ASSERT(denominator != 0);
    return numerator / denominator + (numerator % denominator >= (denominator + 1) / 2 ? 1 : 0);
}

static bool normalized_color_valid(const uint16_t color[3], uint8_t strength) {
    uint16_t peak = MAX(color[0], MAX(color[1], color[2]));
    return peak == UINT16_MAX || (peak == 0 && strength == 0);
}

static int32_t seasonal_solar_elevation(uint16_t solar_hour, uint16_t season_phase) {
    int32_t elevation = orbit_elevation[solar_hour];
    uint16_t factor = season_factor[season_phase / HOURS_PER_MONTH];
    uint64_t magnitude = divide_round_half_up((uint64_t)abs(elevation) * factor, 32768);
    return elevation < 0 ? -(int32_t)magnitude : (int32_t)magnitude;
}

static bool solar_is_night(uint16_t solar_hour, uint16_t season_phase) {
    uint16_t factor = season_factor[season_phase / HOURS_PER_MONTH];
    int32_t twilight = -(int32_t)divide_round_half_up(UINT64_C(8481) * factor, 32768);
    return seasonal_solar_elevation(solar_hour, season_phase) < twilight;
}

static void project_color(uint8_t strength, const uint16_t color[3], uint16_t output[3]) {
    for (size_t channel = 0; channel < 3; channel++) {
        output[channel] =
            (uint16_t)divide_round_half_up((uint64_t)strength * color[channel], UINT16_MAX);
    }
}

void celestial_lunar_root_input(uint64_t absolute_hour, celestial_lunar_input *input) {
    HARD_ASSERT(input != NULL);

    memset(input, 0, sizeof(*input));
    input->solar_hour = absolute_hour % HOURS_PER_DAY;
    input->season_phase = absolute_hour % HOURS_PER_YEAR;
    input->lunar_age = absolute_hour % HOURS_PER_MONTH;
    input->lunar_period = HOURS_PER_MONTH;
    input->moon_color[0] = 34544;
    input->moon_color[1] = 41337;
    input->moon_color[2] = 65535;
    input->starlight_color[0] = 14544;
    input->starlight_color[1] = 26837;
    input->starlight_color[2] = 65535;
    input->moon_max = 20;
    input->starlight_strength = 2;
}

static void copy_revision(const celestial_lunar_input *input, celestial_lunar_input *revision) {
    revision->solar_hour = input->solar_hour;
    revision->season_phase = input->season_phase;
    revision->lunar_age = input->lunar_age;
    revision->lunar_period = input->lunar_period;
    memcpy(revision->moon_color, input->moon_color, sizeof(revision->moon_color));
    memcpy(revision->starlight_color, input->starlight_color, sizeof(revision->starlight_color));
    revision->moon_max = input->moon_max;
    revision->starlight_strength = input->starlight_strength;
}

bool celestial_lunar_evaluate(const celestial_lunar_input *input, celestial_lunar_sample *sample) {
    if (sample == NULL) {
        return false;
    }
    memset(sample, 0, sizeof(*sample));
    if (input == NULL || input->solar_hour >= HOURS_PER_DAY ||
        input->season_phase >= HOURS_PER_YEAR || input->lunar_period < CELESTIAL_LUNAR_PERIOD_MIN ||
        input->lunar_period > CELESTIAL_LUNAR_PERIOD_MAX ||
        input->lunar_period % HOURS_PER_DAY != 0 || input->lunar_period % 8 != 0 ||
        input->lunar_age >= input->lunar_period || input->moon_max > CELESTIAL_MOON_MAX ||
        input->starlight_strength > CELESTIAL_STARLIGHT_MAX ||
        !normalized_color_valid(input->moon_color, input->moon_max) ||
        !normalized_color_valid(input->starlight_color, input->starlight_strength)) {
        return false;
    }

    copy_revision(input, &sample->revision);
    sample->phase = (celestial_lunar_phase)(((uint64_t)input->lunar_age * 8) / input->lunar_period);

    uint16_t half_period = input->lunar_period / 2;
    uint16_t lit_age =
        input->lunar_age <= half_period ? input->lunar_age : input->lunar_period - input->lunar_age;
    sample->illumination =
        (uint16_t)divide_round_half_up((uint64_t)lit_age * UINT16_MAX, half_period);

    uint8_t elongation =
        (uint8_t)(((uint64_t)input->lunar_age * HOURS_PER_DAY) / input->lunar_period);
    uint8_t transit = (12 + elongation) % HOURS_PER_DAY;
    sample->moon_hour = (input->solar_hour + 36 - transit) % HOURS_PER_DAY;
    sample->elevation = orbit_elevation[sample->moon_hour];
    sample->azimuth = orbit_azimuth[sample->moon_hour];
    sample->visible = sample->elevation > 0;

    if (sample->illumination != 0 && sample->visible && input->moon_max != 0) {
        uint64_t numerator =
            (uint64_t)input->moon_max * sample->illumination * (uint32_t)sample->elevation;
        sample->moon_strength =
            (uint8_t)divide_round_half_up(numerator, UINT64_C(65535) * UINT64_C(32768));
        project_color(sample->moon_strength, input->moon_color, sample->moon_radiance);
    }

    if (solar_is_night(input->solar_hour, input->season_phase)) {
        sample->starlight_strength = input->starlight_strength;
        project_color(sample->starlight_strength,
                      input->starlight_color,
                      sample->starlight_radiance);
    }
    return true;
}
