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
 * Deterministic fixed-point celestial model, lunar orbit and transport
 * arithmetic shared by the server and the client.
 */

#include "celestial_model.h"
#include "packet.h"
#include "toolkit.h"

#include <stdlib.h>
#include <string.h>

static const int32_t orbit_elevation[CELESTIAL_MODEL_HOURS_PER_DAY] = {
    -32768, -31651, -28378, -23170, -16384, -8481, 0, 8481,  16384,  23170,  28378,  31651,
    32768,  31651,  28378,  23170,  16384,  8481,  0, -8481, -16384, -23170, -28378, -31651,
};

static const uint8_t orbit_azimuth[CELESTIAL_MODEL_HOURS_PER_DAY] = {
    CELESTIAL_MODEL_NORTH,     CELESTIAL_MODEL_NORTH,     CELESTIAL_MODEL_NORTHEAST,
    CELESTIAL_MODEL_NORTHEAST, CELESTIAL_MODEL_NORTHEAST, CELESTIAL_MODEL_EAST,
    CELESTIAL_MODEL_EAST,      CELESTIAL_MODEL_EAST,      CELESTIAL_MODEL_SOUTHEAST,
    CELESTIAL_MODEL_SOUTHEAST, CELESTIAL_MODEL_SOUTH,     CELESTIAL_MODEL_SOUTH,
    CELESTIAL_MODEL_SOUTH,     CELESTIAL_MODEL_SOUTH,     CELESTIAL_MODEL_SOUTH,
    CELESTIAL_MODEL_SOUTHWEST, CELESTIAL_MODEL_SOUTHWEST, CELESTIAL_MODEL_WEST,
    CELESTIAL_MODEL_WEST,      CELESTIAL_MODEL_WEST,      CELESTIAL_MODEL_NORTHWEST,
    CELESTIAL_MODEL_NORTHWEST, CELESTIAL_MODEL_NORTHWEST, CELESTIAL_MODEL_NORTH,
};

static const uint16_t season_factor[CELESTIAL_MODEL_MONTHS_PER_YEAR] = {
    24576, 25600, 27648, 29952, 31744, 32768, 32768, 31744, 29952, 27648, 25600, 24576,
};

uint64_t celestial_model_round(uint64_t numerator, uint64_t denominator) {
    HARD_ASSERT(denominator != 0);
    /* The ceiling of half the denominator, without overflowing at its top. */
    uint64_t half = denominator / 2 + denominator % 2;
    return numerator / denominator + (numerator % denominator >= half ? 1 : 0);
}

int32_t celestial_model_scale(int32_t value, uint32_t scale, uint32_t denominator) {
    HARD_ASSERT(denominator != 0);
    if (value <= 0 || scale == 0) {
        return 0;
    }
    /* At most 2^31 * 2^32: the product fits, and only the result can exceed
     * the range, which saturates rather than narrow. */
    uint64_t scaled = celestial_model_round((uint64_t)value * scale, denominator);
    return scaled > INT32_MAX ? INT32_MAX : (int32_t)scaled;
}

int32_t celestial_model_transmit(int32_t value, uint16_t coefficient) {
    return (int32_t)celestial_model_round((uint64_t)MAX(value, 0) * coefficient,
                                          CELESTIAL_MODEL_SCALE);
}

static bool normalized_color_valid(const uint16_t color[3], uint8_t strength) {
    uint16_t peak = MAX(color[0], MAX(color[1], color[2]));
    return peak == UINT16_MAX || (peak == 0 && strength == 0);
}

static int32_t seasonal_solar_elevation(uint16_t solar_hour, uint16_t season_phase) {
    int32_t elevation = orbit_elevation[solar_hour];
    uint16_t factor = season_factor[season_phase / CELESTIAL_MODEL_HOURS_PER_MONTH];
    uint64_t magnitude = celestial_model_round((uint64_t)abs(elevation) * factor, 32768);
    return elevation < 0 ? -(int32_t)magnitude : (int32_t)magnitude;
}

