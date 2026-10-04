/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ATRINIK_ADMIN_SHUTDOWN_H
#define ATRINIK_ADMIN_SHUTDOWN_H

#include <stdbool.h>
#include <stddef.h>

#define ADMIN_SHUTDOWN_REQUEST_MAX 1024
#define ADMIN_SHUTDOWN_REASON_MAX 192

typedef struct {
    char id[33];
    unsigned seconds;
    char reason[ADMIN_SHUTDOWN_REASON_MAX + 1];
} admin_shutdown_request;

typedef bool (*admin_shutdown_schedule_fn)(unsigned seconds, const char *reason);

/* Strict, length-aware parser; no embedded NUL, trailing command, or controls. */
bool admin_shutdown_parse(const char *data, size_t size, admin_shutdown_request *request);
/* Linux only; an empty path leaves the endpoint disabled. Game-thread owned. */
bool admin_shutdown_init(const char *path, admin_shutdown_schedule_fn schedule);
void admin_shutdown_poll(void);
void admin_shutdown_deinit(void);
/* Timer changes cancel a pending updater request. Expiry authorizes completion. */
bool admin_shutdown_cancel(void);
void admin_shutdown_expired(void);
/* Publish the durable terminal receipt after every persistence operation. */
bool admin_shutdown_finish(bool saved);

#ifdef ATRINIK_TESTING
void admin_shutdown_fail_sync_for_test(bool fail);
#endif

#endif
