/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <access_resolver.h>
#include "../client/metaserver_private.h"
#include <main.h>
#include <metaserver.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "line %d\n", __LINE__); abort(); } } while (0)
static atomic_uint started, stopped, freed;
static atomic_bool finish;
static const char identity[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static server_struct *directory_server;
void metaserver_server_add(server_struct *server) {
    REQUIRE(directory_server == NULL);
    directory_server = server;
}

static server_struct *parse_protected_directory(void) {
    char xml[1024];
    int size = snprintf(xml, sizeof(xml),
        "<Servers protocol=\"6\" schema=\"atrinik-classic-directory-v6\" generation=\"1\" "
        "generated-at=\"1000\" expires-at=\"2000\"><Server><Id>%s</Id>"
        "<Name>Protected</Name><PlayersCount>0</PlayersCount><Version>1</Version>"
        "<TextComment></TextComment><CertificateSha256>%s</CertificateSha256>"
        "<AccessRequired>true</AccessRequired></Server></Servers>", identity, identity);
    REQUIRE(size > 0 && (size_t)size < sizeof(xml));
    REQUIRE(metaserver_direct_parse(xml, (size_t)size, "https://directory.example", 1500, 0, NULL));
    server_struct *server = directory_server;
    directory_server = NULL;
    REQUIRE(server != NULL && server->hostname == NULL && server->access_required);
    REQUIRE(!server->private_access && access_resolver_required(server));
    return server;
}

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
    server->server_id = strdup(identity);
    server->quic_certificate_sha256 = strdup(identity);
    server->rendezvous_origin = strdup("https://access.example");
    server->private_access = true;
    server->access_required = true;
    strcpy(server->access_grant.server_id, identity);
    memset(server->access_grant.generation, 'b', 64);
    memset(server->access_grant.client_nonce, 'c', 64);
    memset(server->access_grant.grant, 'd', 64);
    server->access_grant.expiry = 1510;
    atomic_fetch_add(&stopped, 1);
    return server;
}

void metaserver_server_free(server_struct *server) {
    client_access_attempt_clear(&server->access_attempt);
    rendezvous_access_grant_clear(&server->access_grant);
    free(server->server_id);
    free(server->quic_certificate_sha256);
    free(server->rendezvous_origin);
    free(server->hostname);
    free(server->name);
    free(server->version);
    free(server->desc);
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

static void directory_resolution(void) {
    server_struct *selected = parse_protected_directory();
    unsigned started_before = atomic_load(&started);
    atomic_store(&finish, false);
    atomic_store(&allow_cancel_return, false);
    access_resolver_job_t *job = start();
    wait_count(&started, started_before + 1);
    uint64_t before = SDL_GetTicks();
    access_resolver_cancel(job);
    REQUIRE(SDL_GetTicks() - before < 100);
    REQUIRE(!selected->access_attempt.present && !selected->private_access);
    REQUIRE(selected->access_grant.grant[0] == 0 && selected->hostname == NULL);
    atomic_store(&allow_cancel_return, true);
    access_resolver_deinit();

    atomic_store(&finish, true);
    job = start();
    server_struct *resolved = NULL;
    uint64_t deadline = SDL_GetTicks() + 4000;
    while (!access_resolver_take(job, &resolved)) {
        REQUIRE(SDL_GetTicks() < deadline);
        SDL_Delay(1);
    }
    REQUIRE(resolved != NULL && resolved->access_attempt.present);
    /* A valid returned identity still cannot replace either selected pin. */
    char *pin = selected->quic_certificate_sha256;
    selected->quic_certificate_sha256 = NULL;
    REQUIRE(!access_resolver_adopt(selected, resolved));
    selected->quic_certificate_sha256 = pin;
    selected->server_id[0] = 'b';
    REQUIRE(!access_resolver_adopt(selected, resolved));
    selected->server_id[0] = 'a';
    selected->quic_certificate_sha256[0] = 'b';
    REQUIRE(!access_resolver_adopt(selected, resolved));
    selected->quic_certificate_sha256[0] = 'a';
    resolved->quic_certificate_sha256[0] = 'b';
    REQUIRE(!access_resolver_adopt(selected, resolved));
    resolved->quic_certificate_sha256[0] = 'a';
    REQUIRE(!selected->private_access && !selected->access_attempt.present);
    REQUIRE(selected->access_grant.grant[0] == 0);
    REQUIRE(strcmp(selected->rendezvous_origin, "https://directory.example") == 0);
    rendezvous_access_grant_t grant = resolved->access_grant;
    REQUIRE(access_resolver_adopt(selected, resolved));
    REQUIRE(selected->private_access && selected->access_attempt.present);
    REQUIRE(rendezvous_access_grant_valid(&selected->access_grant, identity, 1500));
    REQUIRE(selected->hostname == NULL);
    REQUIRE(strcmp(selected->rendezvous_origin, "https://access.example") == 0);
    REQUIRE(memcmp(&selected->access_grant, &grant, sizeof(grant)) == 0);
    REQUIRE(!resolved->access_attempt.present && resolved->access_grant.grant[0] == 0);
    REQUIRE(resolved->rendezvous_origin == NULL);
    REQUIRE(access_resolver_required(selected));
    metaserver_server_free(resolved);
    metaserver_server_free(selected);

    selected = parse_protected_directory();
    selected->hostname = strdup("direct.example");
    REQUIRE(!access_resolver_required(selected));
    selected->access_required = false;
    free(selected->hostname);
    selected->hostname = NULL;
    REQUIRE(!access_resolver_required(selected));
    REQUIRE(!access_resolver_required(NULL));
    metaserver_server_free(selected);
    access_resolver_deinit();
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
    directory_resolution();
    SDL_Quit();
    return 0;
}