static int32_t twilight_elevation(uint16_t season_phase) {
    uint16_t factor = season_factor[season_phase / CELESTIAL_MODEL_HOURS_PER_MONTH];
    return -(int32_t)celestial_model_round(UINT64_C(8481) * factor, 32768);
}

static void project_color(uint8_t strength, const uint16_t color[3], uint16_t output[3]) {
    for (size_t channel = 0; channel < 3; channel++) {
        output[channel] =
            (uint16_t)celestial_model_round((uint64_t)strength * color[channel], UINT16_MAX);
    }
}

const char *celestial_lunar_phase_name(celestial_lunar_phase phase) {
    switch (phase) {
        case CELESTIAL_LUNAR_NEW:
            return "new moon";
        case CELESTIAL_LUNAR_WAXING_CRESCENT:
            return "waxing crescent";
        case CELESTIAL_LUNAR_FIRST_QUARTER:
            return "first quarter";
        case CELESTIAL_LUNAR_WAXING_GIBBOUS:
            return "waxing gibbous";
        case CELESTIAL_LUNAR_FULL:
            return "full moon";
        case CELESTIAL_LUNAR_WANING_GIBBOUS:
            return "waning gibbous";
        case CELESTIAL_LUNAR_LAST_QUARTER:
            return "last quarter";
        case CELESTIAL_LUNAR_WANING_CRESCENT:
            return "waning crescent";
        default:
            return "unknown";
    }
}

static bool lunar_input_valid(const celestial_lunar_input *input) {
    return input->solar_hour < CELESTIAL_MODEL_HOURS_PER_DAY &&
           input->season_phase < CELESTIAL_MODEL_HOURS_PER_YEAR &&
           input->lunar_period >= CELESTIAL_MODEL_LUNAR_PERIOD_MIN &&
           input->lunar_period <= CELESTIAL_MODEL_LUNAR_PERIOD_MAX &&
           input->lunar_period % CELESTIAL_MODEL_HOURS_PER_DAY == 0 &&
           input->lunar_period % 8 == 0 && input->lunar_age < input->lunar_period &&
           input->moon_max <= CELESTIAL_MODEL_MOON_MAX &&
           input->starlight_strength <= CELESTIAL_MODEL_STARLIGHT_MAX &&
           normalized_color_valid(input->moon_color, input->moon_max) &&
           normalized_color_valid(input->starlight_color, input->starlight_strength);
}

bool celestial_lunar_evaluate(const celestial_lunar_input *input, celestial_lunar_sample *sample) {
    if (sample == NULL) {
        return false;
    }
    memset(sample, 0, sizeof(*sample));
    if (input == NULL || !lunar_input_valid(input)) {
        return false;
    }

    sample->revision.solar_hour = input->solar_hour;
    sample->revision.season_phase = input->season_phase;
    sample->revision.lunar_age = input->lunar_age;
    sample->revision.lunar_period = input->lunar_period;
    memcpy(sample->revision.moon_color, input->moon_color, sizeof(sample->revision.moon_color));
    memcpy(sample->revision.starlight_color,
           input->starlight_color,
           sizeof(sample->revision.starlight_color));
    sample->revision.moon_max = input->moon_max;
    sample->revision.starlight_strength = input->starlight_strength;
    sample->phase = (celestial_lunar_phase)(((uint64_t)input->lunar_age * 8) / input->lunar_period);

    uint16_t half_period = input->lunar_period / 2;
    uint16_t lit_age =
        input->lunar_age <= half_period ? input->lunar_age : input->lunar_period - input->lunar_age;
    sample->illumination =
        (uint16_t)celestial_model_round((uint64_t)lit_age * UINT16_MAX, half_period);

    uint8_t elongation =
        (uint8_t)(((uint64_t)input->lunar_age * CELESTIAL_MODEL_HOURS_PER_DAY) / input->lunar_period);
    uint8_t transit = (12 + elongation) % CELESTIAL_MODEL_HOURS_PER_DAY;
    sample->moon_hour = (input->solar_hour + 36 - transit) % CELESTIAL_MODEL_HOURS_PER_DAY;
    sample->elevation = orbit_elevation[sample->moon_hour];
    sample->azimuth = orbit_azimuth[sample->moon_hour];
    sample->visible = sample->elevation > 0;

    if (sample->illumination != 0 && sample->visible && input->moon_max != 0) {
        uint64_t numerator =
            (uint64_t)input->moon_max * sample->illumination * (uint32_t)sample->elevation;
        sample->moon_strength =
            (uint8_t)celestial_model_round(numerator, UINT64_C(65535) * UINT64_C(32768));
        project_color(sample->moon_strength, input->moon_color, sample->moon_radiance);
    }

    if (seasonal_solar_elevation(input->solar_hour, input->season_phase) <
        twilight_elevation(input->season_phase)) {
        sample->starlight_strength = input->starlight_strength;
        project_color(sample->starlight_strength,
                      input->starlight_color,
                      sample->starlight_radiance);
    }
    return true;
}

