/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Atrinik Development Team                         *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#ifndef FACE_LOADER_H
#define FACE_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sprite_struct sprite_struct;

typedef enum face_loader_kind {
    FACE_LOADER_LOCAL,
    FACE_LOADER_NETWORK,
} face_loader_kind_t;

/*
 * Lifecycle and queue APIs are called by the client main thread. The module's
 * private worker owns every queued input and decoded surface until a result is
 * popped; callers must stop/join it before SDL and sprite teardown.
 */

/** Immutable input copied into the bounded loader queue. */
typedef struct face_loader_request {
    uint16_t face;
    uint64_t token;
    face_loader_kind_t kind;
    bool foreground;
    bool urgent;
    const char *name;
    uint32_t checksum;
    long pack_offset;
    size_t pack_size;
    const uint8_t *data;
    size_t size;
} face_loader_request_t;

/** Completed worker result. All pointed-to storage transfers to the caller. */
typedef struct face_loader_result {
    struct face_loader_result *next;
    uint16_t face;
    uint64_t token;
    uint64_t sequence;
    face_loader_kind_t kind;
    bool foreground;
    bool urgent;
    sprite_struct *sprite;
    uint8_t *data;
    size_t size;
    char *override_name;
    uint32_t override_checksum;
    uint64_t load_us;
} face_loader_result_t;

/**
 * Start the single bounded filesystem/PNG worker.
 *
 * All paths are copied and remain immutable until face_loader_stop(). The
 * pack path may be NULL; the other directory paths must not be NULL.
 */
bool face_loader_start(const char *pack_path,
                       const char *gfx_user_path,
                       const char *gfx_current_path,
                       const char *gfx_installed_path,
                       const char *cache_path,
                       uint32_t display_format);

bool face_loader_available(void);

/** Whether a request of the given priority can enter the bounded pipeline. */
bool face_loader_can_submit(bool foreground);

/** Copy and enqueue a request. */
bool face_loader_submit(const face_loader_request_t *request);

/** Promote matching queued, active, or completed work to foreground priority. */
void face_loader_promote(uint16_t face, uint64_t token);

/** Cancel matching work without retaining pointers owned by the caller. */
void face_loader_cancel(uint16_t face, uint64_t token);

/** Pop the oldest foreground result, or a background result when permitted. */
face_loader_result_t *face_loader_result_pop(bool allow_background);

void face_loader_result_free(face_loader_result_t *result);

#ifdef ATRINIK_FACE_REQUEST_TESTING
/** Wait until at least this many decoded results are ready for deterministic tests. */
bool face_loader_test_wait_results(size_t minimum, uint32_t timeout_ms);
#endif

/** Stop/join the worker and release all queued or completed results. */
void face_loader_stop(void);

#endif
