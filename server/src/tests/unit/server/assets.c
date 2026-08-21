/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <initialization.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <openssl/crypto.h>
#if OPENSSL_VERSION_NUMBER >= 0x30500000L
#include <pthread.h>
#include <stdatomic.h>
#endif
#include <toolkit/datetime.h>

#if OPENSSL_VERSION_NUMBER >= 0x30500000L
typedef struct scheduler_client {
    char host[HUGE_BUF];
    uint16_t port;
    char fingerprint[65];
    socket_t *socket;
    socket_connect_failure_t failure;
    atomic_bool finished;
} scheduler_client_t;

static void *scheduler_client_main(void *data) {
    scheduler_client_t *client = data;
    client->socket = socket_quic_client_create(client->host,
                                               client->port,
                                               client->fingerprint,
                                               NULL,
                                               NULL,
                                               NULL,
                                               SOCKET_CONNECTION_PREFERENCE_DIRECTORY,
                                               &client->failure);
    atomic_store(&client->finished, true);
    return NULL;
}
#endif

static bool first_regular_file(const char *root, const char *relative, char *output, size_t size) {
    char directory[HUGE_BUF];
    snprintf(VS(directory), "%s%s%s", root, *relative != '\0' ? "/" : "", relative);
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char child[HUGE_BUF];
        snprintf(VS(child), "%s%s%s", relative, *relative != '\0' ? "/" : "", entry->d_name);
        size_t path_size = strlen(root) + strlen(child) + 2;
        char *path = xmalloc(path_size);
        snprintf(path, path_size, "%s/%s", root, child);
        struct stat metadata;
        if (stat(path, &metadata) != 0) {
            free(path);
            continue;
        }
        free(path);
        if (S_ISDIR(metadata.st_mode) && first_regular_file(root, child, output, size)) {
            closedir(dir);
            return true;
        }
        if (S_ISREG(metadata.st_mode)) {
            int length = snprintf(output, size, "%s", child);
            closedir(dir);
            return length >= 0 && (size_t)length < size;
        }
    }
    closedir(dir);
    return false;
}

static bool first_compressed_data(char *output, size_t size) {
    char root[HUGE_BUF];
    snprintf(VS(root), "%s/data", settings.assetspath);
    DIR *dir = opendir(root);
    if (dir == NULL) {
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length > 3 && strcmp(entry->d_name + length - 3, ".zz") == 0) {
            int written = snprintf(output, size, "data/%s", entry->d_name);
            closedir(dir);
            return written >= 0 && (size_t)written < size;
        }
    }
    closedir(dir);
    return false;
}

START_TEST(test_transport_neutral_asset_cache) {
    char map_path[HUGE_BUF];
    snprintf(VS(map_path), "%s/client-maps/cache-test.png", settings.assetspath);
    FILE *map = fopen(map_path, "wb");
    ck_assert_ptr_ne(map, NULL);
    ck_assert_uint_eq(fwrite("map", 1, 3, map), 3);
    ck_assert_int_eq(fclose(map), 0);

    socket_assets_init();
    ck_assert(socket_assets_contains("data/listing.txt"));

    char name[HUGE_BUF];
    ck_assert(first_compressed_data(VS(name)));
    ck_assert(socket_assets_contains(name));
    ck_assert(socket_assets_contains("client-maps/cache-test.png"));

    char resource[HUGE_BUF];
    ck_assert(first_regular_file(settings.resourcespath, "", VS(resource)));
    size_t resource_size = strlen(resource);
    ck_assert_uint_lt(resource_size + sizeof("resources/"), sizeof(name));
    memcpy(name, "resources/", sizeof("resources/") - 1);
    memcpy(name + sizeof("resources/") - 1, resource, resource_size + 1);
    ck_assert(socket_assets_contains(name));
    ck_assert(!socket_assets_contains("http/data/listing.txt"));

    socket_assets_deinit();
    ck_assert_int_eq(unlink(map_path), 0);
}
END_TEST

START_TEST(test_transport_scheduler_runs_simulation_pass) {
    ck_assert(socket_server_process());
}
END_TEST

#if OPENSSL_VERSION_NUMBER >= 0x30500000L
START_TEST(test_transport_scheduler_services_initialized_quic_listener) {
    scheduler_client_t client = {0};
    atomic_init(&client.finished, false);
    toolkit_import(socket_server);
    ck_assert(socket_server_quic_info(VS(client.host), &client.port, client.fingerprint));
    if (client.host[0] == '\0') {
        snprintf(VS(client.host), "%s", "127.0.0.1");
    }

    pthread_t thread;
    ck_assert_int_eq(pthread_create(&thread, NULL, scheduler_client_main, &client), 0);

    uint64_t deadline = datetime_monotonic_ms() + 5000;
    bool simulation_due = false;
    while (!atomic_load(&client.finished) && datetime_monotonic_ms() < deadline) {
        simulation_due |= socket_server_process();
    }

    ck_assert_int_eq(pthread_join(thread, NULL), 0);
    ck_assert(simulation_due);
    ck_assert_ptr_nonnull(client.socket);
    ck_assert_int_eq(client.failure.code, SOCKET_CONNECT_FAILURE_NONE);

    socket_destroy(client.socket);
    client.socket = NULL;
    deadline = datetime_monotonic_ms() + 1000;
    while (datetime_monotonic_ms() < deadline) {
        socket_server_process();
        socket_server_post_process();
    }
}
END_TEST
#endif

static Suite *suite(void) {
    Suite *s = suite_create("assets");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_transport_neutral_asset_cache);
    tcase_add_test(tc_core, test_transport_scheduler_runs_simulation_pass);
#if OPENSSL_VERSION_NUMBER >= 0x30500000L
    tcase_add_test(tc_core, test_transport_scheduler_services_initialized_quic_listener);
#endif
    return s;
}

void check_server_assets(void) {
    check_run_suite(suite(), __FILE__);
}