static void model_lunar_input(const celestial_model_inputs_t *inputs, celestial_lunar_input *lunar) {
    memset(lunar, 0, sizeof(*lunar));
    lunar->solar_hour = inputs->solar_hour;
    lunar->season_phase = inputs->season_phase;
    lunar->lunar_age = inputs->lunar_age;
    lunar->lunar_period = inputs->lunar_period;
    memcpy(lunar->moon_color, inputs->moon_linear, sizeof(lunar->moon_color));
    memcpy(lunar->starlight_color, inputs->starlight_linear, sizeof(lunar->starlight_color));
    lunar->moon_max = inputs->moon_max;
    lunar->starlight_strength = inputs->starlight_strength;
}

bool celestial_model_inputs_valid(const celestial_model_inputs_t *inputs) {
    if (inputs == NULL) {
        return false;
    }
    if (inputs->day_brightness > CELESTIAL_MODEL_BRIGHTNESS_MAX ||
        inputs->night_brightness > CELESTIAL_MODEL_BRIGHTNESS_MAX) {
        return false;
    }
    /* Day and night colors are black or normalized to a 65535 peak. */
    for (size_t i = 0; i < 2; i++) {
        const uint16_t *color = i == 0 ? inputs->day_linear : inputs->night_linear;
        uint16_t peak = MAX(color[0], MAX(color[1], color[2]));
        if (peak != 0 && peak != UINT16_MAX) {
            return false;
        }
    }
    celestial_lunar_input lunar;
    model_lunar_input(inputs, &lunar);
    return lunar_input_valid(&lunar);
}

