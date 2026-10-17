/*************************************************************************
 * Atrinik client protocol-1085 local light evaluation tests.            *
 *                                                                       *
 * Copyright 2026 The Atrinik Project                                   *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <client_lighting.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <toolkit/light_profile.h>
#include <toolkit/socket.h>

#define WIDTH 9
#define HEIGHT 9

#define TEST_CHECK(condition)                                               \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                            \
            abort();                                                        \
        }                                                                   \
    } while (0)

static client_lighting_cell_t cells[WIDTH * HEIGHT];
static uint8_t occluders[WIDTH * HEIGHT];

static const client_lighting_cell_t *cell_at(int x, int y) {
    return &cells[y * WIDTH + x];
}

static void evaluate(const client_lighting_emitter_t *emitters, size_t count) {
    client_lighting_evaluate(WIDTH, HEIGHT, occluders, emitters, count, cells);
}

static void test_open_white_source_matches_server_profile(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t torch = {.x = 4, .y = 4, .radius = 13, .color = 0xffffff};
    evaluate(&torch, 1);
    int mask = light_profile_mask(13);
    TEST_CHECK(cell_at(4, 4)->scalar == light_profile_value(mask, 0));
    TEST_CHECK(cell_at(5, 4)->scalar == light_profile_value(mask, 1));
    TEST_CHECK(cell_at(5, 5)->scalar == light_profile_value(mask, 2));
    for (int channel = 0; channel < 3; channel++) {
        TEST_CHECK(cell_at(4, 4)->rgb[channel] == cell_at(4, 4)->scalar);
    }
    TEST_CHECK(cell_at(0, 0)->scalar == 0);
}

static void test_wall_stops_light_but_keeps_its_face(void) {
    memset(occluders, 0, sizeof(occluders));
    occluders[4 * WIDTH + 4] = MAP2_OCCLUDER_BLOCKSVIEW;
    client_lighting_emitter_t torch = {.x = 2, .y = 4, .radius = 13, .color = 0xffffff};
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(4, 4)->scalar > 0);
    TEST_CHECK(cell_at(5, 4)->scalar == 0);
    TEST_CHECK(cell_at(6, 4)->scalar == 0);
    TEST_CHECK(cell_at(3, 4)->scalar > 0);
}

static void test_same_tile_emitters_share_the_server_cap(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t one = {.x = 4, .y = 4, .radius = 13, .color = 0xffffff};
    evaluate(&one, 1);
    int32_t single = cell_at(4, 4)->scalar;
    client_lighting_emitter_t two[2] = {one, one};
    evaluate(two, 2);
    TEST_CHECK(cell_at(4, 4)->scalar == single);
}

static void test_transmission_attenuates_crossed_cells(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t torch = {.x = 2, .y = 4, .radius = 13, .color = 0xffffff};
    evaluate(&torch, 1);
    int32_t open = cell_at(4, 4)->scalar;
    occluders[4 * WIDTH + 3] = (uint8_t)(8U << MAP2_OCCLUDER_ATTENUATION_SHIFT);
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(4, 4)->scalar == (open * 128 + 128) / 256);
}

static void test_darkness_is_negative_and_uncolored(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t dark = {.x = 4, .y = 4, .radius = -13, .color = 0};
    evaluate(&dark, 1);
    TEST_CHECK(cell_at(4, 4)->scalar < 0);
    for (int channel = 0; channel < 3; channel++) {
        TEST_CHECK(cell_at(4, 4)->rgb[channel] == 0);
    }
}

static void test_colored_light_keeps_hue(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t red = {.x = 4, .y = 4, .radius = 13, .color = 0xff0000};
    evaluate(&red, 1);
    TEST_CHECK(cell_at(4, 4)->rgb[0] == cell_at(4, 4)->scalar);
    TEST_CHECK(cell_at(4, 4)->rgb[1] == 0);
    TEST_CHECK(cell_at(4, 4)->rgb[2] == 0);
}

static void test_constituent_colors_keep_their_own_profiles(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t pair[2] = {
        {.x = 4, .y = 4, .radius = 13, .color = 0xff0000},
        {.x = 4, .y = 4, .radius = 1, .color = 0x0000ff},
    };
    evaluate(pair, 2);
    /* Both colors mix at the center; only the long red source reaches out. */
    TEST_CHECK(cell_at(4, 4)->rgb[0] > 0 && cell_at(4, 4)->rgb[2] > 0);
    TEST_CHECK(cell_at(7, 4)->rgb[0] > 0);
    TEST_CHECK(cell_at(7, 4)->rgb[2] == 0);
}

static void test_many_emitters_are_all_evaluated(void) {
    enum { SIDE = 20 };
    static client_lighting_emitter_t emitters[SIDE * SIDE];
    static client_lighting_cell_t field[SIDE * SIDE];
    size_t count = 0;
    for (int y = 0; y < SIDE; y++) {
        for (int x = 0; x < SIDE; x++) {
            emitters[count++] = (client_lighting_emitter_t){
                .x = x, .y = y, .radius = 1, .color = 0xffffff};
        }
    }
    client_lighting_evaluate(SIDE, SIDE, NULL, emitters, count, field);
    for (size_t i = 0; i < count; i++) {
        TEST_CHECK(field[i].scalar >= light_profile_value(light_profile_mask(1), 0));
    }
}

static void test_compose_by_sign_and_clamp_to_level(void) {
    int32_t step = MAP2_LOCAL_LIGHT_LEVEL_RAW;
    TEST_CHECK(client_lighting_compose(100, 2) == 2 * step);
    TEST_CHECK(client_lighting_compose(2 * step + 50, 2) == 2 * step + 50);
    TEST_CHECK(client_lighting_compose(1000, 2) == 3 * step - 1);
    TEST_CHECK(client_lighting_compose(0, -1) == -step);
    TEST_CHECK(client_lighting_compose(-step - 20, -1) == -step - 20);
    TEST_CHECK(client_lighting_compose(-1000, -1) == -2 * step + 1);
    TEST_CHECK(client_lighting_compose(500, 0) == step - 1);
    TEST_CHECK(client_lighting_compose(-500, 0) == -step + 1);
    TEST_CHECK(client_lighting_compose(42, 0) == 42);
    TEST_CHECK(client_lighting_compose(5000, MAP2_LOCAL_LIGHT_LEVEL_MAX) == 5000);
    TEST_CHECK(client_lighting_compose(-5000, -MAP2_LOCAL_LIGHT_LEVEL_MAX) == -5000);
}

int main(void) {
    test_open_white_source_matches_server_profile();
    test_wall_stops_light_but_keeps_its_face();
    test_same_tile_emitters_share_the_server_cap();
    test_transmission_attenuates_crossed_cells();
    test_darkness_is_negative_and_uncolored();
    test_colored_light_keeps_hue();
    test_constituent_colors_keep_their_own_profiles();
    test_many_emitters_are_all_evaluated();
    test_compose_by_sign_and_clamp_to_level();
    return EXIT_SUCCESS;
}
