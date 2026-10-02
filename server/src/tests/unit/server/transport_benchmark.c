/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Zoey Rose and Atrinik Development Team           *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Loopback benchmark for the deadline-driven QUIC server pass.
 *
 * The benchmark deliberately keeps all server work on the Check runner's
 * thread. Client connections are established concurrently only to make the
 * multi-connection case observable; the server never starts a networking
 * thread or a cross-thread gameplay queue.
 */

#include <global.h>
#include <server.h>
#include <server_main.h>
#include <initialization.h>
#include <network_metrics.h>
#include <map.h>
#include <object.h>
#include <player.h>
#include <commands.h>
#include <movement.h>
#include <tod.h>
#include <plugin.h>
#include <account.h>
#include <toolkit/packet.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <toolkit/datetime.h>

#include <openssl/crypto.h>

#if OPENSSL_VERSION_NUMBER >= 0x30500000L
#include <pthread.h>
#include <stdatomic.h>
#ifndef WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif

#define TRANSPORT_BENCHMARK_CLIENTS 4U
#define TRANSPORT_BENCHMARK_SAMPLES 24U
#define TRANSPORT_BENCHMARK_IDLE_MS UINT64_C(2000)
#define TRANSPORT_BENCHMARK_ROUND_TIMEOUT_MS UINT64_C(2000)
#define TRANSPORT_BENCHMARK_LATE_GRACE_MS UINT64_C(500)
#define TRANSPORT_BENCHMARK_BUFFER_SIZE 128U

typedef struct transport_benchmark_client {
    char host[HUGE_BUF];
    uint16_t port;
    char fingerprint[65];
    socket_t *socket;
    socket_connect_failure_t failure;
    atomic_bool finished;
    uint8_t receive_buffer[TRANSPORT_BENCHMARK_BUFFER_SIZE];
    size_t receive_length;
    uint32_t expected_id;
    uint64_t sent_us;
    uint64_t rtt_us[TRANSPORT_BENCHMARK_SAMPLES + 1U];
    size_t sent;
    size_t responses;
    size_t rtt_count;
    size_t late;
    size_t missed;
    bool failed;
} transport_benchmark_client_t;

typedef struct transport_benchmark_clock {
    uint64_t wall_us;
    uint64_t cpu_us;
} transport_benchmark_clock_t;

