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
#include <object_methods.h>
#include <player.h>
#include <commands.h>
#include <movement.h>
#include <tod.h>
#include <plugin.h>
#include <account.h>
#include <arch.h>
#include <walking_route.h>
#include <gameplay_journal.h>
#include <auth_worker.h>
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

/** Bounded offline measurements; sorting happens only after the workload. */
#define STRAKEWOOD_TIMING_LIMIT 20000U
#define ISSUE566_DURABILITY_SAMPLES 24U
#define ISSUE566_SLOW_EVENT_LIMIT 64U

typedef struct strakewood_timing {
    const char *stage;
    size_t count;
    size_t slow_events;
    uint64_t samples[STRAKEWOOD_TIMING_LIMIT];
} strakewood_timing_t;

static void strakewood_timing_add(strakewood_timing_t *timing, uint64_t started_us) {
    ck_assert_uint_lt(timing->count, arraysize(timing->samples));
    uint64_t finished_us = datetime_monotonic_us();
    ck_assert_uint_ge(finished_us, started_us);
    size_t sample_index = timing->count;
    uint64_t elapsed_us = finished_us - started_us;
    timing->samples[timing->count++] = elapsed_us;
    if (elapsed_us >= UINT64_C(50000)) {
        /* Stop timing before output; retain bounded chronological evidence. */
        if (timing->slow_events++ < ISSUE566_SLOW_EVENT_LIMIT) {
            printf("ISSUE566 slow_event stage=%s sample_index=%zu duration_us=%" PRIu64 "\n",
                   timing->stage, sample_index, elapsed_us);
        }
    }
}

static void strakewood_timing_report(strakewood_timing_t *timing) {
    if (timing->count == 0) {
        return;
    }
    qsort(timing->samples, timing->count, sizeof(*timing->samples),
          transport_benchmark_compare_u64);
    printf("ISSUE566 timing_us stage=%s count=%zu p50=%" PRIu64
           " p95=%" PRIu64 " max=%" PRIu64 " slow_events=%zu\n",
           timing->stage, timing->count,
           timing->samples[(timing->count - 1U) * 50U / 100U],
           timing->samples[(timing->count * 95U + 99U) / 100U - 1U],
           timing->samples[timing->count - 1U], timing->slow_events);
}

/** Read-only map counts, collected outside the measured stages. */
typedef struct issue566_map_counts {
    size_t total, resident, swapped;
} issue566_map_counts;

static issue566_map_counts issue566_maps(void) {
    issue566_map_counts counts = {0};
    for (mapstruct *map = first_map; map != NULL; map = map->next) {
        counts.total++;
        counts.resident += map->in_memory == MAP_IN_MEMORY;
        counts.swapped += map->in_memory == MAP_SWAPPED;
    }
    return counts;
}

static void issue566_map_changes(const char *stage, unsigned int tick,
                                 issue566_map_counts before) {
    issue566_map_counts after = issue566_maps();
    if (before.total != after.total || before.resident != after.resident ||
        before.swapped != after.swapped) {
        printf("ISSUE566 map_states stage=%s sample_index=%u total=%zu->%zu "
               "resident=%zu->%zu swapped=%zu->%zu\n", stage, tick,
               before.total, after.total, before.resident, after.resident,
               before.swapped, after.swapped);
    }
}

/** Measure normal durable APIs with synthetic, typed no-op progression records.
 * No recovery record claims a gameplay change or uses live player state. */
