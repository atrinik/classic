/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Celestial light model shared by the server's authoritative celestial solver
 * and the client's protocol-1085 celestial evaluation, so both derive the sun,
 * moon, sky and starlight sources and their fixed-point transport arithmetic
 * from identical code.
 *
 * Every function is pure: it reads only its arguments and immutable built-in
 * tables, allocates nothing except where documented, retains no state and is
 * safe to call from any thread.
 */

#ifndef TOOLKIT_CELESTIAL_MODEL_H
#define TOOLKIT_CELESTIAL_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Solar phases per day. */
#define CELESTIAL_MODEL_HOURS_PER_DAY 24
/** Hours of the root lunar month. */
#define CELESTIAL_MODEL_HOURS_PER_MONTH 672
/** Months per seasonal year. */
#define CELESTIAL_MODEL_MONTHS_PER_YEAR 12
/** Seasonal phases per year. */
#define CELESTIAL_MODEL_HOURS_PER_YEAR (CELESTIAL_MODEL_HOURS_PER_MONTH * CELESTIAL_MODEL_MONTHS_PER_YEAR)
/** Shortest valid effective lunar period. */
#define CELESTIAL_MODEL_LUNAR_PERIOD_MIN 168
/** Longest valid effective lunar period. */
#define CELESTIAL_MODEL_LUNAR_PERIOD_MAX CELESTIAL_MODEL_HOURS_PER_YEAR
/** Largest raw moon strength. */
#define CELESTIAL_MODEL_MOON_MAX 20
/** Largest day or night brightness, Q8.8, as region profiles bound it. */
#define CELESTIAL_MODEL_BRIGHTNESS_MAX 1024
/** Largest raw starlight strength. */
#define CELESTIAL_MODEL_STARLIGHT_MAX 2
/** Fixed-point transmission scale: 256 is open, 0 opaque. */
#define CELESTIAL_MODEL_SCALE 256
/** Horizontal cells a directional ray may cross. */
#define CELESTIAL_MODEL_DIRECT_REACH 32
/** Passes of the diffuse spill relaxation. */
#define CELESTIAL_MODEL_SPILL_PASSES 4

/**
 * @defgroup CELESTIAL_MODEL_DIRECTION Celestial azimuth codes
 * Equal to the server's map direction codes.
 *@{*/
#define CELESTIAL_MODEL_NORTH 1
#define CELESTIAL_MODEL_NORTHEAST 2
#define CELESTIAL_MODEL_EAST 3
#define CELESTIAL_MODEL_SOUTHEAST 4
#define CELESTIAL_MODEL_SOUTH 5
#define CELESTIAL_MODEL_SOUTHWEST 6
#define CELESTIAL_MODEL_WEST 7
#define CELESTIAL_MODEL_NORTHWEST 8
/*@}*/

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

/** Human-readable name of a lunar phase. */
const char *celestial_lunar_phase_name(celestial_lunar_phase phase);

/**
 * Evaluate the deterministic lunar orbit and starlight.
 * @return False, with a zeroed sample, for an input outside its documented
 * ranges.
 */
bool celestial_lunar_evaluate(const celestial_lunar_input *input, celestial_lunar_sample *sample);

/**
 * Everything the celestial model consumes for one region profile and one
 * gameplay hour: the profile's light constants and the effective phases after
 * the server has applied the cycle mappings and any lunar override. Epochs,
 * rates, override controls and the profile's identity are never part of it.
 */
typedef struct celestial_model_inputs {
    uint16_t day_linear[3];
    uint16_t night_linear[3];
    uint16_t day_brightness;
    uint16_t night_brightness;
    uint16_t moon_linear[3];
    uint8_t moon_max;
    uint16_t starlight_linear[3];
    uint8_t starlight_strength;
    uint16_t lunar_period;
    /** Effective solar phase, 0..23. */
    uint16_t solar_hour;
    /** Effective seasonal phase, 0..8063. */
    uint16_t season_phase;
    /** Effective lunar age, 0..lunar_period - 1. */
    uint16_t lunar_age;
} celestial_model_inputs_t;

/** Celestial sources of one hour, before transport. */
typedef struct celestial_model {
    int32_t direct;
    int32_t diffuse;
    int32_t moon;
    int32_t starlight;
    uint16_t solar_color[3];
    uint16_t moon_color[3];
    uint16_t starlight_color[3];
    uint16_t solar_brightness;
    int32_t solar_elevation;
    int32_t moon_elevation;
    uint8_t solar_direction;
    uint8_t moon_direction;
} celestial_model_t;

/**
 * Whether model inputs are within the ranges a server produces: phases inside
 * their periods, a valid lunar period, bounded strengths and normalized
 * colors. Decoders reject inputs that fail this.
 */
