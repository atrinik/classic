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

/**
 * @file
 * Client keepalive request accounting.
 */

#ifndef CLIENT_KEEPALIVE_H
#define CLIENT_KEEPALIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <toolkit/socket.h>

/** Keep completed requests long enough to classify late and duplicate replies. */
#define CLIENT_KEEPALIVE_RECORD_CAPACITY 64

/**
 * A keepalive is timed out at the transport's configured idle timeout.
 *
 * This is an application-level deadline, not a QUIC transport timeout: the
 * displayed application RTT includes client queueing, server scheduling and
 * processing, transport, and client dispatch.
 */
#define CLIENT_KEEPALIVE_TIMEOUT_US \
    ((uint64_t)SOCKET_QUIC_IDLE_TIMEOUT_MS * UINT64_C(1000))

typedef enum client_keepalive_request_state {
    CLIENT_KEEPALIVE_REQUEST_EMPTY,
    CLIENT_KEEPALIVE_REQUEST_PENDING,
    CLIENT_KEEPALIVE_REQUEST_MATCHED,
    CLIENT_KEEPALIVE_REQUEST_TIMED_OUT,
    CLIENT_KEEPALIVE_REQUEST_LATE,
} client_keepalive_request_state_t;

typedef enum client_keepalive_response_result {
    CLIENT_KEEPALIVE_RESPONSE_MATCHED,
    CLIENT_KEEPALIVE_RESPONSE_LATE,
    CLIENT_KEEPALIVE_RESPONSE_DUPLICATE,
    CLIENT_KEEPALIVE_RESPONSE_UNKNOWN,
    CLIENT_KEEPALIVE_RESPONSE_CLOCK_REGRESSION,
} client_keepalive_response_result_t;

typedef struct client_keepalive_record {
    uint32_t id;
    uint64_t started_us;
    uint64_t sequence;
    client_keepalive_request_state_t state;
} client_keepalive_record_t;

typedef struct client_keepalive_statistics {
    uint64_t tx;
    /** Replies received before the explicit timeout deadline. */
    uint64_t rx;
    /** All recognized replies, including late replies. */
    uint64_t responses;
    uint64_t timed_out;
    uint64_t late;
    uint64_t duplicate;
    uint64_t unknown;
    uint64_t clock_regressions;
    uint64_t total_rtt_us;
    uint64_t last_rtt_us;
    size_t pending;
} client_keepalive_statistics_t;

typedef struct client_keepalive_state {
    uint32_t next_id;
    uint64_t next_sequence;
    size_t pending;
    client_keepalive_record_t records[CLIENT_KEEPALIVE_RECORD_CAPACITY];
    client_keepalive_statistics_t statistics;
} client_keepalive_state_t;

/** Reset all request history and counters for a new client connection. */
void client_keepalive_reset(client_keepalive_state_t *state);

/** Record a transmitted keepalive and allocate its wire identifier. */
bool client_keepalive_start(client_keepalive_state_t *state,
                            uint64_t now_us,
                            uint32_t *id);

/** Mark requests whose explicit application deadline has elapsed. */
void client_keepalive_expire(client_keepalive_state_t *state, uint64_t now_us);

/** Classify a response and, for an on-time response, return its RTT. */
client_keepalive_response_result_t client_keepalive_receive(client_keepalive_state_t *state,
                                                            uint32_t id,
                                                            uint64_t now_us,
                                                            uint64_t *rtt_us);

/** Copy a stable statistics snapshot for display or tests. */
void client_keepalive_statistics(const client_keepalive_state_t *state,
                                 client_keepalive_statistics_t *statistics);

/** Return the exact integer average of on-time RTT samples in microseconds. */
uint64_t client_keepalive_average_us(const client_keepalive_statistics_t *statistics);

#endif
