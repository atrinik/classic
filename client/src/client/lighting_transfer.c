/*************************************************************************
 * Atrinik client scene-linear lighting transfer.                       *
 *                                                                       *
 * Copyright 2026 The Atrinik Project                                   *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include <lighting.h>

#include "lighting_lut.inc"

_Static_assert(sizeof(lighting_srgb8_to_linear_q16_lut) == 256U * sizeof(uint16_t),
               "forward lighting LUT size changed");
_Static_assert(sizeof(lighting_linear_q16_to_srgb8_lut) == 65536U * sizeof(uint8_t),
               "inverse lighting LUT size changed");
_Static_assert(sizeof(lighting_srgb8_to_linear_q16_lut) +
                       sizeof(lighting_linear_q16_to_srgb8_lut) <=
                   66048U,
               "lighting LUT budget exceeded");

uint16_t lighting_srgb8_to_linear(uint8_t value) {
    return lighting_srgb8_to_linear_q16_lut[value];
}

uint8_t lighting_linear_to_srgb8(uint16_t value) {
    return lighting_linear_q16_to_srgb8_lut[value];
}

uint8_t lighting_radiance_to_level(uint16_t radiance) {
    static const uint16_t anchors[] = {0, 20, 40, 80, 160, 320, 640, 1280};
    static const uint8_t levels[] = {0, 45, 80, 120, 165, 215, 245, 255};
    uint32_t raw = ((uint32_t)radiance * 5 + 4) / 8;

    for (size_t i = 1; i < sizeof(anchors) / sizeof(anchors[0]); i++) {
        if (raw <= anchors[i]) {
            uint32_t range = anchors[i] - anchors[i - 1];
            uint32_t offset = raw - anchors[i - 1];
            uint32_t level_range = levels[i] - levels[i - 1];
            return (uint8_t)(levels[i - 1] + (offset * level_range + range / 2) / range);
        }
    }

    return UINT8_MAX;
}

/** Resolve the unquantized neutral display anchor in scene-linear Q0.16. */
static uint16_t lighting_neutral_linear(uint16_t radiance) {
    if (radiance > 2048) {
        return UINT16_MAX;
    }

    uint32_t lower;
    uint32_t low_level;
    uint32_t level_range;
    uint32_t shift;
    if (radiance <= 32) {
        lower = 0;
        low_level = 0;
        level_range = 45;
        shift = 5;
    } else if (radiance <= 64) {
        lower = 32;
        low_level = 45;
        level_range = 35;
        shift = 5;
    } else if (radiance <= 128) {
        lower = 64;
        low_level = 80;
        level_range = 40;
        shift = 6;
    } else if (radiance <= 256) {
        lower = 128;
        low_level = 120;
        level_range = 45;
        shift = 7;
    } else if (radiance <= 512) {
        lower = 256;
        low_level = 165;
        level_range = 50;
        shift = 8;
    } else if (radiance <= 1024) {
        lower = 512;
        low_level = 215;
        level_range = 30;
        shift = 9;
    } else {
        lower = 1024;
        low_level = 245;
        level_range = 10;
        shift = 10;
    }

    uint32_t range = UINT32_C(1) << shift;
    uint32_t numerator = (low_level << shift) + (radiance - lower) * level_range;
    uint32_t code = numerator >> shift;
    uint32_t remainder = numerator & (range - 1);
    if (code >= UINT8_MAX || remainder == 0) {
        return lighting_srgb8_to_linear_q16_lut[code];
    }
    uint32_t low = lighting_srgb8_to_linear_q16_lut[code];
    uint32_t high = lighting_srgb8_to_linear_q16_lut[code + 1];
    return (uint16_t)(low + (remainder * (high - low) + range / 2) / range);
}

void lighting_tone_map_linear(uint16_t scalar,
                              const uint16_t radiance[3],
                              uint16_t linear[3]) {
    if (scalar == 0) {
        linear[0] = linear[1] = linear[2] = 0;
        return;
    }

    uint32_t neutral = lighting_neutral_linear(scalar);
    for (size_t channel = 0; channel < 3; channel++) {
        uint64_t scaled = (uint64_t)radiance[channel] * neutral;
        scaled = (scaled + scalar / 2) / scalar;
        linear[channel] = (uint16_t)(scaled > UINT16_MAX ? UINT16_MAX : scaled);
    }
}

uint8_t lighting_multiply_channel(uint8_t source, uint16_t illumination_linear) {
    uint32_t source_linear = lighting_srgb8_to_linear_q16_lut[source];
    uint32_t product =
        (source_linear * (uint32_t)illumination_linear + UINT16_MAX / 2) / UINT16_MAX;
    return lighting_linear_q16_to_srgb8_lut[product];
}