static void *transport_benchmark_connect(void *data) {
    transport_benchmark_client_t *client = data;
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

static uint64_t transport_benchmark_cpu_us(void) {
#ifndef WIN32
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    return (uint64_t)usage.ru_utime.tv_sec * UINT64_C(1000000) +
           (uint64_t)usage.ru_utime.tv_usec +
           (uint64_t)usage.ru_stime.tv_sec * UINT64_C(1000000) +
           (uint64_t)usage.ru_stime.tv_usec;
#else
    return 0;
#endif
}

static transport_benchmark_clock_t transport_benchmark_clock(void) {
    return (transport_benchmark_clock_t){
        .wall_us = datetime_monotonic_us(),
        .cpu_us = transport_benchmark_cpu_us(),
    };
}

/** Run exactly the server loop's transport and, when due, simulation lanes. */
static bool transport_benchmark_server_pass(size_t *simulation_passes) {
    uint64_t loop_started_us = datetime_monotonic_us();
    bool simulation_due = socket_server_process();
    if (!simulation_due) {
        return false;
    }

    main_process();
    socket_server_post_process();
    socket_assets_service();
    server_metrics_game_loop(datetime_monotonic_us() - loop_started_us);
    sleep_delta_complete();
    (*simulation_passes)++;
    return true;
}

static void transport_benchmark_service_clients(transport_benchmark_client_t *clients,
                                                size_t count) {
    for (size_t i = 0; i < count; i++) {
        socket_quic_service(clients[i].socket, false, true);
    }
}

static bool transport_benchmark_connect_clients(transport_benchmark_client_t *clients,
                                                 size_t count,
                                                 size_t *simulation_passes) {
    pthread_t threads[TRANSPORT_BENCHMARK_CLIENTS];
    size_t created = 0;
    size_t finished = 0;
    uint64_t deadline = datetime_monotonic_ms() + 5000U;

    for (size_t i = 0; i < count; i++) {
        atomic_init(&clients[i].finished, false);
        if (pthread_create(&threads[i], NULL, transport_benchmark_connect, &clients[i]) != 0) {
            clients[i].failed = true;
            break;
        }
        created++;
    }

    while (finished != created && datetime_monotonic_ms() < deadline) {
        transport_benchmark_server_pass(simulation_passes);
        finished = 0;
        for (size_t i = 0; i < created; i++) {
            finished += atomic_load(&clients[i].finished);
        }
    }

    bool success = created == count && finished == count;
    for (size_t i = 0; i < created; i++) {
        pthread_join(threads[i], NULL);
    }
    for (size_t i = 0; i < count; i++) {
        if (clients[i].socket == NULL || clients[i].failure.code != SOCKET_CONNECT_FAILURE_NONE) {
            success = false;
        }
    }
    return success;
}

static bool transport_benchmark_write_keepalive(transport_benchmark_client_t *client,
                                                 uint32_t id) {
    uint8_t frame[] = {
        0,
        5,
        SERVER_CMD_KEEPALIVE,
        (uint8_t)(id >> 24),
        (uint8_t)(id >> 16),
        (uint8_t)(id >> 8),
        (uint8_t)id,
    };
    size_t position = 0;
    uint64_t deadline = datetime_monotonic_ms() + 1000U;
    while (position < sizeof(frame) && datetime_monotonic_ms() < deadline) {
        size_t amount = 0;
        if (!socket_write(client->socket, frame + position, sizeof(frame) - position, &amount)) {
            return false;
        }
        position += amount;
        if (position == sizeof(frame)) {
            break;
        }

        bool ready = socket_wait(client->socket,
                                 true,
                                 true,
                                 socket_quic_timeout(client->socket, 1U));
        socket_quic_service(client->socket, ready, true);
        if (amount == 0) {
            usleep(1000);
        }
    }
    socket_quic_service(client->socket, false, true);
    return position == sizeof(frame);
}

static bool transport_benchmark_poll_client(transport_benchmark_client_t *client,
                                             uint64_t response_deadline_us,
                                             bool *received) {
    bool ready = socket_wait(client->socket, true, false, 0U);
    socket_quic_service(client->socket, ready, false);

    for (;;) {
        size_t amount = 0;
        if (!socket_read(client->socket,
                         client->receive_buffer + client->receive_length,
                         sizeof(client->receive_buffer) - client->receive_length,
                         &amount)) {
            return false;
        }
        if (amount == 0) {
            break;
        }
        client->receive_length += amount;
        if (client->receive_length == sizeof(client->receive_buffer)) {
            return false;
        }
    }

    while (client->receive_length >= 2) {
        size_t payload_length = ((size_t)client->receive_buffer[0] << 8) |
                                client->receive_buffer[1];
        size_t frame_length = payload_length + 2U;
        if (payload_length < 5U || frame_length > sizeof(client->receive_buffer)) {
            return false;
        }
        if (client->receive_length < frame_length) {
            break;
        }
        if (client->receive_buffer[2] != CLIENT_CMD_KEEPALIVE) {
            return false;
        }
        uint32_t id = ((uint32_t)client->receive_buffer[3] << 24) |
                      ((uint32_t)client->receive_buffer[4] << 16) |
                      ((uint32_t)client->receive_buffer[5] << 8) |
                      client->receive_buffer[6];
        if (id != client->expected_id) {
            return false;
        }

        uint64_t now_us = datetime_monotonic_us();
        if (client->rtt_count < arraysize(client->rtt_us)) {
            client->rtt_us[client->rtt_count++] =
                now_us >= client->sent_us ? now_us - client->sent_us : 0;
        }
        client->responses++;
        if (now_us > response_deadline_us) {
            client->late++;
        }
        *received = true;
        memmove(client->receive_buffer,
                client->receive_buffer + frame_length,
                client->receive_length - frame_length);
        client->receive_length -= frame_length;
        break;
    }
    return true;
}

static bool transport_benchmark_round(transport_benchmark_client_t *clients,
                                       size_t count,
                                       uint32_t round,
                                       size_t *simulation_passes) {
    bool received[TRANSPORT_BENCHMARK_CLIENTS] = {0};
    size_t pending = count;
    uint64_t response_deadline_us =
        datetime_monotonic_us() + TRANSPORT_BENCHMARK_ROUND_TIMEOUT_MS * UINT64_C(1000);
    uint64_t late_deadline_us = response_deadline_us + TRANSPORT_BENCHMARK_LATE_GRACE_MS *
                                                         UINT64_C(1000);

    for (size_t i = 0; i < count; i++) {
        clients[i].expected_id = round * (uint32_t)count + (uint32_t)i + 1U;
        clients[i].sent_us = datetime_monotonic_us();
        clients[i].sent++;
        if (!transport_benchmark_write_keepalive(&clients[i], clients[i].expected_id)) {
            clients[i].failed = true;
            return false;
        }
    }

    while (pending != 0 && datetime_monotonic_us() < late_deadline_us) {
        transport_benchmark_server_pass(simulation_passes);
        for (size_t i = 0; i < count; i++) {
            bool was_received = received[i];
            if (!was_received &&
                !transport_benchmark_poll_client(&clients[i], response_deadline_us, &received[i])) {
                clients[i].failed = true;
                return false;
            }
            if (!was_received && received[i]) {
                pending--;
            }
        }
        if (pending != 0) {
            usleep(1000);
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (!received[i]) {
            clients[i].missed++;
        }
    }
    return pending == 0;
}

static int transport_benchmark_compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a > b ? 1 : a < b ? -1 : 0;
}

static uint64_t transport_benchmark_percentile(const uint64_t *values,
                                               size_t count,
                                               unsigned int percentile) {
    uint64_t sorted[TRANSPORT_BENCHMARK_CLIENTS * (TRANSPORT_BENCHMARK_SAMPLES + 1U)];
    if (count == 0 || count > arraysize(sorted)) {
        return 0;
    }
    memcpy(sorted, values, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), transport_benchmark_compare_u64);
    return sorted[(count - 1U) * percentile / 100U];
}

