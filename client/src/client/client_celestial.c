/*************************************************************************
 * Atrinik client protocol-1085 celestial inputs.                        *
 *                                                                       *
 * Copyright 2026 The Atrinik Project                                   *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <client_celestial.h>

#include <string.h>

static struct {
    uint64_t assigned[(CELESTIAL_PROTOCOL_SLOTS_MAX + 63) / 64];
    uint64_t start_seconds;
    uint64_t revision;
    uint32_t generation;
    celestial_model_t current[CELESTIAL_PROTOCOL_SLOTS_MAX];
    celestial_model_t next[CELESTIAL_PROTOCOL_SLOTS_MAX];
} client_celestial;

static bool client_celestial_assigned(uint8_t slot) {
    return slot < CELESTIAL_PROTOCOL_SLOTS_MAX &&
           ((client_celestial.assigned[slot / 64] >> (slot % 64)) & 1U);
}

void client_celestial_reset(void) {
    uint64_t revision = client_celestial.revision;
    memset(&client_celestial, 0, sizeof(client_celestial));
    client_celestial.revision = revision + 1;
}

bool client_celestial_apply(const uint8_t *data, size_t len) {
    static celestial_protocol_slot_t slots[CELESTIAL_PROTOCOL_SLOTS_MAX];
    uint32_t generation;
    uint64_t start_seconds;
    size_t count;
    if (!celestial_protocol_read(data, len, &generation, &start_seconds, slots, &count) ||
        generation <= client_celestial.generation) {
        return false;
    }
    /* The payload is complete state: slots it omits are no longer shown. */
    memset(client_celestial.assigned, 0, sizeof(client_celestial.assigned));
    for (size_t i = 0; i < count; i++) {
        uint8_t slot = slots[i].slot;
        celestial_model_evaluate(&slots[i].current, &client_celestial.current[slot]);
        celestial_model_evaluate(&slots[i].next, &client_celestial.next[slot]);
        client_celestial.assigned[slot / 64] |= UINT64_C(1) << (slot % 64);
    }
    client_celestial.start_seconds = start_seconds;
    client_celestial.generation = generation;
    client_celestial.revision++;
    return true;
}

bool client_celestial_model(uint8_t slot, bool next, celestial_model_t *model) {
    if (!client_celestial_assigned(slot)) {
        return false;
    }
    *model = next ? client_celestial.next[slot] : client_celestial.current[slot];
    return true;
}

uint64_t client_celestial_start_seconds(void) {
    return client_celestial.start_seconds;
}

uint32_t client_celestial_generation(void) {
    return client_celestial.generation;
}

uint64_t client_celestial_revision(void) {
    return client_celestial.revision;
}
