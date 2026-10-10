/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 ************************************************************************/

#include <client.h>
#include <access_resolver.h>
#include <main.h>
#include <metaserver.h>
#include <metaserver_options.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expression)                                                           \
    do {                                                                              \
        if (!(expression)) {                                                          \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #expression); \
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
static size_t fetch_captured;
static size_t fetch_inflight;
static size_t fetch_peak;
static char fetched_directory[2][128];
static char fetched_rendezvous[2][128];
static bool fetch_server[2];
static bool shutdown_started;
static bool shutdown_complete;

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
    DL_APPEND(*servers, server);
}

/* This is the no-network worker seam in metaserver.c. */
void metaserver_test_fetch(const client_metaserver_options_t *options, server_struct **servers) {
    REQUIRE(options != NULL && options->count == 1 && servers != NULL);

    SDL_LockMutex(fetch_mutex);
    size_t index = fetch_started++;
    fetch_inflight++;
    if (fetch_inflight > fetch_peak) {
        fetch_peak = fetch_inflight;
    }
    REQUIRE(index < 2);
    SDL_BroadcastCondition(fetch_condition);
    while (fetch_block && !fetch_release) {
        SDL_WaitCondition(fetch_condition, fetch_mutex);
    }
    REQUIRE(snprintf(fetched_directory[index],
                     sizeof(fetched_directory[index]),
                     "%s",
                     options->endpoints[0].directory_url) < (int)sizeof(fetched_directory[index]));
    REQUIRE(snprintf(fetched_rendezvous[index],
                     sizeof(fetched_rendezvous[index]),
                     "%s",
                     options->endpoints[0].rendezvous_origin) < (int)sizeof(fetched_rendezvous[index]));
    fetch_captured++;
    SDL_BroadcastCondition(fetch_condition);
    if (fetch_server[index]) {
        test_server_append(servers, index == 0 ? "obsolete" : "development");
    }
    fetch_inflight--;
    SDL_BroadcastCondition(fetch_condition);
    SDL_UnlockMutex(fetch_mutex);
}

static void wait_for_fetch(size_t count) {
    for (size_t i = 0; i < 1000; i++) {
        bool started;
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

static void release_fetch(void) {
    SDL_LockMutex(fetch_mutex);
    fetch_release = true;
    SDL_BroadcastCondition(fetch_condition);
    SDL_UnlockMutex(fetch_mutex);
}

static void set_fetch_server(size_t index, bool value) {
    SDL_LockMutex(fetch_mutex);
    fetch_server[index] = value;
    SDL_UnlockMutex(fetch_mutex);
}

static void wait_for_capture(size_t count) {
    for (size_t i = 0; i < 1000; i++) {
        bool captured;
        SDL_LockMutex(fetch_mutex);
        captured = fetch_captured >= count;
        SDL_UnlockMutex(fetch_mutex);
        if (captured) {
            return;
        }
        SDL_Delay(1);
    }
    REQUIRE(false);
}

static size_t get_fetch_peak(void) {
    SDL_LockMutex(fetch_mutex);
    size_t peak = fetch_peak;
    SDL_UnlockMutex(fetch_mutex);
    return peak;
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

static int shutdown_thread(void *unused) {
    (void)unused;
    SDL_LockMutex(fetch_mutex);
    shutdown_started = true;
    SDL_BroadcastCondition(fetch_condition);
    SDL_UnlockMutex(fetch_mutex);
    metaserver_deinit();
    SDL_LockMutex(fetch_mutex);
    shutdown_complete = true;
    SDL_BroadcastCondition(fetch_condition);
    SDL_UnlockMutex(fetch_mutex);
    return 0;
}

static void wait_for_shutdown_start(void) {
    for (size_t i = 0; i < 1000; i++) {
        bool started;
        SDL_LockMutex(fetch_mutex);
        started = shutdown_started;
        SDL_UnlockMutex(fetch_mutex);
        if (started && ms_connecting(-1) == 0) {
            return;
        }
        SDL_Delay(1);
    }
    REQUIRE(false);
}

server_struct *metaserver_access_resolve_cancellable(
    const client_metaserver_options_t *options, const char *code, const curl_cancel_t *cancel) {
    (void)options;
    (void)code;
    (void)cancel;
    REQUIRE(false);
    return NULL;
}

int main(void) {
    fetch_mutex = SDL_CreateMutex();
    fetch_condition = SDL_CreateCondition();
    REQUIRE(fetch_mutex != NULL && fetch_condition != NULL);

    client_metaserver_options_replace_provider(&clioption_settings.metaservers,
                                                METASERVER_PROVIDER_DEFAULT);
    metaserver_init();
    REQUIRE(metaserver_get_provider() == METASERVER_PROVIDER_DEFAULT);

    server_struct *manual = metaserver_add("manual.invalid", 13327, "manual", "test", "manual");
    REQUIRE(manual != NULL && server_get_count() == 1);
    fetch_block = true;
    set_fetch_server(0, true);
    metaserver_get_servers();
    wait_for_fetch(1);
    selected_server = manual;
    REQUIRE(client_access_attempt_set(&manual->access_attempt, "0123456789ABCDEF", 16));
    /* Replacing configuration while the request is blocked must discard its result. */
    metaserver_toggle_provider();
    REQUIRE(selected_server == NULL);
    metaserver_get_servers();
    metaserver_toggle_provider();
    metaserver_get_servers();
    metaserver_toggle_provider();
    REQUIRE(metaserver_get_provider() == METASERVER_PROVIDER_DEV);
    REQUIRE(server_get_count() == 0);
    REQUIRE(ms_connecting(-1) == 1);
    metaserver_get_servers();
    manual = metaserver_add("manual.invalid", 13327, "manual", "test", "manual");
    REQUIRE(manual != NULL && server_get_count() == 1);
    set_fetch_server(1, true);
    release_fetch();
    poll_until_fetch(2);
    wait_for_capture(2);
    REQUIRE(get_fetch_peak() == 1);
    REQUIRE(strcmp(fetched_directory[0], "https://classic.metaserver.atrinik.org/index.xml") ==
            0);
    REQUIRE(strcmp(fetched_rendezvous[0], "https://rendezvous.meta.atrinik.org/v1/classic") ==
            0);
    REQUIRE(strcmp(fetched_directory[1],
                   "https://classic.dev.metaserver.atrinik.org/index.xml") == 0);
    REQUIRE(strcmp(fetched_rendezvous[1],
                   "https://rendezvous.dev.meta.atrinik.org/v1/classic") == 0);

    poll_until_idle();
    REQUIRE(server_get_count() == 2);
    REQUIRE(strcmp(server_get_id(0)->name, "development") == 0);
    REQUIRE(strcmp(server_get_id(1)->name, "manual") == 0);
    REQUIRE(ms_connecting(-1) == 0);

    /* A current request without results must finish cleanly and clear connecting. */
    metaserver_clear_data();
    REQUIRE(server_get_count() == 0);
    fetch_block = false;
    set_fetch_server(0, false);
    fetch_started = 0;
    fetch_captured = 0;
    fetch_release = false;
    metaserver_get_servers();
    wait_for_fetch(1);
    poll_until_idle();
    REQUIRE(server_get_count() == 0);
    REQUIRE(ms_connecting(-1) == 0);

    metaserver_deinit();
    client_metaserver_options_deinit(&clioption_settings.metaservers);

    client_metaserver_options_disable(&clioption_settings.metaservers);
    metaserver_init();
    metaserver_get_servers();
    REQUIRE(fetch_started == 1);
    metaserver_toggle_provider();
    REQUIRE(client_metaserver_options_enabled(&clioption_settings.metaservers));
    REQUIRE(metaserver_get_provider() == METASERVER_PROVIDER_DEV);
    fetch_block = true;
    fetch_release = false;
    fetch_started = 0;
    metaserver_get_servers();
    wait_for_fetch(1);
    SDL_Thread *shutdown = SDL_CreateThread(shutdown_thread, "metaserver-shutdown", NULL);
    REQUIRE(shutdown != NULL);
    wait_for_shutdown_start();
    SDL_LockMutex(fetch_mutex);
    REQUIRE(!shutdown_complete);
    SDL_UnlockMutex(fetch_mutex);
    release_fetch();
    SDL_WaitThread(shutdown, NULL);
    REQUIRE(shutdown_complete);
    client_metaserver_options_deinit(&clioption_settings.metaservers);
    SDL_DestroyCondition(fetch_condition);
    SDL_DestroyMutex(fetch_mutex);
    return 0;
}
