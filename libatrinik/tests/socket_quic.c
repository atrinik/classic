#include "../socket_private.h"

#include <toolkit/datetime.h>
#include <toolkit/path.h>
#include <toolkit/socket.h>
#include <toolkit/toolkit.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

#define QUIC_TEST_CLIENTS 5U
#define QUIC_TEST_TIMEOUT_MS UINT64_C(5000)

typedef struct quic_test_client {
    uint16_t port;
    const char *fingerprint;
    atomic_uint *started;
    atomic_uint *completed;
    socket_t *connection;
} quic_test_client_t;

typedef struct quic_test_server {
    socket_t *listener;
    socket_t **accepted;
    size_t count;
    bool delay_accept;
    atomic_uint *clients_completed;
    atomic_uint *pending;
    bool failed;
} quic_test_server_t;

static int quic_test_pending_connection(SSL_CTX *ctx, SSL *connection, void *data) {
    atomic_uint *pending = data;
    atomic_fetch_add(pending, 1U);
    return 1;
}

static void *quic_test_client_main(void *data) {
    quic_test_client_t *client = data;
    atomic_fetch_add(client->started, 1U);
    client->connection = socket_quic_client_create("127.0.0.1",
                                                   client->port,
                                                   client->fingerprint,
                                                   NULL,
                                                   NULL,
                                                   NULL,
                                                   SOCKET_CONNECTION_PREFERENCE_DIRECTORY,
                                                   NULL);
    atomic_fetch_add(client->completed, 1U);
    return NULL;
}

static void quic_test_service(socket_t **connections, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (connections[i] != NULL) {
            unsigned int timeout = socket_quic_timeout(connections[i], 1);
            bool ready = socket_wait(connections[i], true, true, timeout);
            socket_quic_service(connections[i], ready, true);
        }
    }
}

static void quic_test_timeout_deadline(void) {
    socket_t connection = {0};
    connection.transport = SOCKET_TRANSPORT_QUIC_CONNECTION;
    connection.quic_event_deadline_ms = datetime_monotonic_ms() + 1000U;
    REQUIRE(socket_quic_timeout(&connection, 5000U) <= 1000U);
    connection.quic_event_deadline_ms = 0;
    REQUIRE(socket_quic_timeout(&connection, 5000U) == 0U);
}

static void quic_test_pending_stream_timeout(socket_t *client, socket_t *server) {
    socket_stream_t *asset = socket_stream_open(client, SOCKET_STREAM_ASSET);
    REQUIRE(asset != NULL);

    uint8_t value = UINT8_C(0xa5);
    size_t written = 0;
    uint64_t deadline = datetime_monotonic_ms() + QUIC_TEST_TIMEOUT_MS;
    while (SSL_get_accept_stream_queue_len(server->quic) == 0 &&
           datetime_monotonic_ms() < deadline) {
        if (written == 0) {
            socket_stream_result_t result =
                socket_stream_write(asset, &value, sizeof(value), &written);
            REQUIRE(result == SOCKET_STREAM_RESULT_OK ||
                    result == SOCKET_STREAM_RESULT_WOULD_BLOCK);
        }

        bool client_ready = socket_wait(client, true, true, 1);
        socket_quic_service(client, client_ready, true);
        bool server_ready = socket_wait(server, true, true, 1);
        socket_quic_service(server, server_ready, false);
    }

    REQUIRE(written == sizeof(value));
    REQUIRE(SSL_get_accept_stream_queue_len(server->quic) != 0);

    /* The transport loop cannot classify this queue; the asset lane does so
     * on the next simulation pass. It must not turn the wait into a spin. */
    server->quic_event_deadline_ms = UINT64_MAX;
    REQUIRE(socket_quic_timeout(server, 1000U) == 1000U);
    socket_stream_destroy(asset);
}