bool celestial_model_inputs_valid(const celestial_model_inputs_t *inputs);

/** Derive the sources of one hour from valid inputs. */
void celestial_model_evaluate(const celestial_model_inputs_t *inputs, celestial_model_t *model);

/** Divide, rounding half up. The denominator must be nonzero. */
uint64_t celestial_model_round(uint64_t numerator, uint64_t denominator);

/** Scale a nonnegative value by scale / denominator; nonpositive values give 0. */
int32_t celestial_model_scale(int32_t value, uint32_t scale, uint32_t denominator);

/** Transmit a value through one crossing coefficient (0..256). */
int32_t celestial_model_transmit(int32_t value, uint16_t coefficient);

/**
 * Value of a traced ray: the source if the ray reached the sky, transmitted
 * through each crossing from the source side to the receiver, rounding at
 * every crossing.
 * @param aperture If not NULL, receives the light that last passed an
 * aperture crossing, likewise transmitted to the receiver.
 */
int32_t celestial_model_ray_value(const uint16_t *coefficients,
                                  const bool *apertures,
                                  unsigned count,
                                  bool sky,
                                  int32_t source,
                                  int32_t *aperture);

/** Coefficient of spilling light from (x + dx, y + dy) into (x, y). */
typedef uint16_t (*celestial_model_spill_fn)(int x, int y, int dx, int dy, void *context);

/**
 * Relax diffuse light across a width-by-height grid for
 * CELESTIAL_MODEL_SPILL_PASSES passes: each cell keeps the maximum of its own
 * value and its eight neighbors' values transmitted through the spill
 * coefficient and the cardinal (192) or diagonal (181) falloff.
 * @return False for a non-positive or oversized width or height, values
 * untouched, or if scratch memory could not be allocated; values are then
 * unchanged.
 */
bool celestial_model_relax(int32_t *values,
                           int width,
                           int height,
                           celestial_model_spill_fn spill,
                           void *context);

/**
 * One row y of one celestial_model_relax() pass: writes row y of next from
 * values, so a caller can run a pass row by row and swap the grids after.
 * Unchecked preconditions: width and height are positive dimensions that
 * celestial_model_relax() accepts, 0 <= y < height, values and next are
 * non-NULL, non-overlapping width × height grids, and spill is non-NULL.
 */
void celestial_model_relax_row(const int32_t *values,
                               int32_t *next,
                               int width,
                               int height,
                               int y,
                               celestial_model_spill_fn spill,
                               void *context);

/** Profile slots one CELESTIAL command can carry; slot ids are 0..254. */
#define CELESTIAL_PROTOCOL_SLOTS_MAX 255
/** Slot id of a fragment whose map has no celestial profile. */
#define CELESTIAL_PROTOCOL_SLOT_NONE 255

/** One CELESTIAL profile slot: the current and next hour's model inputs,
 * which share every profile constant and differ only in their phases. */
typedef struct celestial_protocol_slot {
    uint8_t slot;
    celestial_model_inputs_t current;
    celestial_model_inputs_t next;
} celestial_protocol_slot_t;

struct packet_struct;

/**
 * Append a complete CELESTIAL payload: its publication generation, the start
 * of the current hour in game seconds, then the slots in strictly increasing
 * slot order.
 * @return False, writing nothing, for invalid slots or more than
 * CELESTIAL_PROTOCOL_SLOTS_MAX of them, a start not on an hour boundary, a
 * packet already carrying a writer error, or a write that fails part way
 * (its limit or an allocation), which is rolled back.
 */
bool celestial_protocol_write(struct packet_struct *packet,
                              uint32_t generation,
                              uint64_t start_seconds,
                              const celestial_protocol_slot_t *slots,
                              size_t count);

/**
 * Parse and validate a complete CELESTIAL payload. Rejects truncation,
 * trailing bytes, a start not on an hour boundary, unordered or
 * out-of-range slots, more than CELESTIAL_PROTOCOL_SLOTS_MAX slots and
 * invalid model inputs.
 * @param slots Receives up to CELESTIAL_PROTOCOL_SLOTS_MAX slots; unchanged
 * on failure.
 */
bool celestial_protocol_read(const uint8_t *data,
                             size_t len,
                             uint32_t *generation,
                             uint64_t *start_seconds,
                             celestial_protocol_slot_t *slots,
                             size_t *count);

/** Final scalar and RGB of one cell from its transported components. */
void celestial_model_publish(const celestial_model_t *model,
                             int32_t direct,
                             int32_t diffuse,
                             int32_t aperture_direct,
                             int32_t moon,
                             int32_t moon_aperture,
                             int32_t starlight,
                             int32_t *value,
                             int32_t rgb[3]);

#endif
