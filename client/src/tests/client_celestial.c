/*************************************************************************
 * Atrinik client protocol-1085 celestial input tests.                   *
 *                                                                       *
 * Copyright 2026 The Atrinik Project                                   *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <client_celestial.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <toolkit/packet.h>
#include <toolkit/toolkit.h>

#define TEST_CHECK(condition)                                               \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                            \
            abort();                                                        \
        }                                                                   \
    } while (0)

/** A valid region profile at one hour of the year. */
static celestial_model_inputs_t profile(uint16_t hour, uint16_t brightness) {
    celestial_model_inputs_t inputs = {
        .day_linear = {UINT16_MAX, UINT16_MAX, UINT16_MAX},
        .night_linear = {0, 0, UINT16_MAX},
        .day_brightness = brightness,
        .night_brightness = 32,
        .moon_linear = {UINT16_MAX, UINT16_MAX, 60000},
        .moon_max = 12,
        .starlight_linear = {50000, 50000, UINT16_MAX},
        .starlight_strength = 1,
        .lunar_period = 672,
        .solar_hour = hour % CELESTIAL_MODEL_HOURS_PER_DAY,
        .season_phase = hour % CELESTIAL_MODEL_HOURS_PER_YEAR,
        .lunar_age = hour % 672,
    };
    return inputs;
}

static celestial_protocol_slot_t slot_at(uint8_t slot, uint16_t hour, uint16_t brightness) {
    celestial_protocol_slot_t entry = {.slot = slot};
    entry.current = profile(hour, brightness);
    entry.next = profile(hour + 1, brightness);
    return entry;
}

static packet_struct *encode(uint32_t generation, const celestial_protocol_slot_t *slots,
                             size_t count) {
    packet_struct *packet = packet_new(0, 64, 0);
    TEST_CHECK(celestial_protocol_write(packet, generation, UINT64_C(3600) * 12, slots, count));
    return packet;
}

/** Stage a closure of the given heap fragments for a 9 x 9 window. */
static bool stage_closure(uint32_t generation, celestial_closure_fragment_t *fragments,
                          size_t count) {
    celestial_closure_t closure;
    TEST_CHECK(celestial_closure_build(&closure, generation, 9, 9, fragments, count));
    packet_struct *packet = packet_new(0, 64, 0);
    TEST_CHECK(celestial_closure_write(packet, &closure));
    bool staged = client_celestial_apply_structure(packet->data, packet->len, 9, 9);
    packet_free(packet);
    celestial_closure_free(&closure);
    return staged;
}

/** Stage and publish a CELESTIAL payload with an empty closure. */
static bool apply(uint32_t generation, const celestial_protocol_slot_t *slots, size_t count) {
    packet_struct *packet = encode(generation, slots, count);
    bool applied = client_celestial_apply(packet->data, packet->len);
    packet_free(packet);
    return applied && stage_closure(generation, NULL, 0) && client_celestial_publish(NULL, NULL);
}

static void test_payload_round_trips_every_slot_id(void) {
    static celestial_protocol_slot_t slots[CELESTIAL_PROTOCOL_SLOTS_MAX];
    static celestial_protocol_slot_t decoded[CELESTIAL_PROTOCOL_SLOTS_MAX];
    for (size_t i = 0; i < CELESTIAL_PROTOCOL_SLOTS_MAX - 1; i++) {
        slots[i] = slot_at((uint8_t)i, (uint16_t)(i * 31), (uint16_t)(100 + i));
    }
    size_t count = CELESTIAL_PROTOCOL_SLOTS_MAX - 1;
    packet_struct *packet = encode(7, slots, count);
    TEST_CHECK(packet->len == 13 + count * 43);
    uint32_t generation;
    uint64_t start;
    size_t parsed;
    TEST_CHECK(celestial_protocol_read(packet->data, packet->len, &generation, &start, decoded,
                                       &parsed));
    TEST_CHECK(generation == 7 && start == UINT64_C(3600) * 12 && parsed == count);
    TEST_CHECK(memcmp(decoded, slots, count * sizeof(*slots)) == 0);
    packet_free(packet);
}

