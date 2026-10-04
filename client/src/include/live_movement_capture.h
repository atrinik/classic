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

#ifndef LIVE_MOVEMENT_CAPTURE_H
#define LIVE_MOVEMENT_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct live_movement_capture live_movement_capture_t;

typedef enum live_movement_capture_status {
    LIVE_MOVEMENT_CAPTURE_READY,
    LIVE_MOVEMENT_CAPTURE_PENDING,
    LIVE_MOVEMENT_CAPTURE_COMPLETE,
    LIVE_MOVEMENT_CAPTURE_FAILED,
} live_movement_capture_status_t;

typedef struct live_movement_capture_result {
    live_movement_capture_status_t status;
    const char *path;
    uint32_t width;
    uint32_t height;
    size_t size_bytes;
    char sha256[65];
    char error[160];
} live_movement_capture_result_t;

/** Create an exclusive output file for one absolute diagnostic PNG path. */
live_movement_capture_t *
live_movement_capture_create(const char *absolute_path, char *error, size_t error_size);

/** Queue one full-frame asynchronous GPU readback. Main thread only. */
bool live_movement_capture_request(live_movement_capture_t *capture);

/** Stable result storage owned by capture. */
const live_movement_capture_result_t *
live_movement_capture_result(const live_movement_capture_t *capture);

/**
 * Release a capture. Pending GPU ownership is canceled logically and freed
 * later by its completion or cancellation callback. Main thread only.
 */
void live_movement_capture_destroy(live_movement_capture_t *capture);

#ifdef ATRINIK_LIVE_MOVEMENT_CAPTURE_TESTING
/** Close the backing descriptor to exercise retained partial-file failures. */
bool live_movement_capture_test_close_descriptor(live_movement_capture_t *capture);
#endif

#endif