static void issue566_durability_benchmark(object *pl, strakewood_timing_t *timings) {
    ck_assert(gameplay_journal_available());
    for (unsigned int sample = 0; sample < ISSUE566_DURABILITY_SAMPLES; sample++) {
        for (unsigned int extra_map = 0; extra_map < 2; extra_map++) {
            char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
            uint64_t started_us = datetime_monotonic_us();
            ck_assert(gameplay_journal_player_begin(CONTR(pl), "progression",
                                                   "diagnostic.persistence-noop",
                                                   "diagnostic:persistence", "",
                                                   0, 0, 0, transaction));
            if (extra_map != 0) {
                ck_assert(gameplay_journal_track_map(transaction, pl->map));
            }
            ck_assert(gameplay_journal_commit(transaction));
            strakewood_timing_add(&timings[extra_map], started_us);
        }
        uint64_t started_us = datetime_monotonic_us();
        ck_assert(player_save_checked(pl));
        strakewood_timing_add(&timings[2], started_us);
    }
}
/* Unlike the timing benchmark, this fixture receives arbitrary gameplay
 * packets. A complete maximum-sized wire frame fits, including its header. */
typedef struct transport_auth_received {
    uint8_t data[UINT16_MAX + 2U];
    size_t length;
    bool setup;
    bool version;
    bool authenticated;
    bool keepalive;
    uint32_t keepalive_id;
    size_t maps;
    uint8_t x;
    uint8_t y;
} transport_auth_received_t;

static const char transport_auth_account[] = "transportauth";
static const char transport_auth_character[] = "Transport Auth";
static const char transport_auth_password[] = "local-transport-7!";

static bool transport_auth_write(transport_benchmark_client_t *client,
                                 const uint8_t *payload,
                                 size_t length) {
    uint8_t frame[MAX_BUF];
    if (length == 0 || length + 2U > sizeof(frame)) {
        return false;
    }
    frame[0] = (uint8_t)(length >> 8);
    frame[1] = (uint8_t)length;
    memcpy(frame + 2U, payload, length);
    size_t position = 0;
    uint64_t deadline = datetime_monotonic_ms() + 1000U;
    while (position < length + 2U && datetime_monotonic_ms() < deadline) {
        size_t amount = 0;
        if (!socket_write(client->socket, frame + position, length + 2U - position, &amount)) {
            return false;
        }
        position += amount;
        socket_quic_service(client->socket, false, true);
        if (amount == 0) {
            usleep(1000);
        }
    }
    return position == length + 2U;
}

static bool transport_auth_login(transport_benchmark_client_t *client, bool character) {
    uint8_t payload[MAX_BUF] = {SERVER_CMD_ACCOUNT,
                              character ? CMD_ACCOUNT_LOGIN_CHAR : CMD_ACCOUNT_LOGIN};
    const char *name = character ? transport_auth_character : transport_auth_account;
    size_t length = strlen(name) + 1U;
    memcpy(payload + 2U, name, length);
    length += 2U;
    if (!character) {
        memcpy(payload + length, transport_auth_password, sizeof(transport_auth_password));
        length += sizeof(transport_auth_password);
    }
    bool success = transport_auth_write(client, payload, length);
    OPENSSL_cleanse(payload, sizeof(payload));
    return success;
}

static bool transport_auth_skip_string(const uint8_t *data, size_t length, size_t *position) {
    if (*position >= length) {
        return false;
    }
    const uint8_t *end = memchr(data + *position, 0, length - *position);
    if (end == NULL) {
        return false;
    }
    *position = (size_t)(end - data) + 1U;
    return true;
}

/* Decode the normal MAP2 position header, including NEW-map metadata. The
 * fixture proves transport delivery and coordinates, not GPU presentation. */
