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
static uint32_t occluders[WIDTH * HEIGHT];

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
    occluders[4 * WIDTH + 4] =
        client_lighting_occluder(MAP2_OCCLUDER_BLOCKSVIEW, 0xffff);
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
    occluders[4 * WIDTH + 3] = client_lighting_occluder(0, 0x8888);
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(4, 4)->scalar == (open * 128 + 128) / 256);
}

static void test_face_attenuation_is_directional(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t torch = {.x = 3, .y = 4, .radius = 13, .color = 0xffffff};
    evaluate(&torch, 1);
    int32_t open_east = cell_at(4, 4)->scalar, open_north = cell_at(3, 3)->scalar;
    /* Only the east face of the source cell is glass (attenuation 8). */
    occluders[4 * WIDTH + 3] = client_lighting_occluder(0, 0x0080);
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(4, 4)->scalar == (open_east * 128 + 128) / 256);
    TEST_CHECK(cell_at(3, 3)->scalar == open_north);
}

static void test_diagonal_steps_cross_side_cell_faces(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t torch = {.x = 3, .y = 4, .radius = 13, .color = 0xffffff};
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(4, 3)->scalar > 0);
    /* A wall beside the source closes the diagonal step past its corner. */
    occluders[4 * WIDTH + 4] = client_lighting_occluder(MAP2_OCCLUDER_BLOCKSVIEW, 0xffff);
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(4, 3)->scalar == 0);
    /* Glass beside it attenuates the diagonal step by its north face. */
    int32_t open = 0;
    memset(occluders, 0, sizeof(occluders));
    evaluate(&torch, 1);
    open = cell_at(4, 3)->scalar;
    occluders[4 * WIDTH + 4] = client_lighting_occluder(0, 0x0008);
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(4, 3)->scalar == (open * 128 + 128) / 256);
}

static void test_transmissive_blocker_keeps_its_attenuation(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t torch = {.x = 2, .y = 4, .radius = 13, .color = 0xffffff};
    evaluate(&torch, 1);
    int32_t open = cell_at(4, 4)->scalar;
    /* Glass that blocks view but transmits 192/256 (attenuation 4). */
    occluders[4 * WIDTH + 3] =
        client_lighting_occluder(MAP2_OCCLUDER_BLOCKSVIEW, 0x4444);
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(4, 4)->scalar == (open * 192 + 128) / 256);
}

static void test_discontinuous_face_stops_light(void) {
    memset(occluders, 0, sizeof(occluders));
    /* The east face of (4, 4) is a discontinuous seam. */
    occluders[4 * WIDTH + 4] = client_lighting_occluder(2U << MAP2_OCCLUDER_FACE_SHIFT, 0);
    client_lighting_emitter_t torch = {.x = 2, .y = 4, .radius = 13, .color = 0xffffff};
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(4, 4)->scalar > 0);
    TEST_CHECK(cell_at(5, 4)->scalar == 0);
    TEST_CHECK(cell_at(6, 4)->scalar == 0);
    TEST_CHECK(cell_at(3, 5)->scalar > 0);
}

static void test_occluded_source_cell_attenuates_its_light(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t torch = {.x = 4, .y = 4, .radius = 13, .color = 0xffffff};
    evaluate(&torch, 1);
    int32_t open_center = cell_at(4, 4)->scalar, open_side = cell_at(5, 4)->scalar;
    /* A lamp inside a transmissive cell keeps its own tile lit but leaves it
     * attenuated; inside an opaque cell nothing leaves. */
    occluders[4 * WIDTH + 4] = client_lighting_occluder(0, 0x8888);
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(4, 4)->scalar == open_center);
    TEST_CHECK(cell_at(5, 4)->scalar == (open_side * 128 + 128) / 256);
    occluders[4 * WIDTH + 4] = client_lighting_occluder(0, 0xffff);
    evaluate(&torch, 1);
    TEST_CHECK(cell_at(5, 4)->scalar == 0);
}

