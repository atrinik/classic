/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LIVE_MOVEMENT_H
#define LIVE_MOVEMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <keepalive.h>
#include <gpu_renderer.h>

/** Optional, main-thread-only live route adapter. No effect unless configured. */
bool live_movement_initialize(const char *route_path, const char *report_path);
/** Optional fixed lighting setup and paired diagnostic PNGs in the report directory. */
bool live_movement_configure_review(const char *report_path,
                                    const char *initial_path,
                                    const char *final_path,
                                    const char *lighting_phase);
void live_movement_ready(void);
bool live_movement_enabled(void);
void live_movement_tick(void);
void live_movement_frame_finished(bool presented, const client_keepalive_statistics_t *keepalive);
void live_movement_abort(const char *reason);
/** Privately retain one malformed MAP envelope when live diagnostics are active. */
void live_movement_rejected_map(const uint8_t *data, size_t len, size_t cursor_offset);
/** Allow initial renderer setup only before any diagnostic frame or world publication. */
bool live_movement_renderer_recovery(const char *context,
                                     const gpu_renderer_recreation_diagnostic_t *diagnostic,
                                     const char *error_snapshot,
                                     uint64_t window_flags,
                                     int width,
                                     int height);
bool live_movement_finished(void);
int live_movement_exit_status(void);
void live_movement_close(void);

#endif
