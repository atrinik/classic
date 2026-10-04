/*************************************************************************
 * Atrinik client visibility field and fade regression tests.
 *
 * Copyright 2026 The Atrinik Project
 *************************************************************************/

#include <map_visibility.h>
#include <lighting.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <toolkit/toolkit.h>

#define CHECK(expression)                                                            \
    do {                                                                             \
        if (!(expression)) {                                                         \
            fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expression); \
            return EXIT_FAILURE;                                                     \
        }                                                                            \
    } while (0)

/* A known floor occupies its closed unit square.  At a coverage-grid point,
 * every incident floor must be known; this geometric model deliberately does
 * not reproduce the helper's output indexing or boolean expressions. */
static uint8_t expected_ground_coverage(const bool known[4], int u, int v) {
    bool opaque = true;
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            if (u >= x && u <= x + 1 && v >= y && v <= y + 1) {
                size_t corner = y == 0 ? (size_t)x : (x == 0 ? 3U : 2U);
                opaque = opaque && known[corner];
            }
        }
    }
    return opaque ? UINT8_MAX : 0;
}

static int check_ground_coverage_mask(unsigned int mask) {
    bool known[4];
    uint8_t coverage[9];
    for (size_t corner = 0; corner < arraysize(known); corner++) {
        known[corner] = (mask & (1U << corner)) != 0;
    }
    map_visibility_ground_coverage(known, coverage);
    for (int v = 0; v < 3; v++) {
        for (int u = 0; u < 3; u++) {
            CHECK(coverage[v * 3 + u] == expected_ground_coverage(known, u, v));
        }
    }
    return EXIT_SUCCESS;
}

static int check_ground_coverage_shared_edges(void) {
    /* Enumerate every 3-by-2 floor layout.  The right edge of the left quad
     * and the left edge of the right quad must describe the same geometry. */
    for (unsigned int layout = 0; layout < 64; layout++) {
        bool left_known[4];
        bool right_known[4];
        uint8_t left[9];
        uint8_t right[9];
        left_known[0] = (layout & (1U << 0)) != 0;
        left_known[1] = (layout & (1U << 1)) != 0;
        left_known[2] = (layout & (1U << 4)) != 0;
        left_known[3] = (layout & (1U << 3)) != 0;
        right_known[0] = (layout & (1U << 1)) != 0;
        right_known[1] = (layout & (1U << 2)) != 0;
        right_known[2] = (layout & (1U << 5)) != 0;
        right_known[3] = (layout & (1U << 4)) != 0;
        map_visibility_ground_coverage(left_known, left);
        map_visibility_ground_coverage(right_known, right);
        CHECK(left[2] == right[0]);
        CHECK(left[5] == right[3]);
        CHECK(left[8] == right[6]);
    }
    return EXIT_SUCCESS;
}

static int check_ground_coverage_shapes(void) {
    uint8_t coverage[9];
    const bool isolated[4] = {true, false, false, false};
    map_visibility_ground_coverage(isolated, coverage);
    CHECK(coverage[0] == UINT8_MAX);
    for (size_t index = 1; index < arraysize(coverage); index++) {
        CHECK(coverage[index] == 0);
    }

    const bool corridor[4] = {true, true, false, false};
    map_visibility_ground_coverage(corridor, coverage);
    CHECK(coverage[0] == UINT8_MAX && coverage[1] == UINT8_MAX && coverage[2] == UINT8_MAX);
    for (size_t index = 3; index < arraysize(coverage); index++) {
        CHECK(coverage[index] == 0);
    }

    const bool concave_notch[4] = {true, true, false, true};
    map_visibility_ground_coverage(concave_notch, coverage);
    CHECK(coverage[0] == UINT8_MAX && coverage[1] == UINT8_MAX && coverage[2] == UINT8_MAX);
    CHECK(coverage[3] == UINT8_MAX && coverage[4] == 0 && coverage[5] == 0);
    CHECK(coverage[6] == UINT8_MAX && coverage[7] == 0 && coverage[8] == 0);
    return EXIT_SUCCESS;
}

