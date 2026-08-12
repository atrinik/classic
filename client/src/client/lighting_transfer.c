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
    static const uint16_t anchors[] = {0, 32, 64, 128, 256, 512, 1024, 2048};
    static const uint8_t levels[] = {0, 45, 80, 120, 165, 215, 245, 255};

    for (size_t i = 1; i < sizeof(anchors) / sizeof(anchors[0]); i++) {
        if (radiance <= anchors[i]) {
            uint32_t range = anchors[i] - anchors[i - 1];
            uint32_t offset = radiance - anchors[i - 1];
            uint32_t code_numerator = (uint32_t)levels[i - 1] * range +
                                      offset * (uint32_t)(levels[i] - levels[i - 1]);
            uint32_t code = code_numerator / range;
            uint32_t remainder = code_numerator % range;
            if (code >= UINT8_MAX || remainder == 0) {
                return lighting_srgb8_to_linear_q16_lut[code];
            }
            uint32_t low = lighting_srgb8_to_linear_q16_lut[code];
            uint32_t high = lighting_srgb8_to_linear_q16_lut[code + 1];
            return (uint16_t)(low + (remainder * (high - low) + range / 2) / range);
        }
    }

    return UINT16_MAX;
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