static void *quic_test_server_main(void *data) {
    quic_test_server_t *server = data;
    uint64_t deadline = datetime_monotonic_ms() + QUIC_TEST_TIMEOUT_MS;
    if (server->delay_accept) {
        while ((atomic_load(server->pending) != server->count ||
                atomic_load(server->clients_completed) != server->count) &&
               datetime_monotonic_ms() < deadline) {
            socket_wait(server->listener, true, false, 10);
            if (SSL_handle_events(server->listener->quic) != 1) {
                server->failed = true;
                return NULL;
            }
        }
        if (atomic_load(server->pending) != server->count ||
            atomic_load(server->clients_completed) != server->count) {
            server->failed = true;
            return NULL;
        }
    }

    for (size_t i = 0; i < server->count; i++) {
        while (server->accepted[i] == NULL && datetime_monotonic_ms() < deadline) {
            socket_wait(server->listener, true, false, 10);
            server->accepted[i] = socket_accept(server->listener);
        }
        if (server->accepted[i] == NULL) {
            server->failed = true;
            return NULL;
        }
    }
    while (atomic_load(server->clients_completed) != server->count &&
           datetime_monotonic_ms() < deadline) {
        quic_test_service(server->accepted, server->count);
    }
    server->failed = atomic_load(server->clients_completed) != server->count;
    return NULL;
}