void celestial_model_evaluate(const celestial_model_inputs_t *inputs, celestial_model_t *model) {
    HARD_ASSERT(inputs != NULL);
    HARD_ASSERT(model != NULL);
    memset(model, 0, sizeof(*model));
    HARD_ASSERT(inputs->solar_hour < CELESTIAL_MODEL_HOURS_PER_DAY);
    HARD_ASSERT(inputs->season_phase < CELESTIAL_MODEL_HOURS_PER_YEAR);
    model->solar_direction = orbit_azimuth[inputs->solar_hour];

    int32_t elevation = seasonal_solar_elevation(inputs->solar_hour, inputs->season_phase);
    model->solar_elevation = elevation;
    int32_t twilight = twilight_elevation(inputs->season_phase);
    if (elevation > 0) {
        model->direct = celestial_model_scale(elevation, 960, 32768);
        model->diffuse = 64 + celestial_model_scale(elevation, 256, 32768);
    } else if (elevation == 0) {
        model->diffuse = 64;
    } else if (elevation >= twilight) {
        model->diffuse = 16;
    }

    uint32_t daylight = (uint32_t)MAX(elevation, 0);
    uint32_t inverse = 32768 - MIN(daylight, 32768U);
    uint64_t brightness_numerator = (uint64_t)inputs->night_brightness * inverse +
                                    (uint64_t)inputs->day_brightness * MIN(daylight, 32768U);
    model->solar_brightness = (uint16_t)celestial_model_round(brightness_numerator, 32768);
    for (size_t channel = 0; channel < 3; channel++) {
        uint64_t color_numerator = (uint64_t)inputs->night_linear[channel] * inverse +
                                   (uint64_t)inputs->day_linear[channel] * MIN(daylight, 32768U);
        model->solar_color[channel] = (uint16_t)celestial_model_round(color_numerator, 32768);
    }

    celestial_lunar_input lunar_input;
    celestial_lunar_sample lunar;
    model_lunar_input(inputs, &lunar_input);
    if (!celestial_lunar_evaluate(&lunar_input, &lunar)) {
        return;
    }
    model->moon_direction = lunar.azimuth;
    model->moon_elevation = lunar.elevation;
    model->moon = lunar.moon_strength;
    model->starlight = lunar.starlight_strength;
    memcpy(model->moon_color, inputs->moon_linear, sizeof(model->moon_color));
    memcpy(model->starlight_color, inputs->starlight_linear, sizeof(model->starlight_color));
}

int32_t celestial_model_ray_value(const uint16_t *coefficients,
                                  const bool *apertures,
                                  unsigned count,
                                  bool sky,
                                  int32_t source,
                                  int32_t *aperture) {
    int32_t value = sky ? source : 0;
    int32_t spill = 0;
    for (unsigned i = count; i-- > 0;) {
        value = celestial_model_transmit(value, coefficients[i]);
        spill = celestial_model_transmit(spill, coefficients[i]);
        if (apertures[i]) {
            spill = value;
        }
    }
    if (aperture != NULL) {
        *aperture = spill;
    }
    return value;
}

void celestial_model_relax_row(const int32_t *values,
                               int32_t *next,
                               int width,
                               int height,
                               int y,
                               celestial_model_spill_fn spill,
                               void *context) {
    for (int x = 0; x < width; x++) {
        int32_t best = values[x + width * y];
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if ((dx == 0 && dy == 0) || x + dx < 0 || y + dy < 0 || x + dx >= width ||
                    y + dy >= height) {
                    continue;
                }
                uint16_t coefficient = spill(x, y, dx, dy, context);
                int32_t candidate =
                    celestial_model_transmit(values[x + dx + width * (y + dy)], coefficient);
                candidate = celestial_model_transmit(candidate, dx != 0 && dy != 0 ? 181 : 192);
                best = MAX(best, candidate);
            }
        }
        next[x + width * y] = best;
    }
}

bool celestial_model_relax(int32_t *values,
                           int width,
                           int height,
                           celestial_model_spill_fn spill,
                           void *context) {
    /* Invalid dimensions are rejected before the caller's grid is read. */
    if (width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / sizeof(int32_t) / (size_t)height) {
        return false;
    }
    size_t count = (size_t)width * height;
    bool active = false;
    for (size_t i = 0; i < count; i++) {
        active |= values[i] != 0;
    }
    if (!active) {
        return true;
    }
    int32_t *next = calloc(count, sizeof(*next));
    if (next == NULL) {
        return false;
    }
    int32_t *original = values;
    for (int pass = 0; pass < CELESTIAL_MODEL_SPILL_PASSES; pass++) {
        for (int y = 0; y < height; y++) {
            celestial_model_relax_row(values, next, width, height, y, spill, context);
        }
        int32_t *swap = values;
        values = next;
        next = swap;
    }
    if (values != original) {
        memcpy(original, values, count * sizeof(*original));
        free(values);
    } else {
        free(next);
    }
    return true;
}

