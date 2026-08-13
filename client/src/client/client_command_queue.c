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

/** @file Ordered inbound server-command queue and bounded dispatcher. */

#include <global.h>
#include <client_command_queue.h>
#include <toolkit/datetime.h>

static SDL_Mutex *queue_mutex;
static command_buffer *queue_start;
static command_buffer *queue_end;
static client_command_queue_statistics_t queue_statistics;

static uint64_t queue_order_digest_update(uint64_t digest, const command_buffer *buf) {
    if (digest == 0) {
        digest = UINT64_C(14695981039346656037);
    }
    uint64_t length = buf->len;
    for (size_t i = 0; i < sizeof(length); i++) {
        digest ^= (uint8_t)length;
        digest *= UINT64_C(1099511628211);
        length >>= 8;
    }
    for (size_t i = 0; i < buf->len; i++) {
        digest ^= buf->data[i];
        digest *= UINT64_C(1099511628211);
    }
    return digest;
}

static void queue_lock(void) {
    HARD_ASSERT(queue_mutex != NULL);
    SDL_LockMutex(queue_mutex);
}

static void queue_unlock(void) {
    HARD_ASSERT(queue_mutex != NULL);
    SDL_UnlockMutex(queue_mutex);
}

static uint64_t elapsed_us(uint64_t started, uint64_t finished) {
    return finished >= started ? finished - started : 0;
}

command_buffer *command_buffer_new(size_t len, uint8_t *data) {
    command_buffer *buf = xmalloc(sizeof(command_buffer) + len + 1);

    buf->next = buf->prev = NULL;
    buf->enqueued_us = 0;
    buf->len = len;

    if (data != NULL) {
        memcpy(buf->data, data, len);
    }

    buf->data[len] = '\0';
    return buf;
}

void command_buffer_free(command_buffer *buf) {
    free(buf);
}

static void command_buffer_enqueue(command_buffer *buf, bool first) {
    if (first) {
        buf->next = queue_start;
        buf->prev = NULL;
        if (queue_end == NULL) {
            queue_end = buf;
        }
        if (buf->next != NULL) {
            buf->next->prev = buf;
        }
        queue_start = buf;
    } else {
        buf->next = NULL;
        buf->prev = queue_end;
        if (queue_start == NULL) {
            queue_start = buf;
        }
        if (buf->prev != NULL) {
            buf->prev->next = buf;
        }
        queue_end = buf;
    }

    queue_statistics.enqueued++;
    queue_statistics.enqueued_order_digest =
        queue_order_digest_update(queue_statistics.enqueued_order_digest, buf);
    queue_statistics.depth++;
    queue_statistics.bytes += buf->len;
    queue_statistics.peak_depth = MAX(queue_statistics.peak_depth, queue_statistics.depth);
    queue_statistics.peak_bytes = MAX(queue_statistics.peak_bytes, queue_statistics.bytes);
}

static command_buffer *command_buffer_dequeue(uint64_t now_us) {
    command_buffer *buf = queue_start;
    if (buf == NULL) {
        return NULL;
    }

    queue_start = buf->next;
    if (buf->next != NULL) {
        buf->next->prev = NULL;
    } else {
        queue_end = NULL;
    }
    buf->next = buf->prev = NULL;

    queue_statistics.dequeued++;
    queue_statistics.dequeued_order_digest =
        queue_order_digest_update(queue_statistics.dequeued_order_digest, buf);
    queue_statistics.depth--;
    queue_statistics.bytes -= buf->len;
    queue_statistics.oldest_age_us =
        MAX(queue_statistics.oldest_age_us, elapsed_us(buf->enqueued_us, now_us));
    return buf;
}

bool client_command_queue_initialize(void) {
    if (queue_mutex == NULL) {
        HARD_ASSERT(queue_start == NULL && queue_end == NULL);
        queue_statistics = (client_command_queue_statistics_t){
            .order_digests_comparable = true,
        };
        queue_mutex = SDL_CreateMutex();
    }
    return queue_mutex != NULL;
}

bool client_command_queue_enqueue_buffer_at(command_buffer *buf, uint64_t arrival_us) {
    if (buf == NULL || queue_mutex == NULL) {
        return false;
    }
    buf->enqueued_us = arrival_us;
    queue_lock();
    command_buffer_enqueue(buf, false);
    queue_unlock();
    return true;
}