static void test_encoder_rejects_what_decoders_reject(void) {
    celestial_protocol_slot_t slots[2] = {slot_at(3, 5, 200), slot_at(3, 9, 200)};
    packet_struct *packet = packet_new(0, 64, 0);
    /* Slot 255 is the no-slot sentinel; ids ascend strictly. */
    slots[0].slot = CELESTIAL_PROTOCOL_SLOT_NONE;
    TEST_CHECK(!celestial_protocol_write(packet, 1, 0, slots, 1));
    slots[0].slot = 3;
    TEST_CHECK(!celestial_protocol_write(packet, 1, 0, slots, 2));
    /* Current and next hour share one profile. */
    slots[0].next.day_brightness++;
    TEST_CHECK(!celestial_protocol_write(packet, 1, 0, slots, 1));
    /* Brightness is bounded as region profiles bound it. */
    slots[0] = slot_at(3, 5, CELESTIAL_MODEL_BRIGHTNESS_MAX);
    TEST_CHECK(celestial_protocol_write(packet, 1, 0, slots, 1));
    packet->len = 0;
    slots[0] = slot_at(3, 5, CELESTIAL_MODEL_BRIGHTNESS_MAX + 1);
    TEST_CHECK(!celestial_protocol_write(packet, 1, 0, slots, 1));
    slots[0] = slot_at(3, 5, 200);
    slots[0].current.night_brightness = slots[0].next.night_brightness =
        CELESTIAL_MODEL_BRIGHTNESS_MAX + 1;
    TEST_CHECK(!celestial_protocol_write(packet, 1, 0, slots, 1));
    slots[0] = slot_at(3, 5, 200);
    slots[0].current.lunar_period = 100;
    TEST_CHECK(!celestial_protocol_write(packet, 1, 0, slots, 1));
    /* Day and night colors are black or normalized to a 65535 peak. */
    slots[0] = slot_at(3, 5, 200);
    slots[0].current.day_linear[0] = slots[0].next.day_linear[0] = 30000;
    slots[0].current.day_linear[1] = slots[0].next.day_linear[1] = 30000;
    slots[0].current.day_linear[2] = slots[0].next.day_linear[2] = 30000;
    TEST_CHECK(!celestial_protocol_write(packet, 1, 0, slots, 1));
    slots[0] = slot_at(3, 5, 200);
    slots[0].current.night_linear[2] = slots[0].next.night_linear[2] = 1;
    TEST_CHECK(!celestial_protocol_write(packet, 1, 0, slots, 1));
    TEST_CHECK(packet->len == 0);
    /* Padding never decides whether two hours share a profile. */
    celestial_protocol_slot_t padded;
    memset(&padded, 0xa5, sizeof(padded));
    celestial_protocol_slot_t clean = slot_at(3, 5, 200);
    padded.slot = clean.slot;
    padded.current = clean.current;
    memcpy(padded.next.day_linear, clean.next.day_linear, sizeof(clean.next.day_linear));
    memcpy(padded.next.night_linear, clean.next.night_linear, sizeof(clean.next.night_linear));
    padded.next.day_brightness = clean.next.day_brightness;
    padded.next.night_brightness = clean.next.night_brightness;
    memcpy(padded.next.moon_linear, clean.next.moon_linear, sizeof(clean.next.moon_linear));
    padded.next.moon_max = clean.next.moon_max;
    memcpy(padded.next.starlight_linear, clean.next.starlight_linear,
           sizeof(clean.next.starlight_linear));
    padded.next.starlight_strength = clean.next.starlight_strength;
    padded.next.lunar_period = clean.next.lunar_period;
    padded.next.solar_hour = clean.next.solar_hour;
    padded.next.season_phase = clean.next.season_phase;
    padded.next.lunar_age = clean.next.lunar_age;
    TEST_CHECK(celestial_protocol_write(packet, 1, 0, &padded, 1));
    packet->len = 0;
    packet_free(packet);
}

