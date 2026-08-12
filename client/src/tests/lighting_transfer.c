/*************************************************************************
 * Atrinik client lighting-transfer regression tests.                   *
 *                                                                       *
 * Copyright 2026 The Atrinik Project                                   *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <lighting.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {                                                    \
            fprintf(stderr, "lighting transfer assertion failed at line %d\n", __LINE__); \
            abort();                                                           \
        }                                                                      \
    } while (0)

static uint8_t legacy_level_from_raw(int raw) {
    static const int anchors[] = {0, 20, 40, 80, 160, 320, 640, 1280};
    static const uint8_t levels[] = {0, 45, 80, 120, 165, 215, 245, 255};

    for (size_t i = 1; i < sizeof(anchors) / sizeof(anchors[0]); i++) {
        if (raw <= anchors[i]) {
            int range = anchors[i] - anchors[i - 1];
            int offset = raw - anchors[i - 1];
            int level_range = levels[i] - levels[i - 1];
            return (uint8_t)(levels[i - 1] + (offset * level_range + range / 2) / range);
        }
    }
    return UINT8_MAX;
}

static void test_lookup_tables(void) {
    TEST_CHECK(lighting_srgb8_to_linear(0) == 0);
    TEST_CHECK(lighting_srgb8_to_linear(1) == 20);
    TEST_CHECK(lighting_srgb8_to_linear(10) == 199);
    TEST_CHECK(lighting_srgb8_to_linear(48) == 1937);
    TEST_CHECK(lighting_srgb8_to_linear(64) == 3360);
    TEST_CHECK(lighting_srgb8_to_linear(96) == 7666);
    TEST_CHECK(lighting_srgb8_to_linear(128) == 14146);
    TEST_CHECK(lighting_srgb8_to_linear(192) == 34544);
    TEST_CHECK(lighting_srgb8_to_linear(208) == 41337);
    TEST_CHECK(lighting_srgb8_to_linear(224) == 48850);
    TEST_CHECK(lighting_srgb8_to_linear(255) == 65535);

    for (unsigned int value = 0; value <= UINT8_MAX; value++) {
        int round_trip = lighting_linear_to_srgb8(lighting_srgb8_to_linear((uint8_t)value));
        TEST_CHECK(abs(round_trip - (int)value) <= 1);
    }
}

static void test_discrete_projection(void) {
    for (int raw = 0; raw <= 40959; raw++) {
        uint16_t radiance = (uint16_t)((raw * 8 + 2) / 5);
        TEST_CHECK(lighting_radiance_to_level(radiance) == legacy_level_from_raw(raw));
    }
}

static void test_common_exposure_vectors(void) {
    static const struct {
        uint16_t scalar;
        uint16_t rgb[3];
        int order[3];
    } vectors[] = {
        {2176, {2176, 2062, 2051}, {0, 1, 2}},
        {2560, {2107, 2371, 2560}, {2, 1, 0}},
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint16_t linear[3];
        lighting_tone_map_linear(vectors[i].scalar, vectors[i].rgb, linear);
        uint8_t display[3] = {
            lighting_linear_to_srgb8(linear[0]),
            lighting_linear_to_srgb8(linear[1]),
            lighting_linear_to_srgb8(linear[2]),
        };
        TEST_CHECK(display[vectors[i].order[0]] > display[vectors[i].order[1]]);
        TEST_CHECK(display[vectors[i].order[1]] > display[vectors[i].order[2]]);
        TEST_CHECK(display[vectors[i].order[0]] == UINT8_MAX);
    }

    const uint16_t neutral[3] = {2048, 2048, 2048};
    uint16_t linear[3];
    lighting_tone_map_linear(2048, neutral, linear);
    TEST_CHECK(linear[0] == UINT16_MAX);
    TEST_CHECK(linear[1] == UINT16_MAX);
    TEST_CHECK(linear[2] == UINT16_MAX);

    const uint16_t zero_rgb[3] = {UINT16_MAX, UINT16_MAX, UINT16_MAX};
    lighting_tone_map_linear(0, zero_rgb, linear);
    TEST_CHECK(linear[0] == 0 && linear[1] == 0 && linear[2] == 0);
}

static void test_gamma_aware_multiplication(void) {
    TEST_CHECK(lighting_multiply_channel(0, UINT16_MAX) == 0);
    TEST_CHECK(lighting_multiply_channel(128, UINT16_MAX) == 128);
    TEST_CHECK(lighting_multiply_channel(255, UINT16_MAX) == 255);
    uint16_t half = lighting_srgb8_to_linear(128);
    uint8_t result = lighting_multiply_channel(128, half);
    TEST_CHECK(result > 55);
    TEST_CHECK(result < 64);
}

static void test_high_precision_bilinear_interpolation(void) {
    TEST_CHECK(lighting_bilinear_channel(0, 65535, 65535, 0, 0, 0, 16) == 0);
    TEST_CHECK(lighting_bilinear_channel(0, 65535, 65535, 0, 16, 16, 16) == 65535);
    TEST_CHECK(lighting_bilinear_channel(0, 65535, 65535, 0, 8, 8, 16) == 32768);

    /* Both halves of a quad recover the same U/V coordinates at the seam. */
    uint16_t first_half = lighting_bilinear_channel(128, 2176, 2560, 1024, 7, 7, 14);
    uint16_t second_half = lighting_bilinear_channel(128, 2176, 2560, 1024, 7, 7, 14);
    TEST_CHECK(first_half == second_half);

    /* The bounded implementation also handles the largest permitted scale. */
    uint64_t scale = UINT64_MAX / (UINT64_C(1) + UINT16_MAX);
    TEST_CHECK(lighting_bilinear_channel(65535, 65535, 65535, 65535, scale, scale, scale) ==
               65535);
}

int main(void) {
    test_lookup_tables();
    test_discrete_projection();
    test_common_exposure_vectors();
    test_gamma_aware_multiplication();
    test_high_precision_bilinear_interpolation();
    return EXIT_SUCCESS;
}
