/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ACCESS_RESOLVER_H
#define ACCESS_RESOLVER_H
#include <access_attempt.h>
#include <stdbool.h>
typedef struct server_struct server_struct;
typedef struct access_resolver_job access_resolver_job_t;
/* Includes cancelled workers whose transport cleanup has not finished. */
#define ACCESS_RESOLVER_JOBS_MAX 8U
/* Private sessions and addressless protected directory entries need a fresh grant. */
bool access_resolver_required(const server_struct *server);
/* Main-thread only. Adopt a trusted resolver result only if its identity and
 * certificate pin match the selected entry. Failure leaves both untouched;
 * success moves endpoint, grant and attempt ownership, preserving directory pins.
 * The caller still owns and must free the detached result. */
bool access_resolver_adopt(server_struct *selected, server_struct *resolved);
/* All lifecycle calls are main-thread only. Start consumes and clears attempt
 * even on failure. Jobs copy endpoint settings and retained secret data; they
 * never retain UI state or mutable session settings. */
access_resolver_job_t *access_resolver_start(client_access_attempt_t *attempt);
/* Nonblocking. On true, consumes job and transfers the detached server (possibly
 * NULL on failure) to the caller. The caller must forget its job pointer. */
bool access_resolver_take(access_resolver_job_t *job, server_struct **server);
/* Immediately drops UI ownership. Caller must forget job; the worker retains
 * its storage until completion. Completed cancelled jobs are reaped by service. */
void access_resolver_cancel(access_resolver_job_t *job);
void access_resolver_service(void);
/* Nonblocking cancellation of every job. UI owners must forget their job
 * pointers before calling this; workers retain snapshots until reaped. */
void access_resolver_cancel_all(void);
/* Cancel and join all jobs before endpoint settings, SDL or curl are destroyed. */
void access_resolver_deinit(void);
#ifdef ATRINIK_WIDGET_TESTS
#include <metaserver_options.h>
#include <toolkit/curl.h>
/* Offline transport seam; setter rejects changes while any job owns its context. */
typedef server_struct *(*access_resolver_test_transport_t)(
    const client_metaserver_options_t *options,
    const char *code,
    const curl_cancel_t *cancel,
    void *context);
bool access_resolver_test_transport(access_resolver_test_transport_t transport, void *context);
#endif
#endif
