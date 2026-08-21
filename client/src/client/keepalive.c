/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 The Atrinik Project                              *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *************************************************************************/

/** @file Client keepalive request accounting. */

#include <keepalive.h>

#include <string.h>

static void counter_increment(uint64_t *counter) {
    if (*counter != UINT64_MAX) {
        (*counter)++;
    }
}

static void counter_add(uint64_t *counter, uint64_t value) {
    if (UINT64_MAX - *counter < value) {
        *counter = UINT64_MAX;
    } else {
        *counter += value;
    }
}

/**
 * Calculate elapsed time while accepting one uint64_t counter wrap.
 *
 * A difference larger than half the counter range is a backwards clock step,
 * not a plausible keepalive interval. This keeps testable wraparound support
 * without turning a clock regression into a huge RTT or timeout.
 */
static bool elapsed_us(uint64_t now_us, uint64_t started_us, uint64_t *elapsed) {
    uint64_t difference = now_us - started_us;
    if (difference > UINT64_MAX / 2) {
        return false;
    }

    *elapsed = difference;
    return true;
}

static client_keepalive_record_t *record_find(client_keepalive_state_t *state, uint32_t id) {
    for (size_t i = 0; i < arraysize(state->records); i++) {
        if (state->records[i].state != CLIENT_KEEPALIVE_REQUEST_EMPTY &&
            state->records[i].id == id) {
            return &state->records[i];
        }
    }

    return NULL;
}

static client_keepalive_record_t *record_select_slot(client_keepalive_state_t *state) {
    client_keepalive_record_t *oldest = NULL;

    for (size_t i = 0; i < arraysize(state->records); i++) {
        client_keepalive_record_t *record = &state->records[i];
        if (record->state == CLIENT_KEEPALIVE_REQUEST_EMPTY) {
            return record;
        }

        if (record->state == CLIENT_KEEPALIVE_REQUEST_PENDING) {
            continue;
        }

        if (oldest == NULL || record->sequence < oldest->sequence) {
            oldest = record;
        }
    }

    return oldest;
}

static bool record_next_id(client_keepalive_state_t *state, uint32_t *id) {
    uint32_t candidate = state->next_id;

    for (uint64_t attempt = 0; attempt <= UINT32_MAX; attempt++) {
        candidate++;
        if (candidate == 0) {
            candidate = 1;
        }

        if (record_find(state, candidate) == NULL) {
            state->next_id = candidate;
            *id = candidate;
            return true;
        }
    }

    return false;
}

static void record_sequence_next(client_keepalive_state_t *state,
                                 client_keepalive_record_t *record) {
    state->next_sequence++;
    if (state->next_sequence == 0) {
        state->next_sequence = 1;
    }
    record->sequence = state->next_sequence;
}

void client_keepalive_reset(client_keepalive_state_t *state) {
    memset(state, 0, sizeof(*state));
}

bool client_keepalive_start(client_keepalive_state_t *state, uint64_t now_us, uint32_t *id) {
    client_keepalive_record_t *record = record_select_slot(state);
    if (record == NULL || !record_next_id(state, id)) {
        return false;
    }

    record->id = *id;
    record->started_us = now_us;
    record->state = CLIENT_KEEPALIVE_REQUEST_PENDING;
    record_sequence_next(state, record);
    state->pending++;
    counter_increment(&state->statistics.tx);
    return true;
}

void client_keepalive_expire(client_keepalive_state_t *state, uint64_t now_us) {
    for (size_t i = 0; i < arraysize(state->records); i++) {
        client_keepalive_record_t *record = &state->records[i];
        uint64_t elapsed;

        if (record->state != CLIENT_KEEPALIVE_REQUEST_PENDING ||
            !elapsed_us(now_us, record->started_us, &elapsed) ||
            elapsed < CLIENT_KEEPALIVE_TIMEOUT_US) {
            continue;
        }

        record->state = CLIENT_KEEPALIVE_REQUEST_TIMED_OUT;
        record_sequence_next(state, record);
        state->pending--;
        counter_increment(&state->statistics.timed_out);
    }
}

client_keepalive_response_result_t client_keepalive_receive(client_keepalive_state_t *state,
                                                            uint32_t id,
                                                            uint64_t now_us,
                                                            uint64_t *rtt_us) {
    /* Classify a delayed reply even when the caller did not run the periodic
     * expiry pass immediately before dispatching it. */
    client_keepalive_expire(state, now_us);
    client_keepalive_record_t *record = record_find(state, id);
    if (rtt_us != NULL) {
        *rtt_us = 0;
    }

    if (record == NULL) {
        counter_increment(&state->statistics.unknown);
        return CLIENT_KEEPALIVE_RESPONSE_UNKNOWN;
    }

    if (record->state == CLIENT_KEEPALIVE_REQUEST_PENDING) {
        uint64_t elapsed;
        if (!elapsed_us(now_us, record->started_us, &elapsed)) {
            counter_increment(&state->statistics.clock_regressions);
            return CLIENT_KEEPALIVE_RESPONSE_CLOCK_REGRESSION;
        }

        record->state = CLIENT_KEEPALIVE_REQUEST_MATCHED;
        record_sequence_next(state, record);
        state->pending--;
        counter_increment(&state->statistics.rx);
        counter_increment(&state->statistics.responses);
        counter_add(&state->statistics.total_rtt_us, elapsed);
        state->statistics.last_rtt_us = elapsed;
        if (rtt_us != NULL) {
            *rtt_us = elapsed;
        }
        return CLIENT_KEEPALIVE_RESPONSE_MATCHED;
    }

    if (record->state == CLIENT_KEEPALIVE_REQUEST_TIMED_OUT) {
        record->state = CLIENT_KEEPALIVE_REQUEST_LATE;
        record_sequence_next(state, record);
        counter_increment(&state->statistics.late);
        counter_increment(&state->statistics.responses);
        return CLIENT_KEEPALIVE_RESPONSE_LATE;
    }

    counter_increment(&state->statistics.duplicate);
    return CLIENT_KEEPALIVE_RESPONSE_DUPLICATE;
}

void client_keepalive_statistics(const client_keepalive_state_t *state,
                                 client_keepalive_statistics_t *statistics) {
    *statistics = state->statistics;
    statistics->pending = state->pending;
}

uint64_t client_keepalive_average_us(const client_keepalive_statistics_t *statistics) {
    if (statistics->rx == 0) {
        return 0;
    }

    return statistics->total_rtt_us / statistics->rx;
}