static uint64_t transport_benchmark_max(const uint64_t *values, size_t count) {
    uint64_t maximum = 0;
    for (size_t i = 0; i < count; i++) {
        maximum = MAX(maximum, values[i]);
    }
    return maximum;
}

START_TEST(test_deadline_driven_quic_service_benchmark) {
    transport_benchmark_client_t clients[TRANSPORT_BENCHMARK_CLIENTS] = {0};
    size_t simulation_passes = 0;
    char host[HUGE_BUF];
    uint16_t port = 0;
    char fingerprint[65];
    toolkit_import(socket_server);
    ck_assert(socket_server_quic_info(VS(host), &port, fingerprint));
    if (host[0] == '\0') {
        snprintf(VS(host), "%s", "127.0.0.1");
    }
    for (size_t i = 0; i < arraysize(clients); i++) {
        snprintf(VS(clients[i].host), "%s", host);
        clients[i].port = port;
        snprintf(VS(clients[i].fingerprint), "%s", fingerprint);
    }

    bool connected = transport_benchmark_connect_clients(
        clients, arraysize(clients), &simulation_passes);
    if (!connected) {
        for (size_t i = 0; i < arraysize(clients); i++) {
            if (clients[i].socket != NULL) {
                socket_destroy(clients[i].socket);
            }
        }
    }
    ck_assert(connected);

    transport_benchmark_clock_t started = transport_benchmark_clock();
    size_t idle_passes = 0;
    size_t idle_simulation_passes = 0;
    uint64_t idle_deadline = datetime_monotonic_ms() + TRANSPORT_BENCHMARK_IDLE_MS;
    while (datetime_monotonic_ms() < idle_deadline) {
        idle_passes++;
        transport_benchmark_service_clients(clients, arraysize(clients));
        if (transport_benchmark_server_pass(&simulation_passes)) {
            idle_simulation_passes++;
        }
    }

    bool benchmark_success = idle_simulation_passes != 0 && idle_passes < 256U;
    benchmark_success = benchmark_success &&
                        transport_benchmark_round(clients,
                                                  arraysize(clients),
                                                  0,
                                                  &simulation_passes);
    for (uint32_t round = 1; round <= TRANSPORT_BENCHMARK_SAMPLES && benchmark_success; round++) {
        benchmark_success = transport_benchmark_round(clients,
                                                       arraysize(clients),
                                                       round,
                                                       &simulation_passes);
    }

    uint64_t values[TRANSPORT_BENCHMARK_CLIENTS * (TRANSPORT_BENCHMARK_SAMPLES + 1U)];
    size_t value_count = 0;
    size_t sent = 0;
    size_t responses = 0;
    size_t late = 0;
    size_t missed = 0;
    for (size_t i = 0; i < arraysize(clients); i++) {
        sent += clients[i].sent;
        responses += clients[i].responses;
        late += clients[i].late;
        missed += clients[i].missed;
        if (value_count + clients[i].rtt_count <= arraysize(values)) {
            memcpy(values + value_count,
                   clients[i].rtt_us,
                   clients[i].rtt_count * sizeof(*values));
            value_count += clients[i].rtt_count;
        }
        benchmark_success = benchmark_success && !clients[i].failed;
    }

    transport_benchmark_clock_t finished = transport_benchmark_clock();
    uint64_t wall_us = finished.wall_us - started.wall_us;
    uint64_t cpu_us = finished.cpu_us >= started.cpu_us ? finished.cpu_us - started.cpu_us : 0;
    double cpu_percent = wall_us != 0 ? (double)cpu_us * 100.0 / (double)wall_us : 0.0;
    char stats[HUGE_BUF * 4] = {0};
    server_metrics_stats(VS(stats));
    printf("QUIC benchmark: clients=%u keepalive_interval_ms=%" PRIu64
           " samples=%zu sent=%zu responses=%zu late=%zu missed=%zu\n",
           TRANSPORT_BENCHMARK_CLIENTS,
           TRANSPORT_BENCHMARK_IDLE_MS,
           value_count,
           sent,
           responses,
           late,
           missed);
    printf("Client send-to-dispatch RTT us: p50=%" PRIu64 " p95=%" PRIu64
           " p99=%" PRIu64 " max=%" PRIu64 "\n",
           transport_benchmark_percentile(values, value_count, 50),
           transport_benchmark_percentile(values, value_count, 95),
           transport_benchmark_percentile(values, value_count, 99),
           transport_benchmark_max(values, value_count));
    printf("Idle: wall_ms=%" PRIu64 " passes=%zu simulation_passes=%zu cpu_percent=%.2f\n",
           wall_us / UINT64_C(1000),
           idle_passes,
           idle_simulation_passes,
           cpu_percent);
    printf("Server simulation passes=%zu\n%s", simulation_passes, stats);

    for (size_t i = 0; i < arraysize(clients); i++) {
        socket_destroy(clients[i].socket);
    }
    ck_assert(benchmark_success);
    ck_assert_uint_eq(sent, (TRANSPORT_BENCHMARK_SAMPLES + 1U) * arraysize(clients));
    ck_assert_uint_eq(responses, sent);
    ck_assert_uint_eq(late, 0);
    ck_assert_uint_eq(missed, 0);
    ck_assert_uint_gt(value_count, 0);
}
END_TEST