void celestial_model_publish(const celestial_model_t *model,
                             int32_t direct,
                             int32_t diffuse,
                             int32_t aperture_direct,
                             int32_t moon,
                             int32_t moon_aperture,
                             int32_t starlight,
                             int32_t *value,
                             int32_t rgb[3]) {
    int32_t solar =
        celestial_model_scale(direct + diffuse + aperture_direct, model->solar_brightness, 256);
    int32_t moon_value = moon + moon_aperture;
    *value = solar + moon_value + starlight;
    for (size_t channel = 0; channel < 3; channel++) {
        rgb[channel] = celestial_model_scale(solar, model->solar_color[channel], UINT16_MAX);
        rgb[channel] += celestial_model_scale(moon_value, model->moon_color[channel], UINT16_MAX);
        rgb[channel] +=
            celestial_model_scale(starlight, model->starlight_color[channel], UINT16_MAX);
    }
}

/** Two slot inputs share every profile constant and differ only in phases. */
static bool celestial_protocol_same_profile(const celestial_model_inputs_t *a,
                                            const celestial_model_inputs_t *b) {
    /* Field by field, so padding never decides it. */
    for (size_t channel = 0; channel < 3; channel++) {
        if (a->day_linear[channel] != b->day_linear[channel] ||
            a->night_linear[channel] != b->night_linear[channel] ||
            a->moon_linear[channel] != b->moon_linear[channel] ||
            a->starlight_linear[channel] != b->starlight_linear[channel]) {
            return false;
        }
    }
    return a->day_brightness == b->day_brightness &&
           a->night_brightness == b->night_brightness && a->moon_max == b->moon_max &&
           a->starlight_strength == b->starlight_strength && a->lunar_period == b->lunar_period;
}

static void celestial_protocol_write_rgb(packet_struct *packet, const uint16_t rgb[3]) {
    for (size_t channel = 0; channel < 3; channel++) {
        packet_writer_write_uint16(packet, rgb[channel]);
    }
}

static void celestial_protocol_write_phases(packet_struct *packet,
                                            const celestial_model_inputs_t *inputs) {
    packet_writer_write_uint8(packet, (uint8_t)inputs->solar_hour);
    packet_writer_write_uint16(packet, inputs->season_phase);
    packet_writer_write_uint16(packet, inputs->lunar_age);
}

bool celestial_protocol_write(packet_struct *packet,
                              uint32_t generation,
                              uint64_t start_seconds,
                              const celestial_protocol_slot_t *slots,
                              size_t count) {
    /* The interval starts on an hour boundary, which the client keeps as
     * an hour, and its following hour must be representable too; a packet
     * already in error is refused unwritten. */
    if (packet == NULL || count > CELESTIAL_PROTOCOL_SLOTS_MAX || (count != 0 && slots == NULL) ||
        start_seconds % 3600 != 0 || start_seconds > UINT64_MAX - 3600 ||
        packet_writer_error(packet) != PACKET_ERROR_NONE) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (slots[i].slot >= CELESTIAL_PROTOCOL_SLOT_NONE ||
            (i != 0 && slots[i].slot <= slots[i - 1].slot) ||
            !celestial_model_inputs_valid(&slots[i].current) ||
            !celestial_model_inputs_valid(&slots[i].next) ||
            !celestial_protocol_same_profile(&slots[i].current, &slots[i].next)) {
            return false;
        }
    }
    packet_writer_mark_t mark;
    packet_writer_mark(packet, &mark);
    packet_writer_write_uint32(packet, generation);
    packet_writer_write_uint64(packet, start_seconds);
    packet_writer_write_uint8(packet, (uint8_t)count);
    for (size_t i = 0; i < count; i++) {
        const celestial_model_inputs_t *profile = &slots[i].current;
        packet_writer_write_uint8(packet, slots[i].slot);
        celestial_protocol_write_rgb(packet, profile->day_linear);
        celestial_protocol_write_rgb(packet, profile->night_linear);
        packet_writer_write_uint16(packet, profile->day_brightness);
        packet_writer_write_uint16(packet, profile->night_brightness);
        celestial_protocol_write_rgb(packet, profile->moon_linear);
        packet_writer_write_uint8(packet, profile->moon_max);
        celestial_protocol_write_rgb(packet, profile->starlight_linear);
        packet_writer_write_uint8(packet, profile->starlight_strength);
        packet_writer_write_uint16(packet, profile->lunar_period);
        celestial_protocol_write_phases(packet, &slots[i].current);
        celestial_protocol_write_phases(packet, &slots[i].next);
    }
    if (packet_writer_error(packet) != PACKET_ERROR_NONE) {
        /* A partial payload is never left behind. */
        packet_writer_rollback(packet, &mark);
        return false;
    }
    return true;
}

