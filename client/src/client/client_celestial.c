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

#include <stdlib.h>
#include <string.h>

#include <toolkit/datetime.h>

/** Region inputs of one CELESTIAL payload, with the sources derived from them. */
typedef struct client_celestial_slots {
    uint64_t assigned[(CELESTIAL_PROTOCOL_SLOTS_MAX + 63) / 64];
    uint64_t start_seconds;
    uint32_t generation;
    celestial_model_t current[CELESTIAL_PROTOCOL_SLOTS_MAX];
    celestial_model_t next[CELESTIAL_PROTOCOL_SLOTS_MAX];
} client_celestial_slots_t;

static struct {
    /** The published pair. */
    client_celestial_slots_t slots;
    celestial_closure_t closure;
    uint64_t revision;
    /** The staged payloads of the next publication. */
    client_celestial_slots_t staged_slots;
    celestial_closure_t staged_closure;
    bool staged_slots_valid, staged_closure_valid;
    /** Fragments of the published closure still to solve, and their receiver. */
    size_t solved;
    client_celestial_cell_fn cell;
    void *context;
} client_celestial;

static bool client_celestial_assigned(const client_celestial_slots_t *slots, uint8_t slot) {
    return slot < CELESTIAL_PROTOCOL_SLOTS_MAX &&
           ((slots->assigned[slot / 64] >> (slot % 64)) & 1U);
}

void client_celestial_discard_staged(void) {
    client_celestial.staged_slots_valid = false;
    celestial_closure_free(&client_celestial.staged_closure);
    client_celestial.staged_closure_valid = false;
}

void client_celestial_cancel(void) {
    client_celestial.solved = client_celestial.closure.count;
}

void client_celestial_reset(void) {
    uint64_t revision = client_celestial.revision;
    client_celestial_discard_staged();
    celestial_closure_free(&client_celestial.closure);
    memset(&client_celestial, 0, sizeof(client_celestial));
    client_celestial.revision = revision + 1;
}

bool client_celestial_apply(const uint8_t *data, size_t len) {
    static celestial_protocol_slot_t slots[CELESTIAL_PROTOCOL_SLOTS_MAX];
    uint32_t generation;
    uint64_t start_seconds;
    size_t count;
    if (!celestial_protocol_read(data, len, &generation, &start_seconds, slots, &count) ||
        generation <= client_celestial.slots.generation) {
        return false;
    }
    /* The payload is complete state: slots it omits are no longer shown. */
    client_celestial_slots_t *staged = &client_celestial.staged_slots;
    memset(staged->assigned, 0, sizeof(staged->assigned));
    for (size_t i = 0; i < count; i++) {
        uint8_t slot = slots[i].slot;
        celestial_model_evaluate(&slots[i].current, &staged->current[slot]);
        celestial_model_evaluate(&slots[i].next, &staged->next[slot]);
        staged->assigned[slot / 64] |= UINT64_C(1) << (slot % 64);
    }
    staged->start_seconds = start_seconds;
    staged->generation = generation;
    client_celestial.staged_slots_valid = true;
    return true;
}

bool client_celestial_apply_structure(const uint8_t *data, size_t len, int window_width,
                                      int window_height) {
    celestial_closure_t closure;
    if (!celestial_closure_read(data, len, window_width, window_height, &closure)) {
        return false;
    }
    if (closure.generation <= client_celestial.slots.generation) {
        celestial_closure_free(&closure);
        return false;
    }
    celestial_closure_free(&client_celestial.staged_closure);
    client_celestial.staged_closure = closure;
    client_celestial.staged_closure_valid = true;
    return true;
}

