/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#ifndef CLIENT_COMMAND_QUEUE_H
#define CLIENT_COMMAND_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct command_buffer command_buffer;

/** Production main-loop budget for one inbound command drain. */
#define CLIENT_COMMAND_QUEUE_BUDGET_US UINT64_C(4000)

/** Injectable monotonic clock used by deterministic offline replays. */
typedef uint64_t (*client_command_queue_clock_func)(void *user_data);

/**
 * Handler invoked once for each complete server command envelope.
 * Return false to preserve the remaining queue for a later drain.
 */
typedef bool (*client_command_queue_dispatch_func)(uint8_t *data, size_t len, void *user_data);

/** Result of one bounded queue drain. */
typedef struct client_command_queue_drain_result {
    uint64_t commands;
    uint64_t bytes;
    uint64_t processing_us;
    bool budget_due;
} client_command_queue_drain_result_t;

/** Cumulative queue telemetry since the last successful statistics reset. */
typedef struct client_command_queue_statistics {
    uint64_t enqueued;
    uint64_t dequeued;
    uint64_t budget_yields;
    uint64_t recoveries;
    uint64_t depth;
    uint64_t bytes;
    uint64_t peak_depth;
    uint64_t peak_bytes;
    uint64_t oldest_age_us;
    uint64_t current_oldest_age_us;
    uint64_t processing_us;
    /** FNV-1a digest of length-framed envelopes in enqueue call order. */
    uint64_t enqueued_order_digest;
    /** FNV-1a digest of length-framed envelopes in dequeue order. */
    uint64_t dequeued_order_digest;
    /**
     * Whether the two streaming digests describe directly comparable order.
     * Nested compressed commands deliberately prepend their expanded envelope
     * after later network envelopes may already have been admitted.
     */
    bool order_digests_comparable;
    bool due;
    bool budget_due;
} client_command_queue_statistics_t;

/**
 * Create the queue mutex around the single global inbound queue.
 *
 * Returns false when SDL cannot allocate the mutex. Calls are idempotent and
 * the owner must prevent initialization racing with itself. Once initialized,
 * enqueue, drain, clear, and statistics calls serialize through that mutex.
 * Deinitialization requires every producer and consumer to have stopped.
 */
bool client_command_queue_initialize(void);
void client_command_queue_deinitialize(void);

/** Allocate or free one copied command envelope. */
command_buffer *command_buffer_new(size_t len, uint8_t *data);
void command_buffer_free(command_buffer *buf);

/**
 * Append an owned command buffer using a supplied arrival timestamp.
 *
 * Ownership transfers only on success. The timestamp and every clock supplied
 * to drain/statistics calls must use the same monotonic epoch.
 */
bool client_command_queue_enqueue_buffer_at(command_buffer *buf, uint64_t arrival_us);

/** Copy and append a complete server command envelope for an offline replay. */
bool client_command_queue_enqueue_envelope_at(const uint8_t *data, size_t len, uint64_t arrival_us);

/** Compatibility APIs used by nested compressed commands. */
command_buffer *get_next_input_command(void);
void add_input_command(command_buffer *buf);

/** Drop every pending inbound command without counting it as a recovery. */
void client_command_queue_clear(void);

/**
 * Drain in arrival order, stopping after the first command that reaches the
 * budget. A zero budget drains the entire queue. Commands are freed after the
 * callback returns, so the callback must not retain data pointers.
 */
void client_command_queue_drain(uint64_t budget_us,
                                client_command_queue_clock_func clock_func,
                                void *clock_data,
                                client_command_queue_dispatch_func dispatch_func,
                                void *dispatch_data,
                                client_command_queue_drain_result_t *result);

/** Reset cumulative telemetry only when the queue is empty. */
bool client_command_queue_statistics_reset(void);

/** Snapshot cumulative and current telemetry at the supplied monotonic time. */
void client_command_queue_statistics_get(uint64_t now_us,
                                         client_command_queue_statistics_t *statistics);

#endif