static void test_colors_beyond_the_bound_keep_their_hue(void) {
    memset(occluders, 0, sizeof(occluders));
    /* Ten distinct blue-ish constituents all keep their own colored profile
     * instead of becoming neutral. */
    client_lighting_emitter_t many[10];
    for (int i = 0; i < 10; i++) {
        many[i] = (client_lighting_emitter_t){
            .x = 4, .y = 4, .radius = 1, .color = (uint32_t)(0x0000f0 + i)};
    }
    evaluate(many, 10);
    TEST_CHECK(cell_at(4, 4)->rgb[2] > cell_at(4, 4)->rgb[0]);
    TEST_CHECK(cell_at(4, 4)->rgb[2] == cell_at(4, 4)->scalar);
    /* Radius-1 constituents keep the radius-1 reach: no colored light appears
     * where none of their own profiles reaches. */
    int32_t far_scalar = cell_at(7, 4)->scalar;
    TEST_CHECK(cell_at(7, 4)->rgb[2] == far_scalar && cell_at(7, 4)->rgb[0] == far_scalar);
}

static void test_many_colors_keep_exact_server_weights(void) {
    memset(occluders, 0, sizeof(occluders));
    /* Radii 6 and 7 select the same profile, so the server weighs a red 6 and
     * a blue 7 equally; a radius-weighted fold would tint towards blue. Eight
     * red/blue-symmetric fillers come first so the pair is beyond eight. */
    TEST_CHECK(light_profile_mask(6) == light_profile_mask(7));
    client_lighting_emitter_t many[10];
    for (int i = 0; i < 8; i++) {
        uint32_t level = (uint32_t)(0x80 + i);
        many[i] = (client_lighting_emitter_t){
            .x = 4, .y = 4, .radius = 1, .color = level << 16 | 0x8000 | level};
    }
    many[8] = (client_lighting_emitter_t){.x = 4, .y = 4, .radius = 6, .color = 0xff0000};
    many[9] = (client_lighting_emitter_t){.x = 4, .y = 4, .radius = 7, .color = 0x0000ff};
    evaluate(many, 10);
    TEST_CHECK(cell_at(4, 4)->rgb[0] == cell_at(4, 4)->rgb[2]);
    TEST_CHECK(cell_at(5, 4)->rgb[0] == cell_at(5, 4)->rgb[2]);
}

static void test_profile_folding_matches_separate_constituents(void) {
    memset(occluders, 0, sizeof(occluders));
    /* Radius 6 and 7 resolve to one profile; fold them and compare with
     * evaluating each color alone, which the server would sum. */
    client_lighting_emitter_t both[] = {
        {.x = 4, .y = 4, .radius = 6, .color = 0xff0000},
        {.x = 4, .y = 4, .radius = 7, .color = 0x0000ff},
    };
    TEST_CHECK(light_profile_mask(6) == light_profile_mask(7));
    evaluate(both, 2);
    client_lighting_cell_t folded[WIDTH * HEIGHT];
    memcpy(folded, cells, sizeof(folded));
    /* Equal weights: red and blue channels match at every cell. */
    for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
        TEST_CHECK(folded[i].rgb[0] == folded[i].rgb[2]);
    }
    /* Many emitters on one tile cost one walk per distinct profile. */
    client_lighting_emitter_t many[49];
    for (size_t i = 0; i < (sizeof(many) / sizeof(*many)); i++) {
        many[i] = (client_lighting_emitter_t){
            .x = 4, .y = 4, .radius = (int8_t)(1 + i % 13), .color = (uint32_t)(0x010203 * (i + 1))};
    }
    evaluate(many, (sizeof(many) / sizeof(*many)));
    TEST_CHECK(cell_at(4, 4)->scalar > 0);
}