static void test_payload_replaces_the_slot_table(void) {
    client_celestial_reset();
    celestial_protocol_slot_t slots[2] = {slot_at(0, 10, 300), slot_at(9, 20, 400)};
    TEST_CHECK(apply(1, slots, 2));
    celestial_model_t model, expected;
    TEST_CHECK(client_celestial_model(9, true, &model));
    celestial_model_evaluate(&slots[1].next, &expected);
    TEST_CHECK(memcmp(&model, &expected, sizeof(model)) == 0);
    TEST_CHECK(client_celestial_start_seconds() == UINT64_C(3600) * 12);

    /* A slot the next payload omits is no longer shown. */
    uint64_t revision = client_celestial_revision();
    TEST_CHECK(apply(2, &slots[1], 1));
    TEST_CHECK(!client_celestial_model(0, false, &model));
    TEST_CHECK(client_celestial_model(9, false, &model));
    TEST_CHECK(client_celestial_generation() == 2);
    TEST_CHECK(client_celestial_revision() != revision);
    TEST_CHECK(!client_celestial_model(CELESTIAL_PROTOCOL_SLOT_NONE, false, &model));
}

static void test_stale_and_malformed_payloads_keep_state(void) {
    client_celestial_reset();
    celestial_protocol_slot_t slot = slot_at(4, 30, 500);
    TEST_CHECK(apply(5, &slot, 1));
    uint64_t revision = client_celestial_revision();
    celestial_protocol_slot_t other = slot_at(6, 31, 500);
    TEST_CHECK(!apply(5, &other, 1));
    TEST_CHECK(!apply(4, &other, 1));

    packet_struct *packet = encode(6, &other, 1);
    TEST_CHECK(!client_celestial_apply(packet->data, packet->len - 1));
    packet_writer_write_uint8(packet, 0);
    TEST_CHECK(!client_celestial_apply(packet->data, packet->len));
    /* A phase outside its period is rejected, not clamped. */
    packet->len--;
    packet->data[13 + 38] = CELESTIAL_MODEL_HOURS_PER_DAY;
    TEST_CHECK(!client_celestial_apply(packet->data, packet->len));
    /* So is a day brightness beyond 1024 (bytes 13..14 of the slot). */
    packet->data[13 + 38] = 0;
    packet->data[13 + 13] = 0x04;
    packet->data[13 + 14] = 0x01;
    TEST_CHECK(!client_celestial_apply(packet->data, packet->len));
    packet_free(packet);

    celestial_model_t model;
    TEST_CHECK(client_celestial_revision() == revision);
    TEST_CHECK(client_celestial_generation() == 5);
    TEST_CHECK(client_celestial_model(4, false, &model));
    TEST_CHECK(!client_celestial_model(6, false, &model));
}

static void test_reset_forgets_slots_and_generation(void) {
    celestial_protocol_slot_t slot = slot_at(4, 30, 500);
    client_celestial_reset();
    TEST_CHECK(apply(9, &slot, 1));
    uint64_t revision = client_celestial_revision();
    client_celestial_reset();
    celestial_model_t model;
    TEST_CHECK(!client_celestial_model(4, false, &model));
    TEST_CHECK(client_celestial_generation() == 0);
    TEST_CHECK(client_celestial_revision() != revision);
    /* A new connection's generations start over. */
    TEST_CHECK(apply(1, &slot, 1));
}

#define OPEN_AIR                                                                        \
    (CELESTIAL_CLOSURE_IN | UINT32_C(0x3ff) | CELESTIAL_CLOSURE_COVER_OPEN |            \
     CELESTIAL_CLOSURE_SKY_EXIT | CELESTIAL_CLOSURE_CONTINUOUS_NORTH |                  \
     CELESTIAL_CLOSURE_CONTINUOUS_EAST | CELESTIAL_CLOSURE_CONTINUOUS_SOUTH |           \
     CELESTIAL_CLOSURE_CONTINUOUS_WEST)

