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
/* All lifecycle calls are main-thread only. Start consumes and clears attempt
 * even on failure. Jobs copy all retained secret data and never retain UI state. */
access_resolver_job_t *access_resolver_start(client_access_attempt_t *attempt);
/* Nonblocking. On true, consumes job and transfers the detached server (possibly
 * NULL on failure) to the caller. The caller must forget its job pointer. */
bool access_resolver_take(access_resolver_job_t *job, server_struct **server);
/* Immediately drops UI ownership. Caller must forget job; the worker retains
 * its storage until completion. Completed cancelled jobs are reaped by service. */
void access_resolver_cancel(access_resolver_job_t *job);
void access_resolver_service(void);
/* Cancel and join all jobs before endpoint settings, SDL or curl are destroyed. */
void access_resolver_deinit(void);
#endif
