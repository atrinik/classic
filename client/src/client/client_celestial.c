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
#include <toolkit/packet.h>

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
    /** The fragment being solved keeps its current hour here while its next
     * hour is solved. */
    int32_t *partial_values;
    int32_t (*partial_rgb)[3];
    /** The hour being solved, one bounded solver step per call, and its
     * outputs; the job reads the published closure. */
    celestial_closure_job_t *job;
    int32_t *job_values;
    int32_t (*job_rgb)[3];
    /** Next row of a fragment solved for both hours to report: one row per
     * step, so reporting stays within the budget too. */
    int report_row;
    /** A structure payload could not be decoded for lack of memory. */
    bool reconnect;
} client_celestial;

/** Drop the hour being solved. */
static void client_celestial_job_free(void) {
    celestial_closure_solve_free(client_celestial.job);
    free(client_celestial.job_values);
    free(client_celestial.job_rgb);
    client_celestial.job = NULL;
    client_celestial.job_values = NULL;
    client_celestial.job_rgb = NULL;
}

/** Drop a fragment's partial solve: its held current hour and the hour being
 * solved. */
static void client_celestial_partial_free(void) {
    client_celestial_job_free();
    free(client_celestial.partial_values);
    free(client_celestial.partial_rgb);
    client_celestial.partial_values = NULL;
    client_celestial.partial_rgb = NULL;
    client_celestial.report_row = 0;
}

static bool client_celestial_assigned(const client_celestial_slots_t *slots, uint8_t slot) {
    return slot < CELESTIAL_PROTOCOL_SLOTS_MAX &&
           ((slots->assigned[slot / 64] >> (slot % 64)) & 1U);
}

void client_celestial_abandon_staged(void) {
    /* A staged half belonged to the failed update; the server will not send
     * that publication again. */
    if (client_celestial.staged_slots_valid || client_celestial.staged_closure_valid) {
        client_celestial.reconnect = true;
    }
    client_celestial_discard_staged();
}

void client_celestial_discard_staged(void) {
    client_celestial.staged_slots_valid = false;
    celestial_closure_free(&client_celestial.staged_closure);
    client_celestial.staged_closure_valid = false;
}

void client_celestial_cancel(void) {
    client_celestial_partial_free();
    client_celestial.solved = client_celestial.closure.count;
}