static celestial_closure_fragment_t fragment_of(int8_t depth, uint8_t x, uint8_t y,
                                                uint8_t width, uint8_t height, uint8_t slot,
                                                uint32_t value) {
    celestial_closure_fragment_t fragment = {.depth = depth, .x = x, .y = y, .width = width,
                                             .height = height, .slot = slot};
    fragment.values = malloc((size_t)width * height * sizeof(*fragment.values));
    TEST_CHECK(fragment.values != NULL);
    for (size_t i = 0; i < (size_t)width * height; i++) {
        fragment.values[i] = value;
    }
    return fragment;
}

typedef struct published_cells {
    size_t count;
    int depth[81], x[81], y[81];
    int32_t current[81][4], next[81][4];
} published_cells;

static void record_cell(void *context, int depth, int x, int y, const int32_t current[4],
                        const int32_t next[4]) {
    published_cells *cells = context;
    TEST_CHECK(cells->count < 81);
    cells->depth[cells->count] = depth;
    cells->x[cells->count] = x;
    cells->y[cells->count] = y;
    memcpy(cells->current[cells->count], current, sizeof(cells->current[0]));
    memcpy(cells->next[cells->count], next, sizeof(cells->next[0]));
    cells->count++;
}

static void test_pair_publishes_only_together(void) {
    client_celestial_reset();
    celestial_protocol_slot_t slot = slot_at(9, 12, 500);
    packet_struct *packet = encode(1, &slot, 1);
    TEST_CHECK(client_celestial_apply(packet->data, packet->len));
    packet_free(packet);
    /* A CELESTIAL payload alone is staged, not published. */
    TEST_CHECK(!client_celestial_publish(NULL, NULL));
    TEST_CHECK(client_celestial_generation() == 0);
    celestial_model_t model;
    TEST_CHECK(!client_celestial_model(9, false, &model));
    /* A closure of another generation does not pair with it. */
    TEST_CHECK(stage_closure(2, NULL, 0));
    TEST_CHECK(!client_celestial_publish(NULL, NULL));
    /* A failed MAP2 update discards both. */
    client_celestial_discard_staged();
    packet = encode(2, &slot, 1);
    TEST_CHECK(client_celestial_apply(packet->data, packet->len));
    packet_free(packet);
    TEST_CHECK(!client_celestial_publish(NULL, NULL));
    TEST_CHECK(stage_closure(2, NULL, 0));
    TEST_CHECK(client_celestial_publish(NULL, NULL));
    TEST_CHECK(client_celestial_generation() == 2);
    TEST_CHECK(client_celestial_model(9, false, &model));
    /* Stale closures are rejected once a pair is published. */
    TEST_CHECK(!stage_closure(2, NULL, 0));
}

static void test_staged_generations_never_regress(void) {
    client_celestial_reset();
    celestial_protocol_slot_t slot = slot_at(9, 12, 500);
    /* Generation 3 is staged; an older 2 arriving late cannot replace it. */
    packet_struct *packet = encode(3, &slot, 1);
    TEST_CHECK(client_celestial_apply(packet->data, packet->len));
    packet_free(packet);
    TEST_CHECK(stage_closure(3, NULL, 0));
    packet = encode(2, &slot, 1);
    TEST_CHECK(!client_celestial_apply(packet->data, packet->len));
    packet_free(packet);
    TEST_CHECK(!stage_closure(2, NULL, 0));
    /* A duplicate of the staged generation never replaces it either. */
    celestial_protocol_slot_t other = slot_at(4, 12, 500);
    packet = encode(3, &other, 1);
    TEST_CHECK(!client_celestial_apply(packet->data, packet->len));
    packet_free(packet);
    TEST_CHECK(!stage_closure(3, NULL, 0));
    TEST_CHECK(client_celestial_publish(NULL, NULL));
    TEST_CHECK(client_celestial_generation() == 3);
    celestial_model_t model;
    TEST_CHECK(client_celestial_model(9, false, &model) && !client_celestial_model(4, false, &model));
}