static bool transport_auth_receive_packet(transport_auth_received_t *received,
                                          const uint8_t *data,
                                          size_t length) {
    switch (data[0]) {
    case CLIENT_CMD_SETUP:
        received->setup = true;
        break;
    case CLIENT_CMD_VERSION:
        received->version = length == 5U &&
            (((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) |
             ((uint32_t)data[3] << 8) | data[4]) == SOCKET_VERSION;
        break;
    case CLIENT_CMD_CHARACTERS:
        if (length > sizeof(transport_auth_account) &&
            memcmp(data + 1U, transport_auth_account, sizeof(transport_auth_account)) == 0) {
            received->authenticated = true;
        }
        break;
    case CLIENT_CMD_KEEPALIVE:
        if (length != 5U) {
            return false;
        }
        received->keepalive_id = ((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) |
                                  ((uint32_t)data[3] << 8) | data[4];
        received->keepalive = true;
        break;
    case CLIENT_CMD_MAP: {
        if (length < 2U) {
            return false;
        }
        uint8_t update = data[1];
        if (update == MAP_UPDATE_CMD_PARTIAL) {
            break;
        }
        size_t position = 2U;
        if (update != MAP_UPDATE_CMD_SAME) {
            for (size_t i = 0; i < 3U; i++) {
                if (!transport_auth_skip_string(data, length, &position)) {
                    return false;
                }
            }
            position += 2U; /* Height difference and display-region flag. */
            for (size_t i = 0; i < 3U; i++) {
                if (!transport_auth_skip_string(data, length, &position)) {
                    return false;
                }
            }
            position += update == MAP_UPDATE_CMD_CONNECTED ? 4U : 2U;
        }
        if (position + 5U > length) {
            return false;
        }
        received->x = data[position];
        received->y = data[position + 1U];
        received->maps++;
        break;
    }
    default:
        break;
    }
    return true;
}

static bool transport_auth_receive(transport_benchmark_client_t *client,
                                   transport_auth_received_t *received) {
    bool ready = socket_wait(client->socket, true, false, 0U);
    socket_quic_service(client->socket, ready, true);
    for (;;) {
        while (received->length >= 2U) {
            size_t length = ((size_t)received->data[0] << 8) | received->data[1];
            if (length == 0) {
                return false;
            }
            if (received->length < length + 2U) {
                break;
            }
            if (!transport_auth_receive_packet(received, received->data + 2U, length)) {
                return false;
            }
            received->length -= length + 2U;
            memmove(received->data, received->data + length + 2U, received->length);
        }
        if (received->length == sizeof(received->data)) {
            return false;
        }
        size_t amount = 0;
        if (!socket_read(client->socket, received->data + received->length,
                         sizeof(received->data) - received->length, &amount)) {
            return false;
        }
        if (amount == 0) {
            return true;
        }
        received->length += amount;
    }
}

static void transport_auth_pass(transport_benchmark_client_t *clients,
                                transport_auth_received_t *received,
                                size_t *simulation_passes) {
    transport_benchmark_service_clients(clients, TRANSPORT_BENCHMARK_CLIENTS);
    transport_benchmark_server_pass(simulation_passes);
    for (size_t i = 0; i < TRANSPORT_BENCHMARK_CLIENTS; i++) {
        ck_assert_msg(transport_auth_receive(&clients[i], &received[i]),
                      "QUIC gameplay receive failed for peer %zu", i);
    }
}

static void transport_auth_teardown(void) {
    auth_worker_pause_for_test(false);
    account_deinit();
    check_test_teardown();
}

START_TEST(test_quic_login_and_movement_continue_while_authentication_pending) {
    transport_benchmark_client_t clients[TRANSPORT_BENCHMARK_CLIENTS] = {0};
    transport_auth_received_t received[TRANSPORT_BENCHMARK_CLIENTS] = {0};
    size_t simulation_passes = 0;
    char error[HUGE_BUF];
    ck_assert_msg(account_provision(transport_auth_account, transport_auth_password,
                                   transport_auth_character, "human_male", VS(error)), "%s", error);
    mapstruct *map = get_empty_map(32, 32);
    ck_assert_ptr_nonnull(map);
    FREE_AND_COPY_HASH(map->path, "/tests/transport-auth");
    FREE_AND_COPY_HASH(map->name, "Transport authentication fixture");
    ck_assert_msg(player_provision_scenario(transport_auth_character, "human_male",
                                           map->path, 16, 16, NULL, VS(error)), "%s", error);
    ck_assert(account_auth_start());

    char host[HUGE_BUF], fingerprint[65];
    uint16_t port = 0;
    toolkit_import(socket_server);
    ck_assert(socket_server_quic_info(VS(host), &port, fingerprint));
    for (size_t i = 0; i < arraysize(clients); i++) {
        snprintf(VS(clients[i].host), "%s", *host != '\0' ? host : "127.0.0.1");
        clients[i].port = port;
        snprintf(VS(clients[i].fingerprint), "%s", fingerprint);
    }
    ck_assert(transport_benchmark_connect_clients(clients, arraysize(clients), &simulation_passes));
    const uint8_t version[] = {SERVER_CMD_VERSION, (uint8_t)(SOCKET_VERSION >> 24),
                              (uint8_t)(SOCKET_VERSION >> 16), (uint8_t)(SOCKET_VERSION >> 8),
                              (uint8_t)SOCKET_VERSION};
    const uint8_t setup[] = {SERVER_CMD_SETUP, CMD_SETUP_MAPSIZE, 13, 13};
    for (size_t i = 0; i < arraysize(clients); i++) {
        ck_assert(transport_auth_write(&clients[i], version, sizeof(version)));
        ck_assert(transport_auth_write(&clients[i], setup, sizeof(setup)));
    }
    uint64_t deadline = datetime_monotonic_ms() + 5000U;
    bool negotiated = false;
    while (!negotiated && datetime_monotonic_ms() < deadline) {
        transport_auth_pass(clients, received, &simulation_passes);
        negotiated = true;
        for (size_t i = 0; i < arraysize(clients); i++) {
            negotiated = negotiated && received[i].setup && received[i].version;
        }
    }
    ck_assert_msg(negotiated, "QUIC setup/version did not complete");

    auth_worker_pause_for_test(true);
    ck_assert(transport_auth_login(&clients[1], false));
    deadline = datetime_monotonic_ms() + 5000U;
    while (auth_worker_pending_for_test() == 0 && datetime_monotonic_ms() < deadline) {
        transport_auth_pass(clients, received, &simulation_passes);
    }
    ck_assert_uint_eq(auth_worker_pending_for_test(), 1);
    ck_assert(auth_worker_wait_running_for_test(1));
    size_t before_simulation = simulation_passes;
    uint64_t keepalive_started = datetime_monotonic_us();
    ck_assert(transport_benchmark_write_keepalive(&clients[2], 566U));
    deadline = datetime_monotonic_ms() + 5000U;
    while ((!received[2].keepalive || simulation_passes == before_simulation) &&
           datetime_monotonic_ms() < deadline) {
        transport_auth_pass(clients, received, &simulation_passes);
    }
    ck_assert(received[2].keepalive);
    ck_assert_uint_eq(received[2].keepalive_id, 566U);
    ck_assert_uint_gt(simulation_passes, before_simulation);
    ck_assert_uint_eq(auth_worker_pending_for_test(), 1);
    ck_assert(!received[1].authenticated);
    uint64_t keepalive_us = datetime_monotonic_us() - keepalive_started;

    auth_worker_pause_for_test(false);
    ck_assert(transport_auth_login(&clients[0], false));
    deadline = datetime_monotonic_ms() + 5000U;
    while ((!received[0].authenticated || !received[1].authenticated) &&
           datetime_monotonic_ms() < deadline) {
        transport_auth_pass(clients, received, &simulation_passes);
    }
    ck_assert(received[0].authenticated);
    ck_assert(received[1].authenticated);

    /* The other hash remains held through character login and every MOVE. */
    auth_worker_pause_for_test(true);
    ck_assert(transport_auth_login(&clients[3], false));
    deadline = datetime_monotonic_ms() + 5000U;
    while (auth_worker_pending_for_test() == 0 && datetime_monotonic_ms() < deadline) {
        transport_auth_pass(clients, received, &simulation_passes);
    }
    ck_assert_uint_eq(auth_worker_pending_for_test(), 1);
    ck_assert(auth_worker_wait_running_for_test(1));
    ck_assert(transport_auth_login(&clients[0], true));
    deadline = datetime_monotonic_ms() + 5000U;
    player *pl = NULL;
    while ((pl == NULL || received[0].maps == 0) && datetime_monotonic_ms() < deadline) {
        transport_auth_pass(clients, received, &simulation_passes);
        pl = find_player(transport_auth_character);
    }
    ck_assert_ptr_nonnull(pl);
    ck_assert_int_eq(pl->cs->state, ST_PLAYING);
    ck_assert_ptr_eq(pl->ob->map, map);
    ck_assert_uint_gt(received[0].maps, 0);
    ck_assert_uint_eq(received[0].x, 16);
    ck_assert_uint_eq(received[0].y, 16);
    before_simulation = simulation_passes;
    uint64_t movement_started = datetime_monotonic_us();
    for (uint8_t step = 1; step <= 3; step++) {
        size_t before_maps = received[0].maps;
        const uint8_t move[] = {SERVER_CMD_MOVE, EAST, 0, 0, 0, 0, step};
        ck_assert(transport_auth_write(&clients[0], move, sizeof(move)));
        deadline = datetime_monotonic_ms() + 5000U;
        while ((received[0].maps == before_maps || received[0].x != 16U + step) &&
               datetime_monotonic_ms() < deadline) {
            transport_auth_pass(clients, received, &simulation_passes);
        }
        ck_assert_int_eq(pl->ob->x, 16 + step);
        ck_assert_int_eq(pl->ob->y, 16);
        ck_assert_uint_gt(received[0].maps, before_maps);
        ck_assert_uint_eq(received[0].x, 16U + step);
        ck_assert_uint_eq(received[0].y, 16);
        ck_assert_uint_eq(auth_worker_pending_for_test(), 1);
        ck_assert(!received[3].authenticated);
    }
    ck_assert_uint_gt(simulation_passes, before_simulation);
    printf("QUIC pending-auth acceptance: keepalive_us=%" PRIu64
           " movement_steps=3 movement_us=%" PRIu64 " simulation_passes=%zu\n",
           keepalive_us, datetime_monotonic_us() - movement_started, simulation_passes);
    auth_worker_pause_for_test(false);
    deadline = datetime_monotonic_ms() + 5000U;
    while (!received[3].authenticated && datetime_monotonic_ms() < deadline) {
        transport_auth_pass(clients, received, &simulation_passes);
    }
    ck_assert(received[3].authenticated);
    account_deinit();
    pl->cs->state = ST_DEAD;
    player_logout(pl);
    for (size_t i = 0; i < arraysize(clients); i++) {
        socket_destroy(clients[i].socket);
    }
    char *path = account_make_path(transport_auth_account);
    ck_assert_int_eq(unlink(path), 0);
    free(path);
    path = player_make_path(transport_auth_character, "player.dat");
    ck_assert_int_eq(unlink(path), 0);
    free(path);
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
    const char *timing_stages[] = {
        "map-load", "initial-save", "main-process", "map2", "movement-attempt",
        "final-save", "journal-player-noop", "journal-player-map-noop", "repeated-save",
        "route-map-load", "route-plan",
    };
    strakewood_timing_t *timings = calloc(arraysize(timing_stages), sizeof(*timings));
    ck_assert_ptr_nonnull(timings);
    for (size_t i = 0; i < arraysize(timing_stages); i++) {
        timings[i].stage = timing_stages[i];
    }
    bool durability_benchmark = getenv("ATRINIK_ISSUE566_BENCHMARK") != NULL;
    if (durability_benchmark) {
        /* init() does not open the journal in unit mode. Enable the normal
         * durable policy for this entire synthetic workload, including actions
         * and autosaves, using only this isolated test runtime's datapath. */
        const gameplay_journal_profile_t profile = {
            .id = "legacy-unknown",
            .schema = 0,
            .digest = "unknown",
            .effective_axes = "unknown",
        };
        strakewood_progress(0, "journal-init");
        ck_assert(gameplay_journal_init(settings.datapath, "issue566-diagnostic", &profile));
    }
    bool plugins = getenv("ATRINIK_DIAGNOSTIC_PLUGINS") != NULL;
    if (plugins) {
        strakewood_progress(0, "plugins-init");
        init_plugins();
    }
    const char *route_option = getenv("ATRINIK_ISSUE566_ROUTE");
    bool route_mode = route_option != NULL;
    ck_assert_msg(!route_mode || strcmp(route_option, "1") == 0,
                  "ATRINIK_ISSUE566_ROUTE must be 1 when set");
    walking_route_point *route_points = NULL;
    size_t route_count = 0;
    bool route_visited[WALKING_ROUTE_MAPS] = {false};
    mapstruct *town = NULL;
    if (route_mode) {
        mapstruct *route_maps[WALKING_ROUTE_MAPS] = {NULL};
        for (int m = 0; m < WALKING_ROUTE_MAPS; m++) {
            char path[MAX_BUF];
            snprintf(VS(path), "/shattered_islands/world_%d_%d", m % 4, m / 4 + 66);
            strakewood_progress(0, path);
            uint64_t load_started_us = datetime_monotonic_us();
            route_maps[m] = ready_map_name(path, NULL, 0);
            strakewood_timing_add(&timings[9], load_started_us);
            ck_assert_ptr_nonnull(route_maps[m]);
        }
        object *human = walking_route_candidate_create(arch_find("human_male"));
        ck_assert_ptr_nonnull(human);
        uint64_t plan_started_us = datetime_monotonic_us();
        bool planned = walking_route_plan(route_maps, human, &route_points, &route_count);
        strakewood_timing_add(&timings[10], plan_started_us);
        object_destroy(human);
        ck_assert_msg(planned, "offline route planning failed; no traversal attempted");
        ck_assert_uint_gt(route_count, 1);
        ck_assert_uint_le(route_count, WALKING_ROUTE_LIMIT);
        ck_assert_int_eq(route_points[0].map, 16);
        ck_assert_int_eq(route_points[0].x, 20);
        ck_assert_int_eq(route_points[0].y, 8);
        town = route_maps[16];
        route_visited[16] = true;
    } else {
        const char *paths[] = {
            "/shattered_islands/world_1_61",
            "/shattered_islands/world_2_61",
            "/shattered_islands/world_0_70",
        };
        for (size_t i = 0; i < arraysize(paths); i++) {
            strakewood_progress(0, paths[i]);
            uint64_t load_started_us = datetime_monotonic_us();
            mapstruct *map = ready_map_name(paths[i], NULL, 0);
            strakewood_timing_add(&timings[0], load_started_us);
            ck_assert_ptr_nonnull(map);
            if (i + 1 == arraysize(paths)) {
                town = map;
            }
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
    const char *glow_option = getenv("ATRINIK_ISSUE566_GLOW");
    int requested_glow = 0;
    if (glow_option != NULL) {
        ck_assert_msg(strcmp(glow_option, "0") == 0 || strcmp(glow_option, "1") == 0 ||
                          strcmp(glow_option, "3") == 0 || strcmp(glow_option, "7") == 0 ||
                          strcmp(glow_option, "13") == 0,
                      "ATRINIK_ISSUE566_GLOW must be 0, 1, 3, 7, or 13");
        requested_glow = (int)strtol(glow_option, NULL, 10);
        if (requested_glow > 0) {
            /* Apply a real inventory emitter. Disable fuel consumption and
             * animation in this synthetic steady fixture, so emission remains
             * present throughout the bounded simulation and ordinary saves. */
            object *light = arch_get("torch");
            ck_assert_ptr_nonnull(light);
            ck_assert_int_eq(light->type, LIGHT_APPLY);
            light->nrof = 1;
            light->glow_radius = 0;
            light->last_sp = requested_glow;
            light->last_eat = 0;
            light->speed = 0;
            light->anim_speed = 0;
            CLEAR_FLAG(light, FLAG_CHANGING);
            CLEAR_FLAG(light, FLAG_ANIMATE);
            light = object_insert_into(light, pl, INS_NO_MERGE);
            ck_assert_ptr_nonnull(light);
            player_apply(pl, light, 0, 0);
            ck_assert(QUERY_FLAG(light, FLAG_APPLIED));
            ck_assert_int_eq(light->glow_radius, requested_glow);
            ck_assert(!QUERY_FLAG(light, FLAG_CHANGING));
        }
    }
    SET_FLAG(pl, FLAG_INVULNERABLE);
    ck_assert(object_enter_map(pl, NULL, town, 20, 8, false));
    if (glow_option != NULL) {
        ck_assert_int_eq(pl->glow_radius, requested_glow);
    }
    strakewood_progress(0, "initial-save");
    uint64_t started_us = datetime_monotonic_us();
    ck_assert(player_save_checked(pl));
    strakewood_timing_add(&timings[1], started_us);
    char *checkpoint_path = player_make_path(pl->name, "player.dat");
    FILE *checkpoint = fopen(checkpoint_path, "rb");
    ck_assert_ptr_nonnull(checkpoint);
    CONTR(pl)->last_save_tick = pticks;
    long last_save_tick = CONTR(pl)->last_save_tick;
    unsigned int autosaves = 0;
    unsigned int movement_successes = 0;
    socket_buffer_clear(CONTR(pl)->cs);

    /* Default is idle, matching the report. A separate opt-in run explores
     * movement and clock changes without claiming they caused the idle hang. */
    bool actions = !route_mode && getenv("ATRINIK_DIAGNOSTIC_ACTIONS") != NULL;
    unsigned int ticks = 12000;
    const char *tick_option = getenv("ATRINIK_DIAGNOSTIC_TICKS");
    if (tick_option != NULL) {
        char *end = NULL;
        unsigned long parsed = strtoul(tick_option, &end, 10);
        ck_assert_msg(end != tick_option && *end == '\0' && parsed > 0 && parsed <= 20000,
                      "ATRINIK_DIAGNOSTIC_TICKS must be between 1 and 20000");
        ticks = (unsigned int)parsed;
    }
    if (route_mode) {
        /* One ordinary move per simulation tick, exactly once through the plan.
         * This is offline server traversal, without live client acknowledgement. */
        ticks = (unsigned int)(route_count - 1U);
    }
    for (unsigned int tick = 0; tick < ticks; tick++) {
        if (route_mode) {
            static const uint8_t compass[] = {0, 6, 5, 4, 7, 0, 3, 8, 1, 2};
            walking_route_point point = route_points[tick + 1U];
            ck_assert_uint_lt(point.map, WALKING_ROUTE_MAPS);
            ck_assert_uint_lt(point.direction, arraysize(compass));
            ck_assert_uint_gt(compass[point.direction], 0);
            char expected_path[MAX_BUF];
            snprintf(VS(expected_path), "/shattered_islands/world_%d_%d",
                     point.map % 4, point.map / 4 + 66);
            strakewood_progress(tick, "route-movement");
            started_us = datetime_monotonic_us();
            mapstruct *previous_map = pl->map;
            int moved = move_ob(pl, compass[point.direction], pl);
            strakewood_timing_add(&timings[4], started_us);
            ck_assert_msg(moved && pl->map != NULL &&
                              strcmp(pl->map->path, expected_path) == 0 &&
                              pl->x == point.x && pl->y == point.y,
                          "offline route blocked or diverged at step %u; expected %s (%u,%u)",
                          tick + 1U, expected_path, point.x, point.y);
            if (pl->map != previous_map) {
                printf("ISSUE566 route_transition sample_index=%u map=%s\n",
                       tick, pl->map->path);
            }
            movement_successes++;
            route_visited[point.map] = true;
            if (glow_option != NULL) {
                ck_assert_int_eq(pl->glow_radius, requested_glow);
            }
        } else if (actions && tick % 128 == 0) {
            strakewood_progress(tick, "settime");
            char hour[16];
            snprintf(VS(hour), "%u", (tick / 128) % HOURS_PER_DAY);
            command_settime(pl, "settime", hour);
            strakewood_progress(tick, "movement");
            started_us = datetime_monotonic_us();
            mapstruct *before_map = pl->map;
            int before_x = pl->x, before_y = pl->y;
            (void)move_ob(pl, (tick / 128) % 8 + 1, pl);
            movement_successes += pl->map != before_map || pl->x != before_x || pl->y != before_y;
            strakewood_timing_add(&timings[4], started_us);
            if (glow_option != NULL) {
                ck_assert_int_eq(pl->glow_radius, requested_glow);
            }
        }
        strakewood_progress(tick, "main-process");
        issue566_map_counts before_main = issue566_maps();
        started_us = datetime_monotonic_us();
        main_process();
        strakewood_timing_add(&timings[2], started_us);
        issue566_map_changes("main-process", tick, before_main);
        ck_assert_ptr_nonnull(pl->map);
        if (route_mode) {
            walking_route_point point = route_points[tick + 1U];
            char expected_path[MAX_BUF];
            snprintf(VS(expected_path), "/shattered_islands/world_%d_%d",
                     point.map % 4, point.map / 4 + 66);
            ck_assert_msg(strcmp(pl->map->path, expected_path) == 0 &&
                              pl->x == point.x && pl->y == point.y,
                          "offline route simulation diverged after step %u", tick + 1U);
        }
        if (glow_option != NULL) {
            ck_assert_int_eq(pl->glow_radius, requested_glow);
        }
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
        issue566_map_counts before_map2 = issue566_maps();
        started_us = datetime_monotonic_us();
        draw_client_map(pl);
        strakewood_timing_add(&timings[3], started_us);
        issue566_map_changes("map2", tick, before_map2);
        strakewood_progress(tick, "queue-drain");
        /* This offline player has no authenticated peer. Exercise MAP2 and
         * simulation, but do not pass its dummy socket through live transport. */
        socket_buffer_clear(CONTR(pl)->cs);
    }
    ck_assert_uint_eq(autosaves, ticks / (AUTOSAVE + 1));
    ck_assert_int_eq(fclose(checkpoint), 0);
    free(checkpoint_path);
    strakewood_progress(ticks, "final-save");
    started_us = datetime_monotonic_us();
    ck_assert(player_save_checked(pl));
    strakewood_timing_add(&timings[5], started_us);
    if (glow_option != NULL) {
        ck_assert_int_eq(pl->glow_radius, requested_glow);
    }
    if (durability_benchmark) {
        strakewood_progress(ticks, "durability-benchmark");
        issue566_durability_benchmark(pl, &timings[6]);
    }
    for (size_t i = 0; i < arraysize(timing_stages); i++) {
        strakewood_timing_report(&timings[i]);
    }
    printf("ISSUE566 movement attempts=%zu successes=%u glow_radius=%d autosaves=%u\n",
           timings[4].count, movement_successes, pl->glow_radius, autosaves);
    if (route_mode) {
        size_t maps_visited = 0;
        for (size_t m = 0; m < arraysize(route_visited); m++) {
            maps_visited += route_visited[m];
        }
        ck_assert_uint_eq(movement_successes, route_count - 1U);
        printf("ISSUE566 offline planned traversal steps=%u maps_visited=%zu complete=1\n",
               movement_successes, maps_visited);
    }
    free(route_points);
    fflush(stdout);
    free(timings);
    if (durability_benchmark) {
        strakewood_progress(ticks, "journal-deinit");
        ck_assert(gameplay_journal_deinit_checked());
    }
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
        TCase *tc_auth = tcase_create("Pending authentication");
        tcase_set_timeout(tc_auth, 45);
        tcase_add_unchecked_fixture(tc_auth, check_setup, check_teardown);
        tcase_add_checked_fixture(tc_auth, check_test_setup, transport_auth_teardown);
        tcase_add_test(tc_auth, test_quic_login_and_movement_continue_while_authentication_pending);
        suite_add_tcase(s, tc_auth);
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