/** Phase markers survive a stuck callback; the isolated runner bounds the child. */
static void strakewood_progress(unsigned int tick, const char *phase) {
    fprintf(stderr, "STRAKEWOOD tick=%u phase=%s\n", tick, phase);
    fflush(stderr);
}

static bool strakewood_python_loaded(object *pl) {
    static const char identification[] = "Python, ";
    socket_buffer_clear(CONTR(pl)->cs);
    display_plugins_list(pl);
    for (packet_struct *packet = CONTR(pl)->cs->packets; packet != NULL;
         packet = packet->next) {
        for (size_t i = 0; i + sizeof(identification) - 1 <= packet->len; i++) {
            if (memcmp(packet->data + i, identification, sizeof(identification) - 1) == 0) {
                return true;
            }
        }
    }
    return false;
}

START_TEST(test_strakewood_idle_simulation) {
    bool plugins = getenv("ATRINIK_DIAGNOSTIC_PLUGINS") != NULL;
    if (plugins) {
        strakewood_progress(0, "plugins-init");
        init_plugins();
    }
    const char *paths[] = {
        "/shattered_islands/world_1_61",
        "/shattered_islands/world_2_61",
        "/shattered_islands/world_0_70",
    };
    mapstruct *town = NULL;
    for (size_t i = 0; i < arraysize(paths); i++) {
        strakewood_progress(0, paths[i]);
        mapstruct *map = ready_map_name(paths[i], NULL, 0);
        ck_assert_ptr_nonnull(map);
        if (i + 1 == arraysize(paths)) {
            town = map;
        }
    }
    strakewood_progress(0, "player-create");
    char provision_error[HUGE_BUF];
    ck_assert_msg(account_provision("hangdiagnostic", "local-test-7!", "Hang Diagnostic",
                                   "human_male", VS(provision_error)),
                  "%s", provision_error);
    object *pl = player_get_dummy("Hang Diagnostic", NULL);
    ck_assert_ptr_nonnull(pl);
    free(CONTR(pl)->cs->account);
    CONTR(pl)->cs->account = xstrdup("hangdiagnostic");
    if (plugins) {
        ck_assert_msg(strakewood_python_loaded(pl), "Python plugin did not load");
    }
    SET_FLAG(pl, FLAG_INVULNERABLE);
    ck_assert(object_enter_map(pl, NULL, town, 20, 8, false));
    strakewood_progress(0, "initial-save");
    ck_assert(player_save_checked(pl));
    char *checkpoint_path = player_make_path(pl->name, "player.dat");
    FILE *checkpoint = fopen(checkpoint_path, "rb");
    ck_assert_ptr_nonnull(checkpoint);
    CONTR(pl)->last_save_tick = pticks;
    long last_save_tick = CONTR(pl)->last_save_tick;
    unsigned int autosaves = 0;
    socket_buffer_clear(CONTR(pl)->cs);

    /* Default is idle, matching the report. A separate opt-in run explores
     * movement and clock changes without claiming they caused the idle hang. */
    bool actions = getenv("ATRINIK_DIAGNOSTIC_ACTIONS") != NULL;
    unsigned int ticks = 12000;
    const char *tick_option = getenv("ATRINIK_DIAGNOSTIC_TICKS");
    if (tick_option != NULL) {
        char *end = NULL;
        unsigned long parsed = strtoul(tick_option, &end, 10);
        ck_assert_msg(end != tick_option && *end == '\0' && parsed > 0 && parsed <= 20000,
                      "ATRINIK_DIAGNOSTIC_TICKS must be between 1 and 20000");
        ticks = (unsigned int)parsed;
    }
    for (unsigned int tick = 0; tick < ticks; tick++) {
        if (actions && tick % 128 == 0) {
            strakewood_progress(tick, "settime");
            char hour[16];
            snprintf(VS(hour), "%u", (tick / 128) % HOURS_PER_DAY);
            command_settime(pl, "settime", hour);
            strakewood_progress(tick, "movement");
            (void)move_ob(pl, (tick / 128) % 8 + 1, pl);
        }
        strakewood_progress(tick, "main-process");
        main_process();
        ck_assert_ptr_nonnull(pl->map);
        if (CONTR(pl)->last_save_tick != last_save_tick) {
            struct stat before, after;
            ck_assert_int_eq(fstat(fileno(checkpoint), &before), 0);
            ck_assert_int_eq(stat(checkpoint_path, &after), 0);
            ck_assert_int_gt(after.st_size, 0);
            /* Keep the previous file open until comparison so its inode cannot
             * be recycled: an advanced timer alone does not prove a save. */
            ck_assert(before.st_dev != after.st_dev || before.st_ino != after.st_ino);
            ck_assert_int_eq(fclose(checkpoint), 0);
            checkpoint = fopen(checkpoint_path, "rb");
            ck_assert_ptr_nonnull(checkpoint);
            last_save_tick = CONTR(pl)->last_save_tick;
            autosaves++;
            strakewood_progress(tick, "autosave-returned");
        }
        strakewood_progress(tick, "map2");
        draw_client_map(pl);
        strakewood_progress(tick, "queue-drain");
        /* This offline player has no authenticated peer. Exercise MAP2 and
         * simulation, but do not pass its dummy socket through live transport. */
        socket_buffer_clear(CONTR(pl)->cs);
    }
    ck_assert_uint_eq(autosaves, ticks / (AUTOSAVE + 1));
    ck_assert_int_eq(fclose(checkpoint), 0);
    free(checkpoint_path);
    strakewood_progress(ticks, "final-save");
    ck_assert(player_save_checked(pl));
    if (plugins) {
        strakewood_progress(ticks, "plugins-remove");
        remove_plugins();
    }
    strakewood_progress(ticks, "complete");
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("transport_benchmark");
    TCase *tc_core = tcase_create("Core");
    bool diagnostic = getenv("ATRINIK_DIAGNOSTIC_STRAKEWOOD") != NULL;
    tcase_set_timeout(tc_core, diagnostic ? 240 : 30);
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    if (diagnostic) {
        tcase_add_test(tc_core, test_strakewood_idle_simulation);
    } else {
        tcase_add_test(tc_core, test_deadline_driven_quic_service_benchmark);
    }
    return s;
}

#else

static Suite *suite(void) {
    return suite_create("transport_benchmark");
}

#endif

void check_server_transport_benchmark(void) {
#if OPENSSL_VERSION_NUMBER >= 0x30500000L
    check_run_suite(suite(), __FILE__);
#else
    (void)suite;
    if (getenv("ATRINIK_DIAGNOSTIC_STRAKEWOOD") != NULL) {
        fputs("Strakewood diagnostic requires the OpenSSL 3.5 test runner\n", stderr);
        exit(EXIT_FAILURE);
    }
#endif
}