void client_celestial_reset(void) {
    uint64_t revision = client_celestial.revision;
    client_celestial_partial_free();
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
    /* Publications start at generation 1, so 0 is malformed, not stale. */
    if (!celestial_protocol_read(data, len, &generation, &start_seconds, slots, &count) ||
        generation == 0) {
        /* A malformed half voids the pair, and the server, which considers
         * it delivered, would never resend it: the connection is reset. */
        client_celestial.reconnect = true;
        client_celestial_discard_staged();
        return false;
    }
    /* Generations never regress, against the published pair or a payload
     * already staged: one generation is one atomic publication, so a
     * duplicate never replaces the payload staged first. */
    if (generation <= client_celestial.slots.generation ||
        (client_celestial.staged_slots_valid &&
         generation <= client_celestial.staged_slots.generation)) {
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
    bool out_of_memory;
    if (!celestial_closure_decode(data, len, window_width, window_height, &closure,
                                  &out_of_memory)) {
        /* Malformed or out of memory, the server considers the pair
         * delivered and will not resend it: the caller reconnects rather
         * than present a stale closure or abort on bad input. */
        (void)out_of_memory;
        client_celestial.reconnect = true;
        client_celestial_discard_staged();
        return false;
    }
    if (closure.generation == 0) {
        /* Publications start at generation 1: malformed, not stale. */
        celestial_closure_free(&closure);
        client_celestial.reconnect = true;
        client_celestial_discard_staged();
        return false;
    }
    if (closure.generation <= client_celestial.slots.generation ||
        (client_celestial.staged_closure_valid &&
         closure.generation <= client_celestial.staged_closure.generation)) {
        celestial_closure_free(&closure);
        return false;
    }
    celestial_closure_free(&client_celestial.staged_closure);
    client_celestial.staged_closure = closure;
    client_celestial.staged_closure_valid = true;
    return true;
}

/** Solve one fragment's cells under one slot's model, or zero them. */
/** Whether both halves are staged with one generation, and every fragment
 * names a slot of its paired CELESTIAL (only slot 255 means no profile). */
static bool client_celestial_staged_pair(void) {
    if (!client_celestial.staged_slots_valid || !client_celestial.staged_closure_valid ||
        client_celestial.staged_slots.generation !=
            client_celestial.staged_closure.generation) {
        return false;
    }
    for (size_t i = 0; i < client_celestial.staged_closure.count; i++) {
        uint8_t slot = client_celestial.staged_closure.fragments[i].slot;
        if (slot != CELESTIAL_PROTOCOL_SLOT_NONE &&
            !client_celestial_assigned(&client_celestial.staged_slots, slot)) {
            return false;
        }
    }
    return true;
}

bool client_celestial_staged(void) {
    return client_celestial.staged_slots_valid || client_celestial.staged_closure_valid;
}

bool client_celestial_publishable(void) {
    /* A connection's first MAP2 update always comes with its pair: before
     * any publication, an update without one would present a base that
     * excludes celestial light with no celestial field at all. */
    if (!client_celestial_staged()) {
        return client_celestial.slots.generation != 0;
    }
    return client_celestial_staged_pair();
}

void client_celestial_reject_staged(void) {
    client_celestial.reconnect = true;
    client_celestial_discard_staged();
}

bool client_celestial_stage_empty(int window_width, int window_height) {
    /* Through the real codecs, as a server with nothing celestial in view
     * would send it: no profiles and no closure cells. */
    uint32_t generation = MAX(client_celestial.slots.generation,
                              MAX(client_celestial.staged_slots_valid
                                      ? client_celestial.staged_slots.generation
                                      : 0,
                                  client_celestial.staged_closure_valid
                                      ? client_celestial.staged_closure.generation
                                      : 0));
    if (generation == UINT32_MAX) {
        return false;
    }
    generation++;
    packet_struct *packet = packet_new(0, 32, 0);
    bool staged = celestial_protocol_write(packet, generation, 0, NULL, 0) &&
                  client_celestial_apply(packet->data, packet->len);
    packet_free(packet);
    celestial_closure_t closure;
    if (!staged ||
        !celestial_closure_build(&closure, generation, window_width, window_height, NULL, 0)) {
        return false;
    }
    packet = packet_new(0, 32, 0);
    staged = celestial_closure_write(packet, &closure) &&
             client_celestial_apply_structure(packet->data, packet->len, window_width,
                                              window_height);
    packet_free(packet);
    celestial_closure_free(&closure);
    return staged;
}

bool client_celestial_publish(client_celestial_cell_fn cell, void *context) {
    if (!client_celestial_staged_pair()) {
        /* Each half belongs to the MAP update right after it: one left
         * unmatched by that update can never pair with a later one, and a
         * fragment naming an omitted slot makes the pair malformed rather
         * than darkness no later duplicate could repair. The server sends
         * both halves together and considers them delivered, so a staged
         * half lost here means the caller reconnects. An update with
         * nothing staged is ordinary. */
        if (client_celestial.staged_slots_valid || client_celestial.staged_closure_valid) {
            client_celestial.reconnect = true;
        }
        client_celestial_discard_staged();
        return false;
    }
    /* A newer publication supersedes any fragment still to solve; its job
     * reads the closure, so it goes first. */
    client_celestial_partial_free();
    client_celestial.slots = client_celestial.staged_slots;
    celestial_closure_free(&client_celestial.closure);
    client_celestial.closure = client_celestial.staged_closure;
    memset(&client_celestial.staged_closure, 0, sizeof(client_celestial.staged_closure));
    client_celestial.staged_slots_valid = false;
    client_celestial.staged_closure_valid = false;
    client_celestial.revision++;

    client_celestial.solved = 0;
    client_celestial.cell = cell;
    client_celestial.context = context;
    return true;
}

/**
 * Take one bounded step of solving fragment i: start an hour, or run one
 * solver step of it (a row of rays, spill coefficients or a relaxation
 * pass, or of the publish). Its
 * current hour is held once solved; once its next hour is too, its closure
 * cells are reported one row per step.
 * @return False when an allocation fails; the hour is retried later.
 */
static bool client_celestial_solve_step(size_t i, bool *done) {
    const celestial_closure_fragment_t *fragment = &client_celestial.closure.fragments[i];
    size_t cells = (size_t)fragment->width * fragment->height;
    bool next = client_celestial.partial_values != NULL;
    *done = false;
    if (client_celestial.job_values == NULL) {
        client_celestial.job_values = malloc(cells * sizeof(*client_celestial.job_values));
        client_celestial.job_rgb = malloc(cells * sizeof(*client_celestial.job_rgb));
        if (client_celestial.job_values == NULL || client_celestial.job_rgb == NULL) {
            client_celestial_job_free();
            return false;
        }
        if (!client_celestial_assigned(&client_celestial.slots, fragment->slot)) {
            /* No profile: celestial light fails closed, as on the server. */
            memset(client_celestial.job_values, 0, cells * sizeof(*client_celestial.job_values));
            memset(client_celestial.job_rgb, 0, cells * sizeof(*client_celestial.job_rgb));
        } else {
            const celestial_model_t *model = next
                                                 ? &client_celestial.slots.next[fragment->slot]
                                                 : &client_celestial.slots.current[fragment->slot];
            client_celestial.job = celestial_closure_solve_begin(
                &client_celestial.closure, i, model, client_celestial.job_values,
                client_celestial.job_rgb);
            if (client_celestial.job == NULL) {
                client_celestial_job_free();
                return false;
            }
            return true;
        }
    }
    if (client_celestial.job != NULL) {
        celestial_solver_status_t status = celestial_closure_solve_step(client_celestial.job);
        if (status == CELESTIAL_SOLVER_FAILED) {
            client_celestial_job_free();
            return false;
        }
        if (status == CELESTIAL_SOLVER_PENDING) {
            return true;
        }
        celestial_closure_solve_free(client_celestial.job);
        client_celestial.job = NULL;
    }
    if (!next) {
        client_celestial.partial_values = client_celestial.job_values;
        client_celestial.partial_rgb = client_celestial.job_rgb;
        client_celestial.job_values = NULL;
        client_celestial.job_rgb = NULL;
        return true;
    }
    const int32_t *values = client_celestial.partial_values;
    const int32_t (*rgb)[3] = (const int32_t (*)[3])client_celestial.partial_rgb;
    const int32_t *next_values = client_celestial.job_values;
    const int32_t (*next_rgb)[3] = (const int32_t (*)[3])client_celestial.job_rgb;
    int y = client_celestial.report_row;
    for (int x = 0; x < fragment->width; x++) {
        size_t index = (size_t)x + (size_t)fragment->width * y;
        if (fragment->values[index] == 0) {
            continue;
        }
        int32_t current[4] = {values[index], rgb[index][0], rgb[index][1], rgb[index][2]};
        int32_t following[4] = {next_values[index], next_rgb[index][0], next_rgb[index][1],
                                next_rgb[index][2]};
        client_celestial.cell(client_celestial.context, fragment->depth, fragment->x + x,
                              fragment->y + y, current, following);
    }
    if (++client_celestial.report_row < fragment->height) {
        return true;
    }
    client_celestial_partial_free();
    *done = true;
    return true;
}

bool client_celestial_process(uint64_t budget_us) {
    uint64_t start = datetime_monotonic_us();
    while (client_celestial.solved < client_celestial.closure.count) {
        if (client_celestial.cell == NULL) {
            client_celestial.solved = client_celestial.closure.count;
            break;
        }
        bool done;
        if (!client_celestial_solve_step(client_celestial.solved, &done)) {
            /* Out of memory: the cells keep the field they last had, and the
             * step is retried on the next call, since an unchanged pair is
             * never sent again. */
            break;
        }
        if (done) {
            client_celestial.solved++;
        }
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

bool client_celestial_solved(void) {
    return client_celestial.solved == client_celestial.closure.count;
}

bool client_celestial_take_reconnect(void) {
    bool reconnect = client_celestial.reconnect;
    client_celestial.reconnect = false;
    return reconnect;
}

uint32_t client_celestial_generation(void) {
    return client_celestial.slots.generation;
}

uint64_t client_celestial_revision(void) {
    return client_celestial.revision;
}
