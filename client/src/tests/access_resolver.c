/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <access_resolver.h>
#include <main.h>
#include <metaserver.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "line %d\n", __LINE__); abort(); } } while (0)
static atomic_uint started, stopped, freed;
static atomic_bool finish;

/* Deliberately hold cancelled work after the callback observes cancellation:
 * popup teardown must return without waiting for this worker or its storage. */
static atomic_bool allow_cancel_return;
server_struct *metaserver_access_resolve_cancellable(const char *code, const curl_cancel_t *cancel) {
    REQUIRE(access_code_valid(code, ACCESS_CODE_LENGTH));
    atomic_fetch_add(&started, 1);
    uint64_t deadline = SDL_GetTicks() + 4000;
    while (!atomic_load(&finish) && !curl_cancelled(cancel)) {
        REQUIRE(SDL_GetTicks() < deadline);
        SDL_Delay(1);
    }
    if (curl_cancelled(cancel)) {
        while (!atomic_load(&allow_cancel_return)) {
            REQUIRE(SDL_GetTicks() < deadline);
            SDL_Delay(1);
        }
        atomic_fetch_add(&stopped, 1);
        return NULL;
    }
    server_struct *server = calloc(1, sizeof(*server));
    REQUIRE(server != NULL);
    atomic_fetch_add(&stopped, 1);
    return server;
}

void metaserver_server_free(server_struct *server) {
    client_access_attempt_clear(&server->access_attempt);
    rendezvous_access_grant_clear(&server->access_grant);
    free(server);
    atomic_fetch_add(&freed, 1);
}

static void wait_count(atomic_uint *counter, unsigned expected) {
    uint64_t deadline = SDL_GetTicks() + 4000;
    while (atomic_load(counter) != expected) {
        REQUIRE(SDL_GetTicks() < deadline);
        SDL_Delay(1);
    }
}

static access_resolver_job_t *start(void) {
    client_access_attempt_t attempt;
    REQUIRE(client_access_attempt_set(&attempt, "0123456789ABCDEF", 16));
    access_resolver_job_t *job = access_resolver_start(&attempt);
    REQUIRE(job != NULL);
    const unsigned char *bytes = (const unsigned char *)&attempt;
    for (size_t i = 0; i < sizeof(attempt); i++)
        REQUIRE(bytes[i] == 0);
    return job;
}

int main(void) {
    REQUIRE(SDL_Init(0));
    access_resolver_job_t *job = start();
    wait_count(&started, 1);
    server_struct *server = NULL;
    REQUIRE(!access_resolver_take(job, &server));
    uint64_t before = SDL_GetTicks();
    access_resolver_cancel(job);
    access_resolver_service();
    REQUIRE(SDL_GetTicks() - before < 100);
    REQUIRE(atomic_load(&stopped) == 0);
    /* Reopening gets an independent worker while cancelled storage is retained. */
    access_resolver_job_t *reopened = start();
    wait_count(&started, 2);
    atomic_store(&allow_cancel_return, true);
    atomic_store(&finish, true);
    uint64_t deadline = SDL_GetTicks() + 4000;
    while (!access_resolver_take(reopened, &server)) {
        REQUIRE(SDL_GetTicks() < deadline);
        SDL_Delay(1);
    }
    REQUIRE(server != NULL && server->access_attempt.present);
    REQUIRE(strcmp(server->access_attempt.code, "0123456789ABCDEF") == 0);
    metaserver_server_free(server);
    access_resolver_deinit();
    REQUIRE(atomic_load(&freed) == 1);

    /* A completed result abandoned before take must be released by the reaper. */
    job = start();
    wait_count(&stopped, 3);
    access_resolver_cancel(job);
    deadline = SDL_GetTicks() + 4000;
    while (atomic_load(&freed) != 2) {
        REQUIRE(SDL_GetTicks() < deadline);
        access_resolver_service();
        SDL_Delay(1);
    }

    /* Shutdown cancels active work before shared settings/toolkit teardown. */
    atomic_store(&finish, false);
    (void)start();
    wait_count(&started, 4);
    before = SDL_GetTicks();
    access_resolver_deinit();
    REQUIRE(SDL_GetTicks() - before < 1000);
    REQUIRE(atomic_load(&stopped) == 4);
    access_resolver_deinit();

    /* Rapid close/reopen cannot accumulate unbounded stalled resolver workers. */
    atomic_store(&allow_cancel_return, false);
    for (unsigned i = 0; i < ACCESS_RESOLVER_JOBS_MAX; i++) {
        job = start();
        wait_count(&started, 5 + i);
        access_resolver_cancel(job);
    }
    client_access_attempt_t rejected;
    REQUIRE(client_access_attempt_set(&rejected, "0123456789ABCDEF", 16));
    REQUIRE(access_resolver_start(&rejected) == NULL);
    const unsigned char *bytes = (const unsigned char *)&rejected;
    for (size_t i = 0; i < sizeof(rejected); i++)
        REQUIRE(bytes[i] == 0);
    atomic_store(&allow_cancel_return, true);
    access_resolver_deinit();
    REQUIRE(atomic_load(&stopped) == 4 + ACCESS_RESOLVER_JOBS_MAX);
    SDL_Quit();
    return 0;
}
