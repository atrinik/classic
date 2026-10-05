/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <access_resolver.h>
#include <main.h>
#include <metaserver.h>
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdlib.h>

struct access_resolver_job {
    client_access_attempt_t attempt;
    server_struct *server;
    SDL_Thread *thread;
    atomic_bool done, cancelled;
    struct access_resolver_job *next;
};
/* Only the main thread reads/writes the list and thread handles. The worker
 * owns attempt/server until publishing done with release semantics. */
static access_resolver_job_t *jobs;
static unsigned job_count;

static bool cancelled(void *context) {
    access_resolver_job_t *job = context;
    return atomic_load(&job->cancelled);
}

static int resolve_worker(void *context) {
    access_resolver_job_t *job = context;
    curl_cancel_t cancel = {.cancelled = cancelled, .context = job};
    server_struct *server = metaserver_access_resolve_cancellable(job->attempt.code, &cancel);
    if (server != NULL) {
        if (cancelled(job)) {
            metaserver_server_free(server);
            server = NULL;
        } else {
            server->access_attempt = job->attempt;
        }
    }
    client_access_attempt_clear(&job->attempt);
    job->server = server;
    atomic_store_explicit(&job->done, true, memory_order_release);
    return 0;
}

static void release_job(access_resolver_job_t *job) {
    access_resolver_job_t **link = &jobs;
    while (*link != job)
        link = &(*link)->next;
    *link = job->next;
    job_count--;
    SDL_WaitThread(job->thread, NULL);
    if (job->server != NULL)
        metaserver_server_free(job->server);
    client_access_attempt_clear(&job->attempt);
    access_code_clear(job, sizeof(*job));
    free(job);
}

access_resolver_job_t *access_resolver_start(client_access_attempt_t *attempt) {
    access_resolver_service();
    if (job_count == ACCESS_RESOLVER_JOBS_MAX) {
        client_access_attempt_clear(attempt);
        return NULL;
    }
    access_resolver_job_t *job = calloc(1, sizeof(*job));
    if (job != NULL) {
        atomic_init(&job->done, false);
        atomic_init(&job->cancelled, false);
        job->attempt = *attempt;
        job->thread = SDL_CreateThread(resolve_worker, "access-resolve", job);
        if (job->thread == NULL) {
            client_access_attempt_clear(&job->attempt);
            free(job);
            job = NULL;
        } else {
            job->next = jobs;
            jobs = job;
            job_count++;
        }
    }
    client_access_attempt_clear(attempt);
    return job;
}

bool access_resolver_take(access_resolver_job_t *job, server_struct **server) {
    if (job == NULL || !atomic_load_explicit(&job->done, memory_order_acquire))
        return false;
    *server = job->server;
    job->server = NULL;
    release_job(job);
    return true;
}

void access_resolver_cancel(access_resolver_job_t *job) {
    if (job != NULL)
        atomic_store(&job->cancelled, true);
}

void access_resolver_service(void) {
    access_resolver_job_t *job = jobs;
    while (job != NULL) {
        access_resolver_job_t *next = job->next;
        if (atomic_load(&job->cancelled) &&
            atomic_load_explicit(&job->done, memory_order_acquire))
            release_job(job);
        job = next;
    }
}

void access_resolver_deinit(void) {
    for (access_resolver_job_t *job = jobs; job != NULL; job = job->next)
        access_resolver_cancel(job);
    while (jobs != NULL)
        release_job(jobs);
}