/** Solve one fragment's cells under one slot's model, or zero them. */
static bool client_celestial_solve(size_t fragment, bool next, int32_t *values,
                                   int32_t (*rgb)[3]) {
    const celestial_closure_fragment_t *solved = &client_celestial.closure.fragments[fragment];
    size_t cells = (size_t)solved->width * solved->height;
    if (!client_celestial_assigned(&client_celestial.slots, solved->slot)) {
        /* No profile: celestial light fails closed, as on the server. */
        memset(values, 0, cells * sizeof(*values));
        memset(rgb, 0, cells * sizeof(*rgb));
        return true;
    }
    const celestial_model_t *model = next ? &client_celestial.slots.next[solved->slot]
                                          : &client_celestial.slots.current[solved->slot];
    return celestial_closure_solve(&client_celestial.closure, fragment, model, values, rgb);
}

bool client_celestial_publish(client_celestial_cell_fn cell, void *context) {
    if (!client_celestial.staged_slots_valid || !client_celestial.staged_closure_valid ||
        client_celestial.staged_slots.generation !=
            client_celestial.staged_closure.generation) {
        return false;
    }
    client_celestial.slots = client_celestial.staged_slots;
    celestial_closure_free(&client_celestial.closure);
    client_celestial.closure = client_celestial.staged_closure;
    memset(&client_celestial.staged_closure, 0, sizeof(client_celestial.staged_closure));
    client_celestial.staged_slots_valid = false;
    client_celestial.staged_closure_valid = false;
    client_celestial.revision++;

    /* A newer publication supersedes any fragment still to solve. */
    client_celestial.solved = 0;
    client_celestial.cell = cell;
    client_celestial.context = context;
    return true;
}

/** Solve one fragment for both hours and report its closure cells. */
static bool client_celestial_solve_fragment(size_t i) {
    const celestial_closure_fragment_t *fragment = &client_celestial.closure.fragments[i];
    size_t cells = (size_t)fragment->width * fragment->height;
    int32_t *values = malloc(2 * cells * sizeof(*values));
    int32_t (*rgb)[3] = malloc(2 * cells * sizeof(*rgb));
    bool solved = values != NULL && rgb != NULL && client_celestial_solve(i, false, values, rgb) &&
                  client_celestial_solve(i, true, values + cells, rgb + cells);
    for (int y = 0; solved && y < fragment->height; y++) {
        for (int x = 0; x < fragment->width; x++) {
            size_t index = (size_t)x + (size_t)fragment->width * y;
            if (fragment->values[index] == 0) {
                continue;
            }
            int32_t current[4] = {values[index], rgb[index][0], rgb[index][1], rgb[index][2]};
            int32_t following[4] = {values[cells + index], rgb[cells + index][0],
                                    rgb[cells + index][1], rgb[cells + index][2]};
            client_celestial.cell(client_celestial.context, fragment->depth, fragment->x + x,
                                  fragment->y + y, current, following);
        }
    }
    free(values);
    free(rgb);
    return solved;
}

bool client_celestial_process(uint64_t budget_us) {
    uint64_t start = datetime_monotonic_us();
    while (client_celestial.solved < client_celestial.closure.count) {
        if (client_celestial.cell == NULL) {
            client_celestial.solved = client_celestial.closure.count;
            break;
        }
        if (!client_celestial_solve_fragment(client_celestial.solved)) {
            /* Out of memory: the cells keep the field they last had. */
            client_celestial.solved = client_celestial.closure.count;
            break;
        }
        client_celestial.solved++;
        if (datetime_monotonic_us() - start >= budget_us) {
            break;
        }
    }
    return client_celestial.solved == client_celestial.closure.count;
}

bool client_celestial_model(uint8_t slot, bool next, celestial_model_t *model) {
    if (!client_celestial_assigned(&client_celestial.slots, slot)) {
        return false;
    }
    *model = next ? client_celestial.slots.next[slot] : client_celestial.slots.current[slot];
    return true;
}

const celestial_closure_t *client_celestial_closure(void) {
    return &client_celestial.closure;
}

uint64_t client_celestial_start_seconds(void) {
    return client_celestial.slots.start_seconds;
}

uint32_t client_celestial_generation(void) {
    return client_celestial.slots.generation;
}

uint64_t client_celestial_revision(void) {
    return client_celestial.revision;
}
