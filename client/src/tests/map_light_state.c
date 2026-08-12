/*************************************************************************
 * Atrinik client map-light state regression tests.
 *
 * Copyright 2026 The Atrinik Project
 ************************************************************************/

#include <global.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

int main(void) {
    MapCell cell = {0};
    cell.light_radiance[2] = 128;
    cell.light_known[2] = 1;
    cell.light_rgb_radiance[2][0] = 128;
    cell.light_rgb_radiance[2][1] = 0;
    cell.light_rgb_radiance[2][2] = 0;
    cell.light_rgb_explicit = UINT8_C(1) << 2;

    map_cell_clear_light_state(&cell);
    TEST_CHECK(cell.light_radiance[2] == 128);
    TEST_CHECK(cell.light_known[2] == 0);
    TEST_CHECK(cell.light_rgb_explicit == 0);
    TEST_CHECK(cell.light_rgb_radiance[2][0] == 0);
    TEST_CHECK(cell.light_rgb_radiance[2][1] == 0);
    TEST_CHECK(cell.light_rgb_radiance[2][2] == 0);
    TEST_CHECK(sizeof(cell.light_radiance) + sizeof(cell.light_rgb_radiance) == 56);

    return EXIT_SUCCESS;
}
