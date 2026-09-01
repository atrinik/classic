/*************************************************************************
 * Atrinik client map-light state regression tests.
 *
 * Copyright 2026 The Atrinik Project
 ************************************************************************/

#include <map.h>
#include <stdlib.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

int main(void) {
    TEST_CHECK(map_layer_is_remembered(LAYER_FLOOR));
    TEST_CHECK(map_layer_is_remembered(LAYER_FMASK));
    TEST_CHECK(map_layer_is_remembered(LAYER_WALL));
    TEST_CHECK(!map_layer_is_remembered(LAYER_ITEM));
    TEST_CHECK(!map_layer_is_remembered(LAYER_ITEM2));
    TEST_CHECK(!map_layer_is_remembered(LAYER_LIVING));
    TEST_CHECK(!map_layer_is_remembered(LAYER_EFFECT));

    return EXIT_SUCCESS;
}
