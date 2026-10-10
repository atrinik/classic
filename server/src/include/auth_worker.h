/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef AUTH_WORKER_H
#define AUTH_WORKER_H

#include <toolkit/password.h>

#define AUTH_WORKER_THREADS 2
#define AUTH_WORKER_QUEUE 8
#define AUTH_WORKER_CAPACITY (AUTH_WORKER_THREADS + AUTH_WORKER_QUEUE)
#define AUTH_WORKER_SECRET_SIZE 8192

/* Value-only credentials: workers never retain account, player or socket objects. */
typedef struct auth_credential {
    char record[PASSWORD_RECORD_SIZE];
    char legacy[41];
    unsigned char hash[32];
    unsigned char salt[32];
    bool pbkdf2;
} auth_credential_t;

typedef struct auth_work {
    uint64_t generation;
    uint64_t request;
    auth_credential_t credential;
    char password[AUTH_WORKER_SECRET_SIZE];
    char replacement[AUTH_WORKER_SECRET_SIZE];
    bool verify;
    bool create;
    password_verify_result_t result;
    char record[PASSWORD_RECORD_SIZE];
} auth_work_t;

/* Lifecycle, submission, cancellation and take are main-thread only. Stop joins
 * running work and clears all queued secrets/results before returning. */
bool auth_worker_start(void);
void auth_worker_stop(void);
bool auth_worker_submit(const auth_work_t *work);
bool auth_worker_take(auth_work_t *result);
void auth_worker_cancel(uint64_t generation, uint64_t request);
#ifdef ATRINIK_TESTING
void auth_worker_pause_for_test(bool pause);
size_t auth_worker_pending_for_test(void);
bool auth_worker_wait_running_for_test(size_t running);
bool auth_worker_wait_idle_for_test(void);
#endif
#endif