static void quic_test_run(size_t count, bool delay_accept) {
    char directory[HUGE_BUF];
#ifdef WIN32
    char temporary_root[HUGE_BUF];
    DWORD temporary_root_length = GetTempPathA(sizeof(temporary_root), temporary_root);
    REQUIRE(temporary_root_length > 0 && temporary_root_length < sizeof(temporary_root));
    int directory_length = snprintf(directory,
                                    sizeof(directory),
                                    "%satrinik-libatrinik-quic-%lu",
                                    temporary_root,
                                    (unsigned long)GetCurrentProcessId());
    REQUIRE(directory_length > 0 && (size_t)directory_length < sizeof(directory));
    REQUIRE(CreateDirectoryA(directory, NULL));
#else
    snprintf(directory, sizeof(directory), "/tmp/atrinik-libatrinik-quic-XXXXXX");
    REQUIRE(mkdtemp(directory) != NULL);
#endif
    char identity[4096];
    int identity_length = snprintf(identity, sizeof(identity), "%s/identity.pem", directory);
    REQUIRE(identity_length > 0 && (size_t)identity_length < sizeof(identity));

    socket_t *listener = socket_quic_server_create("127.0.0.1", 0, false, identity);
    REQUIRE(listener != NULL);
    uint16_t port = 0;
    char fingerprint[65];
    REQUIRE(socket_local_port(listener, &port));
    REQUIRE(socket_certificate_sha256(listener, fingerprint));

    atomic_uint started;
    atomic_uint completed;
    atomic_uint pending;
    atomic_init(&started, 0U);
    atomic_init(&completed, 0U);
    atomic_init(&pending, 0U);
    SSL_CTX_set_new_pending_conn_cb(listener->quic_ctx, quic_test_pending_connection, &pending);
    quic_test_client_t clients[QUIC_TEST_CLIENTS] = {0};
    pthread_t threads[QUIC_TEST_CLIENTS];
    socket_t *accepted[QUIC_TEST_CLIENTS] = {0};
    quic_test_server_t server = {
        .listener = listener,
        .accepted = accepted,
        .count = count,
        .delay_accept = delay_accept,
        .clients_completed = &completed,
        .pending = &pending,
    };
    pthread_t server_thread;
    REQUIRE(pthread_create(&server_thread, NULL, quic_test_server_main, &server) == 0);
    for (size_t i = 0; i < count; i++) {
        clients[i] = (quic_test_client_t){
            .port = port,
            .fingerprint = fingerprint,
            .started = &started,
            .completed = &completed,
        };
        if (count == 1U) {
            quic_test_client_main(&clients[i]);
        } else {
            REQUIRE(pthread_create(&threads[i], NULL, quic_test_client_main, &clients[i]) == 0);
        }
    }

    uint64_t deadline = datetime_monotonic_ms() + QUIC_TEST_TIMEOUT_MS;
    while (atomic_load(&started) != count && datetime_monotonic_ms() < deadline) {
        usleep(1000);
    }
    REQUIRE(atomic_load(&started) == count);
    for (size_t i = 0; i < count; i++) {
        if (count != 1U) {
            REQUIRE(pthread_join(threads[i], NULL) == 0);
        }
        REQUIRE(clients[i].connection != NULL);
    }
    REQUIRE(pthread_join(server_thread, NULL) == 0);
    REQUIRE(!server.failed);

    quic_test_pending_stream_timeout(clients[0].connection, accepted[0]);

    /* Force an otherwise idle connection's QUIC timer due. The server-side
     * service call must make progress without a readable UDP handle. */
    accepted[0]->quic_event_deadline_ms = 0;
    REQUIRE(socket_quic_timeout(accepted[0], 1000U) == 0U);
    REQUIRE(socket_quic_timer_due(accepted[0]));
    REQUIRE(socket_quic_service(accepted[0], false, false));

    bool sent[QUIC_TEST_CLIENTS] = {0};
    bool received[QUIC_TEST_CLIENTS] = {0};
    socket_t *accepted_by_client[QUIC_TEST_CLIENTS] = {0};
    size_t received_count = 0;
    while (received_count != count && datetime_monotonic_ms() < deadline) {
        for (size_t i = 0; i < count; i++) {
            if (!sent[i]) {
                uint8_t value = (uint8_t)i;
                size_t amount = 0;
                REQUIRE(socket_write(clients[i].connection, &value, sizeof(value), &amount));
                sent[i] = amount == sizeof(value);
            }
        }
        quic_test_service(accepted, count);
        for (size_t i = 0; i < count; i++) {
            uint8_t value = 0;
            size_t amount = 0;
            REQUIRE(socket_read(accepted[i], &value, sizeof(value), &amount));
            if (amount == sizeof(value)) {
                REQUIRE(value < count);
                REQUIRE(!received[value]);
                received[value] = true;
                accepted_by_client[value] = accepted[i];
                received_count++;
            }
        }
    }
    REQUIRE(received_count == count);

    for (size_t i = 1; i < count; i++) {
        socket_destroy(clients[i].connection);
        clients[i].connection = NULL;
    }
    socket_t *selected = accepted_by_client[0];
    for (size_t i = 0; i < count; i++) {
        if (accepted[i] == selected) {
            accepted[i] = NULL;
            break;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (accepted[i] != NULL) {
            socket_destroy(accepted[i]);
        }
    }
    uint8_t repeat = UINT8_C(0xa5);
    size_t repeat_written = 0;
    size_t repeat_read = 0;
    uint8_t repeat_received = 0;
    deadline = datetime_monotonic_ms() + QUIC_TEST_TIMEOUT_MS;
    while (repeat_read == 0 && datetime_monotonic_ms() < deadline) {
        if (repeat_written == 0) {
            REQUIRE(socket_write(clients[0].connection, &repeat, sizeof(repeat), &repeat_written));
        }
        bool ready = socket_wait(selected, true, true, 1);
        socket_quic_service(selected, ready, true);
        REQUIRE(socket_read(selected, &repeat_received, sizeof(repeat_received), &repeat_read));
    }
    REQUIRE(repeat_written == sizeof(repeat));
    REQUIRE(repeat_read == sizeof(repeat_received));
    REQUIRE(repeat_received == repeat);

    socket_destroy(clients[0].connection);
    socket_destroy(selected);
    socket_destroy(listener);
    REQUIRE(unlink(identity) == 0);
#ifdef WIN32
    REQUIRE(RemoveDirectoryA(directory));
#else
    REQUIRE(rmdir(directory) == 0);
#endif
}

int main(void) {
    toolkit_import(path);
    toolkit_import(socket);
    quic_test_timeout_deadline();
    quic_test_run(1U, false);
    quic_test_run(QUIC_TEST_CLIENTS, true);
    toolkit_deinit();
    return 0;
}