static void celestial_protocol_read_rgb(packet_reader_t *reader, uint16_t rgb[3]) {
    for (size_t channel = 0; channel < 3; channel++) {
        rgb[channel] = packet_reader_read_uint16(reader);
    }
}

static void celestial_protocol_read_phases(packet_reader_t *reader,
                                           celestial_model_inputs_t *inputs) {
    inputs->solar_hour = packet_reader_read_uint8(reader);
    inputs->season_phase = packet_reader_read_uint16(reader);
    inputs->lunar_age = packet_reader_read_uint16(reader);
}

bool celestial_protocol_read(const uint8_t *data,
                             size_t len,
                             uint32_t *generation,
                             uint64_t *start_seconds,
                             celestial_protocol_slot_t *slots,
                             size_t *count) {
    if (data == NULL || generation == NULL || start_seconds == NULL || slots == NULL ||
        count == NULL) {
        return false;
    }
    packet_reader_t reader;
    packet_reader_init(&reader, data, len);
    uint32_t parsed_generation = packet_reader_read_uint32(&reader);
    uint64_t start = packet_reader_read_uint64(&reader);
    uint8_t parsed = packet_reader_read_uint8(&reader);
    if (packet_reader_error(&reader) != PACKET_ERROR_NONE || parsed > CELESTIAL_PROTOCOL_SLOTS_MAX ||
        start % 3600 != 0 || start > UINT64_MAX - 3600) {
        return false;
    }
    celestial_protocol_slot_t staged[CELESTIAL_PROTOCOL_SLOTS_MAX];
    memset(staged, 0, sizeof(staged));
    for (size_t i = 0; i < parsed; i++) {
        celestial_protocol_slot_t *slot = &staged[i];
        celestial_model_inputs_t *profile = &slot->current;
        slot->slot = packet_reader_read_uint8(&reader);
        celestial_protocol_read_rgb(&reader, profile->day_linear);
        celestial_protocol_read_rgb(&reader, profile->night_linear);
        profile->day_brightness = packet_reader_read_uint16(&reader);
        profile->night_brightness = packet_reader_read_uint16(&reader);
        celestial_protocol_read_rgb(&reader, profile->moon_linear);
        profile->moon_max = packet_reader_read_uint8(&reader);
        celestial_protocol_read_rgb(&reader, profile->starlight_linear);
        profile->starlight_strength = packet_reader_read_uint8(&reader);
        profile->lunar_period = packet_reader_read_uint16(&reader);
        slot->next = *profile;
        celestial_protocol_read_phases(&reader, &slot->current);
        celestial_protocol_read_phases(&reader, &slot->next);
        if (packet_reader_error(&reader) != PACKET_ERROR_NONE ||
            slot->slot >= CELESTIAL_PROTOCOL_SLOT_NONE ||
            (i != 0 && slot->slot <= staged[i - 1].slot) ||
            !celestial_model_inputs_valid(&slot->current) ||
            !celestial_model_inputs_valid(&slot->next)) {
            return false;
        }
    }
    if (!packet_reader_finish(&reader)) {
        return false;
    }
    *generation = parsed_generation;
    *start_seconds = start;
    memcpy(slots, staged, parsed * sizeof(*slots));
    *count = parsed;
    return true;
}
