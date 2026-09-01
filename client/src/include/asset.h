/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * In-band QUIC asset download declarations.
 */

#ifndef CLIENT_ASSET_H
#define CLIENT_ASSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <toolkit/socket.h>

typedef struct asset_request asset_request_t;

typedef enum asset_request_state {
    ASSET_REQUEST_PENDING,
    ASSET_REQUEST_COMPLETE,
    ASSET_REQUEST_ERROR,
    /** Locally displaced for visible work; retry without charging an attempt. */
    ASSET_REQUEST_PREEMPTED,
} asset_request_state_t;

/** Attach the scheduler to a live connection owned by the I/O thread. */
void asset_requests_connect(socket_t *sc);

/** Publish the setup-negotiated transport capability bitmask. */
void asset_requests_set_capabilities(uint8_t capabilities);

/** Whether the negotiated in-band transport is currently available. */
bool asset_requests_available(void);

/** Whether negotiated face batching is live on the current connection. */
bool asset_face_batch_available(void);

asset_request_t *asset_request_start(const char *path);

/** Start a body request whose declared response may not exceed max_size. */
asset_request_t *asset_request_start_bounded(const char *path, size_t max_size);

/** Queue a valid, max-sized visible face ahead of speculative asset work. */
asset_request_t *asset_request_start_bounded_priority(const char *path, size_t max_size);

/** Advisory physical-capacity check; asset_request_preempt resolves its races. */
bool asset_request_preemption_needed(const asset_request_t *replacement);

asset_request_t *asset_request_start_cached(const char *path, const char *cache_path);

asset_request_t *asset_request_start_metadata(const char *path);

asset_request_state_t asset_request_get_state(asset_request_t *request);

const uint8_t *asset_request_get_data(const asset_request_t *request, size_t *size);

bool asset_request_get_metadata(const asset_request_t *request,
                                size_t *size,
                                uint8_t digest[ASSET_DIGEST_SIZE]);

void asset_request_free(asset_request_t *request);

/**
 * Prioritize `replacement` and publish an I/O-thread handoff intent for
 * `victim`'s physical stream. A speculative batched victim preempts every
 * unfinished, non-cancelled member in that batch; callers observe them as
 * ASSET_REQUEST_PREEMPTED. Priority batches reject preemption. Both handles
 * stay caller-owned. This is reserved for visible face admission, not
 * cancellation. On success, `displace_victim` tells the caller whether the
 * victim must be released; it stays false when the replacement won the stream
 * race and only a logical full-cap handoff may still require that release.
 */
bool asset_request_preempt(asset_request_t *victim,
                           asset_request_t *replacement,
                           bool *displace_victim);

bool asset_requests_service(socket_t *sc, bool *write_pending);

void asset_requests_disconnect(void);

void asset_requests_deinit(void);

#endif
