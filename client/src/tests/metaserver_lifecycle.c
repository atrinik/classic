/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 The Atrinik Project                              *
 ************************************************************************/

#include <client.h>
#include <main.h>
#include <metaserver.h>
#include <metaserver_options.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expression)                                                           \
    do {                                                                              \
        if (!(expression)) {                                                          \
            fprintf(stderr, "%s:%d: failed: %s\\n", __FILE__, __LINE__, #expression); \
            abort();                                                                  \
        }                                                                             \
    } while (0)

clioption_settings_struct clioption_settings;
server_struct *selected_server;

static SDL_Mutex *fetch_mutex;
static SDL_Condition *fetch_condition;
static bool fetch_block;
static bool fetch_release;
static size_t fetch_started;
static char fetched_directory[2][128];
static char fetched_rendezvous[2][128];
static bool fetch_server[2];

static void test_server_append(server_struct **servers, const char *name) {
    server_struct *server = calloc(1, sizeof(*server));
    REQUIRE(server != NULL);
    server->hostname = strdup("test.invalid");
    server->name = strdup(name);
    server->version = strdup("test");
    server->desc = strdup("test server");
    REQUIRE(server->hostname != NULL && server->name != NULL && server->version != NULL &&
            server->desc != NULL);
    server->is_meta = true;
    server->next = *servers;
    if (*servers != NULL) {
        (*servers)->prev = server;
    }
    *servers = server;
}

/* This is the no-network worker seam in metaserver.c. */
void metaserver_test_fetch(const client_metaserver_options_t *options, server_struct **servers) {
    REQUIRE(options != NULL && options->count == 1 && servers != NULL);

    SDL_LockMutex(fetch_mutex);
    size_t index = fetch_started++;
    REQUIRE(index < 2);
    REQUIRE(snprintf(fetched_directory[index],
                     sizeof(fetched_directory[index]),
                     "%s",
                     options->endpoints[0].directory_url) < (int)sizeof(fetched_directory[index]));
    REQUIRE(snprintf(fetched_rendezvous[index],
                     sizeof(fetched_rendezvous[index]),
                     "%s",
                     options->endpoints[0].rendezvous_origin) < (int)sizeof(fetched_rendezvous[index]));
    SDL_ConditionBroadcast(fetch_condition);
    while (fetch_block && !fetch_release) {
        SDL_WaitCondition(fetch_condition, fetch_mutex);
    }
    if (fetch_server[index]) {
        test_server_append(servers, index == 0 ? "obsolete" : "development");
    }
    SDL_ConditionBroadcast(fetch_condition);
    SDL_UnlockMutex(fetch_mutex);
}

static void wait_for_fetch(size_t count) {
    SDL_LockMutex(fetch_mutex);
    while (fetch_started < count) {
        SDL_WaitCondition(fetch_condition, fetch_mutex);
    }
    SDL_UnlockMutex(fetch_mutex);
}

static void release_fetch(void) {
    SDL_LockMutex(fetch_mutex);
    fetch_release = true;
    SDL_ConditionBroadcast(fetch_condition);
    SDL_UnlockMutex(fetch_mutex);
}

static void poll_until_fetch(size_t count) {
    for (size_t i = 0; i < 1000; i++) {
        bool started;
        metaserver_poll();
        SDL_LockMutex(fetch_mutex);
        started = fetch_started >= count;
        SDL_UnlockMutex(fetch_mutex);
        if (started) {
            return;
        }
        SDL_Delay(1);
    }
    REQUIRE(false);
}

static void poll_until_idle(void) {
    for (size_t i = 0; i < 1000; i++) {
        metaserver_poll();
        if (ms_connecting(-1) == 0) {
            return;
        }
        SDL_Delay(1);
    }
    REQUIRE(false);
}

int main(void) {
    fetch_mutex = SDL_CreateMutex();
    fetch_condition = SDL_CreateCondition();
    REQUIRE(fetch_mutex != NULL && fetch_condition != NULL);

    client_metaserver_options_replace_provider(&clioption_settings.metaservers,
                                                METASERVER_PROVIDER_DEFAULT);
    metaserver_init();
    REQUIRE(metaserver_get_provider() == METASERVER_PROVIDER_DEFAULT);

    fetch_block = true;
    fetch_server[0] = true;
    metaserver_get_servers();
    wait_for_fetch(1);
    REQUIRE(strcmp(fetched_directory[0], "https://classic.metaserver.atrinik.org/index.xml") ==
            0);
    REQUIRE(strcmp(fetched_rendezvous[0], "https://rendezvous.meta.atrinik.org/v1/classic") ==
            0);

    /* Replacing configuration while the request is blocked must discard its result. */
    metaserver_toggle_provider();
    REQUIRE(metaserver_get_provider() == METASERVER_PROVIDER_DEV);
    REQUIRE(server_get_count() == 0);
    REQUIRE(ms_connecting(-1) == 1);
    metaserver_get_servers();
    fetch_server[1] = true;
    release_fetch();
    poll_until_fetch(2);
    REQUIRE(strcmp(fetched_directory[1],
                   "https://classic.dev.metaserver.atrinik.org/index.xml") == 0);
    REQUIRE(strcmp(fetched_rendezvous[1],
                   "https://rendezvous.dev.meta.atrinik.org/v1/classic") == 0);
    REQUIRE(server_get_count() == 0);

    poll_until_idle();
    REQUIRE(server_get_count() == 1);
    REQUIRE(strcmp(server_get_id(0)->name, "development") == 0);
    REQUIRE(ms_connecting(-1) == 0);

    /* A current request without results must finish cleanly and clear connecting. */
    metaserver_clear_data();
    REQUIRE(server_get_count() == 0);
    fetch_block = false;
    fetch_server[0] = false;
    fetch_started = 0;
    fetch_release = false;
    metaserver_get_servers();
    wait_for_fetch(1);
    poll_until_idle();
    REQUIRE(server_get_count() == 0);
    REQUIRE(ms_connecting(-1) == 0);

    metaserver_deinit();
    client_metaserver_options_deinit(&clioption_settings.metaservers);
    SDL_DestroyCondition(fetch_condition);
    SDL_DestroyMutex(fetch_mutex);
    return 0;
}