int main(void) {
    for (unsigned int mask = 0; mask < 16; mask++) {
        if (check_ground_coverage_mask(mask) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
    }
    if (check_ground_coverage_shared_edges() != EXIT_SUCCESS ||
        check_ground_coverage_shapes() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const uint16_t expected[] = {256, 256, 256, 256, 256, 251, 208, 171, 149, 85, 80, 0};
    const uint32_t distances_squared[] = {0, 1, 4, 8, 16, 17, 25, 32, 36, 48, 49, 64};
    for (size_t i = 0; i < arraysize(distances_squared); i++) {
        CHECK(map_visibility_field_weight_squared(distances_squared[i]) == expected[i]);
    }

    CHECK(map_visibility_field_weight(2, 2) == 256);
    CHECK(map_visibility_field_weight(3, 4) == 208);
    CHECK(map_visibility_add_player_radiance(2048, 256) == 2176);
    CHECK(map_visibility_add_player_radiance(0, 208) == 104);
    CHECK(map_visibility_add_player_radiance(UINT16_MAX, 256) == UINT16_MAX);

    /* A zero-ambient player field must remain visibly dark after the actual
     * scene-linear tone map, not merely have a small raw/daylight fraction. */
    uint16_t night = map_visibility_add_player_radiance(0, 256);
    uint16_t night_rgb[3] = {night, night, night};
    uint16_t illumination[3];
    lighting_tone_map_linear(night, night_rgb, illumination);
    CHECK(lighting_multiply_channel(255, illumination[0]) == 120);
    CHECK(illumination[0] < UINT16_MAX / 5);
    uint16_t memory_rgb[3] = {64, 64, 64};
    lighting_tone_map_linear(64, memory_rgb, illumination);
    CHECK(lighting_multiply_channel(255, illumination[0]) == 80);
    CHECK(illumination[0] < UINT16_MAX / 10);
    uint16_t day_rgb[3] = {2176, 2176, 2176};
    lighting_tone_map_linear(2176, day_rgb, illumination);
    CHECK(lighting_multiply_channel(255, illumination[0]) == 255);

    for (int distance = -1; distance <= 3; distance++) {
        uint16_t weight = distance <= 0 ? 0 : distance == 1 ? 16 : 256;
        CHECK(map_visibility_window_weight(distance, 8, 17, 17) == weight);
        CHECK(map_visibility_window_weight(16 - distance, 8, 17, 17) == weight);
        CHECK(map_visibility_window_weight(8, distance, 17, 17) == weight);
        CHECK(map_visibility_window_weight(8, 16 - distance, 17, 17) == weight);
    }
    CHECK(map_visibility_window_weight(0, 0, 0, 0) == 0);
    CHECK(map_visibility_scale_radiance(2048, 0) == 0);
    CHECK(map_visibility_scale_radiance(2048, 16) == 128);
    CHECK(map_visibility_scale_radiance(2048, 256) == 2048);
    CHECK(map_visibility_scale_radiance(UINT16_MAX, UINT16_MAX) == UINT16_MAX);
    const uint16_t expected_boundary[] = {0, 128, 2048, UINT16_MAX};
    for (int x = 0; x <= 3; x++) {
        uint16_t scalar = UINT16_MAX;
        uint16_t rgb[3] = {UINT16_MAX, 32768, 16384};
        map_visibility_apply_window_fade(x, 8, 17, 17, &scalar, rgb);
        CHECK(scalar == expected_boundary[x]);
        CHECK(rgb[0] == expected_boundary[x]);
        CHECK(rgb[1] == (x == 3 ? 32768 : expected_boundary[x] / 2));
        CHECK(rgb[2] == (x == 3 ? 16384 : expected_boundary[x] / 4));
    }

    CHECK(MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE ==
          (MAP_VISIBILITY_MEMORY_FLOOR_RAW * 8U + 2U) / 5U);
    CHECK(map_visibility_memory_floor(0) == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    CHECK(map_visibility_memory_floor(MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE) ==
          MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    CHECK(map_visibility_memory_floor(UINT16_MAX) == UINT16_MAX);
    uint16_t remembered_scalar = 0;
    uint16_t remembered_rgb[3] = {0, 0, 0};
    map_visibility_apply_memory_floor(&remembered_scalar, remembered_rgb);
    CHECK(remembered_scalar == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    CHECK(remembered_rgb[0] == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    CHECK(remembered_rgb[1] == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    CHECK(remembered_rgb[2] == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    remembered_scalar = 32;
    remembered_rgb[0] = 32;
    remembered_rgb[1] = 0;
    remembered_rgb[2] = 0;
    map_visibility_apply_memory_floor(&remembered_scalar, remembered_rgb);
    CHECK(remembered_scalar == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    CHECK(remembered_rgb[0] == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    CHECK(remembered_rgb[1] == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE - 32);
    CHECK(remembered_rgb[2] == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE - 32);
    remembered_scalar = MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE;
    remembered_rgb[0] = 1000;
    remembered_rgb[1] = 2000;
    remembered_rgb[2] = 3000;
    map_visibility_apply_memory_floor(&remembered_scalar, remembered_rgb);
    CHECK(remembered_scalar == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    CHECK(remembered_rgb[0] == 1000 && remembered_rgb[1] == 2000 && remembered_rgb[2] == 3000);

    map_visibility_fade_t fade;
    map_visibility_fade_init(&fade);
    map_visibility_fade_authorize(&fade, 255, 1000);
    CHECK(fade.alpha == 0);
    CHECK(map_visibility_fade_advance(&fade, 1125));
    CHECK(fade.alpha == 128);
    CHECK(map_visibility_fade_interactive(&fade) == false);
    CHECK(map_visibility_fade_advance(&fade, 1250));
    CHECK(fade.alpha == 255);
    CHECK(map_visibility_fade_interactive(&fade));

    map_visibility_fade_revoke(&fade, 1250);
    CHECK(map_visibility_fade_advance(&fade, 1375));
    CHECK(fade.alpha == 127);
    CHECK(map_visibility_fade_advance(&fade, 1500));
    CHECK(fade.alpha == 0);
    CHECK(!map_visibility_fade_interactive(&fade));

    map_visibility_fade_authorize(&fade, 255, 2000);
    CHECK(map_visibility_fade_advance(&fade, 2250));
    CHECK(fade.alpha == 255);
    CHECK(!map_visibility_fade_advance(&fade, 2500));
    CHECK(fade.authorized);
    CHECK(!map_visibility_fade_advance(&fade, 2750));
    CHECK(fade.alpha == 255);
    CHECK(fade.authorized);
    return EXIT_SUCCESS;
}