static void test_fragments_must_name_published_slots(void) {
    client_celestial_reset();
    celestial_protocol_slot_t slot = slot_at(9, 12, 500);
    packet_struct *packet = encode(1, &slot, 1);
    TEST_CHECK(client_celestial_apply(packet->data, packet->len));
    packet_free(packet);
    /* Slot 4 is not in the paired CELESTIAL: the pair is malformed. */
    celestial_closure_fragment_t *fragments = malloc(sizeof(*fragments));
    TEST_CHECK(fragments != NULL);
    fragments[0] = fragment_of(0, 1, 1, 2, 2, 4, OPEN_AIR);
    TEST_CHECK(stage_closure(1, fragments, 1));
    TEST_CHECK(!client_celestial_publish(NULL, NULL));
    TEST_CHECK(client_celestial_generation() == 0);
    /* The pair was discarded, so its CELESTIAL half cannot pair again. */
    TEST_CHECK(stage_closure(1, NULL, 0));
    TEST_CHECK(!client_celestial_publish(NULL, NULL));
}

static void test_malformed_half_discards_the_staged_pair(void) {
    client_celestial_reset();
    celestial_protocol_slot_t slot = slot_at(9, 12, 500);
    packet_struct *packet = encode(1, &slot, 1);
    TEST_CHECK(client_celestial_apply(packet->data, packet->len));
    packet_free(packet);
    /* A malformed structure voids the staged CELESTIAL half, so a later
     * structure of its generation cannot pair with it. */
    static const uint8_t truncated[] = {0, 0, 0, 1, 0};
    TEST_CHECK(!client_celestial_apply_structure(truncated, sizeof(truncated), 9, 9));
    TEST_CHECK(stage_closure(1, NULL, 0));
    TEST_CHECK(!client_celestial_publish(NULL, NULL));
    TEST_CHECK(client_celestial_generation() == 0);
}

static void test_publication_solves_every_closure_cell(void) {
    client_celestial_reset();
    celestial_protocol_slot_t slot = slot_at(9, 12, 900);
    packet_struct *packet = encode(1, &slot, 1);
    TEST_CHECK(client_celestial_apply(packet->data, packet->len));
    packet_free(packet);
    celestial_closure_fragment_t *fragments = malloc(2 * sizeof(*fragments));
    TEST_CHECK(fragments != NULL);
    fragments[0] = fragment_of(0, 1, 1, 3, 2, 9, OPEN_AIR);
    fragments[1] = fragment_of(1, 4, 4, 2, 2, CELESTIAL_PROTOCOL_SLOT_NONE, OPEN_AIR);
    fragments[0].values[1] = 0;
    TEST_CHECK(stage_closure(1, fragments, 2));
    published_cells cells = {0};
    TEST_CHECK(client_celestial_publish(record_cell, &cells));
    /* Fragments are solved whole, one at least per call. */
    TEST_CHECK(cells.count == 0);
    TEST_CHECK(!client_celestial_process(0));
    TEST_CHECK(cells.count == 5);
    TEST_CHECK(client_celestial_process(UINT64_MAX));
    /* Five closure cells of the first fragment, four of the second. */
    TEST_CHECK(cells.count == 9);
    celestial_model_t current, next;
    celestial_model_evaluate(&slot.current, &current);
    celestial_model_evaluate(&slot.next, &next);
    const celestial_closure_t *closure = client_celestial_closure();
    int32_t values[6], rgb[6][3], next_values[6], next_rgb[6][3];
    TEST_CHECK(celestial_closure_solve(closure, 0, &current, values, rgb));
    TEST_CHECK(celestial_closure_solve(closure, 0, &next, next_values, next_rgb));
    for (size_t i = 0; i < cells.count; i++) {
        if (cells.depth[i] == 1) {
            /* Slot 255: zero celestial light. */
            for (size_t w = 0; w < 4; w++) {
                TEST_CHECK(cells.current[i][w] == 0 && cells.next[i][w] == 0);
            }
            continue;
        }
        size_t index = (size_t)(cells.x[i] - 1) + 3 * (size_t)(cells.y[i] - 1);
        TEST_CHECK(index != 1);
        TEST_CHECK(cells.current[i][0] == values[index]);
        TEST_CHECK(memcmp(&cells.current[i][1], rgb[index], sizeof(rgb[index])) == 0);
        TEST_CHECK(cells.next[i][0] == next_values[index]);
        TEST_CHECK(memcmp(&cells.next[i][1], next_rgb[index], sizeof(next_rgb[index])) == 0);
        TEST_CHECK(cells.current[i][0] > 0);
    }
}