bool client_command_queue_enqueue_envelope_at(const uint8_t *data,
                                              size_t len,
                                              uint64_t arrival_us) {
    if (data == NULL || len == 0) {
        return false;
    }

    command_buffer *buf = command_buffer_new(len, (uint8_t *)data);
    if (!client_command_queue_enqueue_buffer_at(buf, arrival_us)) {
        command_buffer_free(buf);
        return false;
    }
    return true;
}

command_buffer *get_next_input_command(void) {
    queue_lock();
    command_buffer *buf = command_buffer_dequeue(datetime_monotonic_us());
    queue_unlock();
    return buf;
}

void add_input_command(command_buffer *buf) {
    HARD_ASSERT(buf != NULL);
    buf->enqueued_us = datetime_monotonic_us();
    queue_lock();
    /* Admission order and effective dispatch order intentionally diverge
     * when a compressed command expands ahead of envelopes already queued. */
    queue_statistics.order_digests_comparable = false;
    command_buffer_enqueue(buf, true);
    queue_unlock();
}

void client_command_queue_clear(void) {
    if (queue_mutex == NULL) {
        return;
    }
    queue_lock();
    while (queue_start != NULL) {
        command_buffer *buf = queue_start;
        queue_start = buf->next;
        command_buffer_free(buf);
    }
    queue_end = NULL;
    queue_statistics.depth = 0;
    queue_statistics.bytes = 0;
    queue_statistics.budget_due = false;
    queue_unlock();
}

void client_command_queue_deinitialize(void) {
    if (queue_mutex == NULL) {
        return;
    }
    client_command_queue_clear();
    memset(&queue_statistics, 0, sizeof(queue_statistics));
    SDL_DestroyMutex(queue_mutex);
    queue_mutex = NULL;
}

void client_command_queue_drain(uint64_t budget_us,
                                client_command_queue_clock_func clock_func,
                                void *clock_data,
                                client_command_queue_dispatch_func dispatch_func,
                                void *dispatch_data,
                                client_command_queue_drain_result_t *result) {
    client_command_queue_drain_result_t local_result = {0};
    if (clock_func == NULL || dispatch_func == NULL) {
        if (result != NULL) {
            *result = local_result;
        }
        return;
    }

    uint64_t drain_started = clock_func(clock_data);
    while (true) {
        uint64_t command_started = clock_func(clock_data);
        queue_lock();
        command_buffer *buf = command_buffer_dequeue(command_started);
        queue_unlock();
        if (buf == NULL) {
            break;
        }

        size_t len = buf->len;
        dispatch_func(buf->data, buf->len, dispatch_data);
        uint64_t command_finished = clock_func(clock_data);
        uint64_t processing_us = elapsed_us(command_started, command_finished);
        command_buffer_free(buf);

        local_result.commands++;
        local_result.bytes += len;
        local_result.processing_us += processing_us;

        queue_lock();
        queue_statistics.processing_us += processing_us;
        bool queue_due = queue_statistics.depth != 0;
        if (!queue_due && queue_statistics.budget_due) {
            queue_statistics.budget_due = false;
            queue_statistics.recoveries++;
        }
        if (budget_us != 0 && elapsed_us(drain_started, command_finished) >= budget_us &&
            queue_due) {
            queue_statistics.budget_due = true;
            queue_statistics.budget_yields++;
            local_result.budget_due = true;
        }
        queue_unlock();
        if (local_result.budget_due) {
            break;
        }
    }

    if (result != NULL) {
        *result = local_result;
    }
}

bool client_command_queue_statistics_reset(void) {
    queue_lock();
    bool empty = queue_statistics.depth == 0;
    if (empty) {
        queue_statistics = (client_command_queue_statistics_t){
            .order_digests_comparable = true,
        };
    }
    queue_unlock();
    return empty;
}

void client_command_queue_statistics_get(uint64_t now_us,
                                         client_command_queue_statistics_t *statistics) {
    HARD_ASSERT(statistics != NULL);
    queue_lock();
    *statistics = queue_statistics;
    statistics->due = queue_statistics.depth != 0;
    if (queue_start != NULL) {
        statistics->current_oldest_age_us = elapsed_us(queue_start->enqueued_us, now_us);
        statistics->oldest_age_us =
            MAX(statistics->oldest_age_us, statistics->current_oldest_age_us);
    }
    queue_unlock();
}
