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

#ifndef CELESTIAL_LUNAR_H
#define CELESTIAL_LUNAR_H

#include <stdbool.h>
#include <stdint.h>

/** Eight deterministic lunar phase bins, beginning at new moon. */
typedef enum celestial_lunar_phase {
    CELESTIAL_LUNAR_NEW,
    CELESTIAL_LUNAR_WAXING_CRESCENT,
    CELESTIAL_LUNAR_FIRST_QUARTER,
    CELESTIAL_LUNAR_WAXING_GIBBOUS,
    CELESTIAL_LUNAR_FULL,
    CELESTIAL_LUNAR_WANING_GIBBOUS,
    CELESTIAL_LUNAR_LAST_QUARTER,
    CELESTIAL_LUNAR_WANING_CRESCENT,
    CELESTIAL_LUNAR_PHASE_COUNT,
} celestial_lunar_phase;

/** Effective celestial inputs resolved by the regional profile owner. */
typedef struct celestial_lunar_input {
    /** Effective solar phase in the range 0..23. */
    uint16_t solar_hour;
    /** Effective seasonal phase in the range 0..8063. */
    uint16_t season_phase;
    /** Effective lunar phase in the range 0..lunar_period - 1. */
    uint16_t lunar_age;
    /** Effective synodic period: 168..8064, divisible by 24 and 8. */
    uint16_t lunar_period;
    /** Normalized scene-linear Q0.16 moon color. */
    uint16_t moon_color[3];
    /** Normalized scene-linear Q0.16 starlight color. */
    uint16_t starlight_color[3];
    /** Maximum raw moon strength in the range 0..20. */
    uint8_t moon_max;
    /** Raw starlight strength in the range 0..2. */
    uint8_t starlight_strength;
} celestial_lunar_input;

/**
 * Immutable result published to the structural field evaluator.
 *
 * The copied effective inputs make cache identity independent of mutable
 * global clock or profile state. Absolute gameplay time is deliberately not
 * retained: fixed profiles remain cache-stable while it advances. Consumers
 * compare fields, not C object representations. RGB values are raw
 * pre-transport radiance.
 */
typedef struct celestial_lunar_sample {
    celestial_lunar_input revision;
    celestial_lunar_phase phase;
    uint16_t illumination;
    uint8_t moon_hour;
    int32_t elevation;
    uint8_t azimuth;
    bool visible;
    uint8_t moon_strength;
    uint16_t moon_radiance[3];
    uint8_t starlight_strength;
    uint16_t starlight_radiance[3];
} celestial_lunar_sample;

/** Return the stable human-readable name for one lunar phase. */
extern const char *celestial_lunar_phase_name(celestial_lunar_phase phase);

/** Fill the frozen root profile using direct absolute-hour modulo. */
extern void celestial_lunar_root_input(uint64_t absolute_hour, celestial_lunar_input *input);

/**
 * Evaluate one pure deterministic lunar/starlight sample.
 *
 * @return True on success. Invalid input returns false and clears @p sample.
 */
extern bool celestial_lunar_evaluate(const celestial_lunar_input *input,
                                     celestial_lunar_sample *sample);

#endif