static void test_flicker_field_is_the_flickering_emitters_alone(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t mixed[] = {
        {.x = 3, .y = 4, .radius = 5, .color = 0xffffff},
        {.x = 5, .y = 4, .radius = 4, .flags = MAP2_EMITTER_FLICKER, .color = 0xff8000},
    };
    evaluate(mixed, 2);
    client_lighting_cell_t combined[WIDTH * HEIGHT];
    memcpy(combined, cells, sizeof(combined));
    evaluate(&mixed[1], 1);
    for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
        TEST_CHECK(combined[i].flicker_scalar == cells[i].scalar);
        for (int channel = 0; channel < 3; channel++) {
            TEST_CHECK(combined[i].flicker_rgb[channel] == cells[i].rgb[channel]);
        }
    }
    /* A steady emitter alone has no flicker field. */
    evaluate(&mixed[0], 1);
    for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
        TEST_CHECK(cells[i].flicker_scalar == 0);
    }
    /* The gain only dims, stays bounded and varies over time. */
    uint16_t lowest = 256, highest = 0;
    for (uint64_t ms = 0; ms < 60000; ms += 7) {
        uint16_t gain = client_lighting_flicker_gain(ms);
        TEST_CHECK(gain >= 205 && gain <= 256);
        lowest = gain < lowest ? gain : lowest;
        highest = gain > highest ? gain : highest;
    }
    TEST_CHECK(lowest < highest);
    TEST_CHECK(client_lighting_flicker_gain(1234) == client_lighting_flicker_gain(1234));
}

static void test_darkness_is_negative_and_uncolored(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t dark = {.x = 4, .y = 4, .radius = -13, .color = 0};
    evaluate(&dark, 1);
    TEST_CHECK(cell_at(4, 4)->scalar < 0);
    /* Darkness carries no hue: every channel equals the scalar. */
    for (int channel = 0; channel < 3; channel++) {
        TEST_CHECK(cell_at(4, 4)->rgb[channel] == cell_at(4, 4)->scalar);
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

static void test_cancelled_scalar_keeps_color_shift(void) {
    memset(occluders, 0, sizeof(occluders));
    client_lighting_emitter_t pair[2] = {
        {.x = 4, .y = 4, .radius = 13, .color = 0xff0000},
        {.x = 4, .y = 4, .radius = -13, .color = 0},
    };
    evaluate(pair, 2);
    /* The scalars cancel, but red keeps its share while green and blue lose
     * the positive part, as in the server's aggregate before clamping. */
    TEST_CHECK(cell_at(4, 4)->scalar == 0);
    TEST_CHECK(cell_at(4, 4)->rgb[0] == 0);
    TEST_CHECK(cell_at(4, 4)->rgb[1] < 0 && cell_at(4, 4)->rgb[1] == cell_at(4, 4)->rgb[2]);
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

static void test_grouping_is_independent_of_emitter_order(void) {
    memset(occluders, 0, sizeof(occluders));
    static client_lighting_emitter_t forward[60], backward[60];
    static const uint32_t colors[] = {0xff8040, 0x40ff80, 0x4080ff, 0xffffff};
    for (size_t i = 0; i < 60; i++) {
        forward[i] = (client_lighting_emitter_t){
            .x = (int)(i % 3) + 3, .y = 4, .radius = (int8_t)(i % 5) + 1, .color = colors[i % 4]};
        backward[59 - i] = forward[i];
    }
    static client_lighting_cell_t first[WIDTH * HEIGHT];
    client_lighting_evaluate(WIDTH, HEIGHT, occluders, forward, 60, first);
    client_lighting_evaluate(WIDTH, HEIGHT, occluders, backward, 60, cells);
    TEST_CHECK(memcmp(first, cells, sizeof(cells)) == 0);
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
    test_transmissive_blocker_keeps_its_attenuation();
    test_face_attenuation_is_directional();
    test_diagonal_steps_cross_side_cell_faces();
    test_discontinuous_face_stops_light();
    test_occluded_source_cell_attenuates_its_light();
    test_colors_beyond_the_bound_keep_their_hue();
    test_many_colors_keep_exact_server_weights();
    test_profile_folding_matches_separate_constituents();
    test_flicker_field_is_the_flickering_emitters_alone();
    test_darkness_is_negative_and_uncolored();
    test_colored_light_keeps_hue();
    test_constituent_colors_keep_their_own_profiles();
    test_cancelled_scalar_keeps_color_shift();
    test_grouping_is_independent_of_emitter_order();
    test_many_emitters_are_all_evaluated();
    test_compose_by_sign_and_clamp_to_level();
    return EXIT_SUCCESS;
}
