/*************************************************************************
 * Atrinik client visibility field and fade regression tests.
 *
 * Copyright 2026 The Atrinik Project
 *************************************************************************/

#include <global.h>
#include <map_visibility.h>

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
    CHECK(map_visibility_add_player_radiance(1280, 256) == 2304);
    CHECK(map_visibility_add_player_radiance(UINT16_MAX, 256) == UINT16_MAX);

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
    remembered_scalar = 128;
    remembered_rgb[0] = 128;
    remembered_rgb[1] = 0;
    remembered_rgb[2] = 0;
    map_visibility_apply_memory_floor(&remembered_scalar, remembered_rgb);
    CHECK(remembered_scalar == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    CHECK(remembered_rgb[0] == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE);
    CHECK(remembered_rgb[1] == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE - 128);
    CHECK(remembered_rgb[2] == MAP_VISIBILITY_MEMORY_FLOOR_RADIANCE - 128);
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
