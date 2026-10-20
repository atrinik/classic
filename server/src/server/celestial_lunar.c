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

/** @file Root lunar profile; the orbit itself is libatrinik's celestial model. */

#include <global.h>
#include <celestial_lunar.h>
#include <tod.h>

_Static_assert(HOURS_PER_DAY == CELESTIAL_MODEL_HOURS_PER_DAY,
               "celestial-v1 requires a 24-hour solar day");
_Static_assert(HOURS_PER_MONTH == CELESTIAL_MODEL_HOURS_PER_MONTH,
               "celestial-v1 requires a 672-hour root lunar period");
_Static_assert(HOURS_PER_YEAR == CELESTIAL_MODEL_HOURS_PER_YEAR,
               "celestial-v1 requires an 8064-hour seasonal period");
_Static_assert(NORTH == CELESTIAL_MODEL_NORTH && NORTHEAST == CELESTIAL_MODEL_NORTHEAST &&
                   EAST == CELESTIAL_MODEL_EAST && SOUTHEAST == CELESTIAL_MODEL_SOUTHEAST &&
                   SOUTH == CELESTIAL_MODEL_SOUTH && SOUTHWEST == CELESTIAL_MODEL_SOUTHWEST &&
                   WEST == CELESTIAL_MODEL_WEST && NORTHWEST == CELESTIAL_MODEL_NORTHWEST,
               "celestial azimuth codes are map directions");

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
