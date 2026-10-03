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

int main(void) {
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