static void test_worst_case_closure_cost(void) {
    /* Every cell of a 32 x 32 window over all 13 depths. */
    client_celestial_reset();
    celestial_protocol_slot_t slot = slot_at(0, 15, 900);
    packet_struct *packet = encode(1, &slot, 1);
    TEST_CHECK(client_celestial_apply(packet->data, packet->len));
    packet_free(packet);
    celestial_closure_fragment_t *fragments =
        malloc(CELESTIAL_CLOSURE_DEPTHS * sizeof(*fragments));
    TEST_CHECK(fragments != NULL);
    for (int depth = 0; depth < CELESTIAL_CLOSURE_DEPTHS; depth++) {
        uint32_t value = (OPEN_AIR & ~CELESTIAL_CLOSURE_SKY_EXIT) |
                         CELESTIAL_CLOSURE_CONTINUOUS_UP | CELESTIAL_CLOSURE_CONTINUOUS_DOWN;
        if (depth == CELESTIAL_CLOSURE_DEPTHS - 1) {
            value = (OPEN_AIR | CELESTIAL_CLOSURE_CONTINUOUS_DOWN);
        }
        fragments[depth] = fragment_of((int8_t)(depth - CELESTIAL_CLOSURE_DEPTH_MAX), 0, 0, 32,
                                       32, 0, value);
    }
    celestial_closure_t closure;
    TEST_CHECK(celestial_closure_build(&closure, 1, 32, 32, fragments, CELESTIAL_CLOSURE_DEPTHS));
    packet = packet_new(0, 1024, 0);
    TEST_CHECK(celestial_closure_write(packet, &closure));
    size_t wire = packet->len;
    TEST_CHECK(client_celestial_apply_structure(packet->data, packet->len, 32, 32));
    packet_free(packet);
    celestial_closure_free(&closure);
    TEST_CHECK(client_celestial_publish(NULL, NULL));
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    celestial_model_t model;
    TEST_CHECK(client_celestial_model(0, false, &model));
    for (size_t i = 0; i < client_celestial_closure()->count; i++) {
        static int32_t values[32 * 32], rgb[32 * 32][3];
        TEST_CHECK(celestial_closure_solve(client_celestial_closure(), i, &model, values, rgb));
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = (end.tv_sec - start.tv_sec) * 1e3 + (end.tv_nsec - start.tv_nsec) / 1e6;
    fprintf(stderr, "worst-case closure: %zu bytes on the wire, one hourly field in %.1f ms\n",
            wire, ms);
}

int main(void) {
    toolkit_import(packet);
    test_payload_round_trips_every_slot_id();
    test_encoder_rejects_what_decoders_reject();
    test_payload_replaces_the_slot_table();
    test_stale_and_malformed_payloads_keep_state();
    test_reset_forgets_slots_and_generation();
    test_pair_publishes_only_together();
    test_staged_generations_never_regress();
    test_malformed_half_discards_the_staged_pair();
    test_fragments_must_name_published_slots();
    test_publication_solves_every_closure_cell();
    test_worst_case_closure_cost();
    toolkit_deinit();
    return EXIT_SUCCESS;
}
