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

static bool apply(uint32_t generation, const celestial_protocol_slot_t *slots, size_t count) {
    packet_struct *packet = encode(generation, slots, count);
    bool applied = client_celestial_apply(packet->data, packet->len);
    packet_free(packet);
    return applied;
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

int main(void) {
    toolkit_import(packet);
    test_payload_round_trips_every_slot_id();
    test_encoder_rejects_what_decoders_reject();
    test_payload_replaces_the_slot_table();
    test_stale_and_malformed_payloads_keep_state();
    test_reset_forgets_slots_and_generation();
    toolkit_deinit();
    return EXIT_SUCCESS;
}
