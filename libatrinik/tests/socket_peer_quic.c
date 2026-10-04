/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "socket_peer_quic_fixture.h"

#include "../socket_private.h"

#include <toolkit/datetime.h>
#include <toolkit/path.h>
#include <toolkit/socket.h>
#include <toolkit/toolkit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                 \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                       \
        }                                                                                  \
    } while (0)

#define PEER_COUNT 2U
#define TIMEOUT_MS UINT64_C(10000)
#define ROUNDS 8U

/* Both clients remain live on the same listener throughout the test. Their
 * explicitly bound source addresses make a shared UDP BIO's last sender an
 * invalid substitute for the accepted QUIC connection's current peer. */
typedef struct peer_client {
    socket_t *socket;
    struct sockaddr_in source;
    bool connected;
} peer_client_t;

static peer_client_t peer_client_create(const char *host, uint16_t port) {
    peer_client_t client = {0};
    client.socket = calloc(1, sizeof(*client.socket));
    REQUIRE(client.socket != NULL);
    socket_t *sc = client.socket;
    sc->handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(sc->handle != -1);
    sc->owns_handle = true;
    sc->transport = SOCKET_TRANSPORT_QUIC_CONNECTION;
    sc->connection_mode = SOCKET_CONNECTION_MODE_QUIC;
    sc->role = SOCKET_ROLE_CLIENT;
    client.source.sin_family = AF_INET;
    REQUIRE(inet_pton(AF_INET, host, &client.source.sin_addr) == 1);
    REQUIRE(bind(sc->handle, (struct sockaddr *)&client.source, sizeof(client.source)) == 0);
    socklen_t source_size = sizeof(client.source);
    REQUIRE(getsockname(sc->handle, (struct sockaddr *)&client.source, &source_size) == 0);
    REQUIRE(source_size == sizeof(client.source));
    REQUIRE(client.source.sin_port != 0);
    REQUIRE(socket_opt_non_blocking(sc, true));

    sc->quic_ctx = SSL_CTX_new(OSSL_QUIC_client_method());
    REQUIRE(sc->quic_ctx != NULL);
    /* This isolated synthetic identity is checked against its exact fingerprint
     * after the handshake and before any application payload is sent. */
    SSL_CTX_set_verify(sc->quic_ctx, SSL_VERIFY_NONE, NULL);
    sc->quic = SSL_new(sc->quic_ctx);
    REQUIRE(sc->quic != NULL);
    BIO *network = BIO_new_dgram(sc->handle, BIO_NOCLOSE);
    REQUIRE(network != NULL);
    SSL_set_bio(sc->quic, network, network);
    BIO_ADDR *peer = BIO_ADDR_new();
    REQUIRE(peer != NULL);
    struct in_addr destination;
    REQUIRE(inet_pton(AF_INET, "127.0.0.1", &destination) == 1);
    REQUIRE(BIO_ADDR_rawmake(peer, AF_INET, &destination, sizeof(destination), htons(port)) == 1);
    static const unsigned char alpn[] = {9, 'a', 't', 'r', 'i', 'n', 'i', 'k', '/', '2'};
    REQUIRE(SSL_set_default_stream_mode(sc->quic, SSL_DEFAULT_STREAM_MODE_NONE) == 1);
    REQUIRE(SSL_set_blocking_mode(sc->quic, 0) == 1);
    REQUIRE(SSL_set_alpn_protos(sc->quic, alpn, sizeof(alpn)) == 0);
    REQUIRE(SSL_set1_initial_peer_addr(sc->quic, peer) == 1);
    BIO_ADDR_free(peer);
    return client;
}

static bool peer_matches(socket_t *sc, const struct sockaddr_in *expected) {
    if (sc->addr.ss_family != AF_INET) {
        return false;
    }
    const struct sockaddr_in *actual = (const struct sockaddr_in *)&sc->addr;
    return actual->sin_port == expected->sin_port &&
           actual->sin_addr.s_addr == expected->sin_addr.s_addr;
}

static void peer_assert_all(socket_t **accepted,
                            const peer_client_t *clients,
                            socket_peer_quic_observer_t observe) {
    for (size_t i = 0; i < PEER_COUNT; i++) {
        REQUIRE(socket_refresh_peer_addr(accepted[i]));
        REQUIRE(peer_matches(accepted[i], &clients[i].source));
        if (observe != NULL) {
            observe(accepted[i], i);
        }
    }
}

static void peer_service(socket_t **accepted,
                         const peer_client_t *clients,
                         bool reverse,
                         socket_peer_quic_observer_t observe) {
    for (size_t n = 0; n < PEER_COUNT; n++) {
        size_t i = reverse ? PEER_COUNT - 1U - n : n;
        REQUIRE(SSL_handle_events(clients[i].socket->quic) == 1);
        bool ready = socket_wait(accepted[i], true, true, 1);
        socket_quic_service(accepted[i], ready, true);
        /* Refresh both children after each event: the most recent sender must
         * never replace the other child's peer, including its source port. */
        peer_assert_all(accepted, clients, observe);
    }
}

static void peer_exchange(socket_t **accepted,
                          const peer_client_t *clients,
                          unsigned int round,
                          socket_peer_quic_observer_t observe) {
    size_t sent[PEER_COUNT] = {0};
    size_t received[PEER_COUNT] = {0};
    size_t echoed[PEER_COUNT] = {0};
    size_t returned[PEER_COUNT] = {0};
    uint8_t values[PEER_COUNT];
    uint64_t deadline = datetime_monotonic_ms() + TIMEOUT_MS;
    for (size_t i = 0; i < PEER_COUNT; i++) {
        values[i] = (uint8_t)(round * PEER_COUNT + i + 1U);
    }
    while ((returned[0] == 0 || returned[1] == 0) && datetime_monotonic_ms() < deadline) {
        for (size_t n = 0; n < PEER_COUNT; n++) {
            size_t i = round % 2U != 0 ? PEER_COUNT - 1U - n : n;
            if (sent[i] == 0) {
                REQUIRE(socket_write(clients[i].socket, &values[i], 1, &sent[i]));
            }
            if (received[i] == 0) {
                uint8_t value = 0;
                REQUIRE(socket_read(accepted[i], &value, 1, &received[i]));
                if (received[i] != 0) {
                    REQUIRE(value == values[i]);
                }
            }
            if (received[i] != 0 && echoed[i] == 0) {
                REQUIRE(socket_write(accepted[i], &values[i], 1, &echoed[i]));
            }
            if (returned[i] == 0) {
                uint8_t value = 0;
                REQUIRE(socket_read(clients[i].socket, &value, 1, &returned[i]));
                if (returned[i] != 0) {
                    REQUIRE(value == values[i]);
                }
            }
            peer_assert_all(accepted, clients, observe);
        }
        peer_service(accepted, clients, round % 2U != 0, observe);
    }
    for (size_t i = 0; i < PEER_COUNT; i++) {
        REQUIRE(sent[i] == 1 && received[i] == 1 && echoed[i] == 1 && returned[i] == 1);
        /* A refresh must replace a stale value, rather than merely preserving
         * an address that happened to be correct at accept time. */
        memset(&accepted[i]->addr, 0, sizeof(accepted[i]->addr));
        memcpy(&accepted[i]->addr, &clients[1U - i].source, sizeof(clients[i].source));
        REQUIRE(socket_refresh_peer_addr(accepted[i]));
        REQUIRE(peer_matches(accepted[i], &clients[i].source));
        if (observe != NULL) {
            observe(accepted[i], i);
        }
    }
}

int socket_peer_quic_fixture_run(socket_peer_quic_observer_t observe) {
    toolkit_import(path);
    toolkit_import(socket);
    char directory[HUGE_BUF];
#ifdef WIN32
    char temporary_root[HUGE_BUF];
    DWORD root_length = GetTempPathA(sizeof(temporary_root), temporary_root);
    REQUIRE(root_length > 0 && root_length < sizeof(temporary_root));
    int length = snprintf(directory, sizeof(directory), "%satrinik-peer-quic-%lu",
                          temporary_root, (unsigned long)GetCurrentProcessId());
    REQUIRE(length > 0 && (size_t)length < sizeof(directory));
    REQUIRE(CreateDirectoryA(directory, NULL));
#else
    snprintf(directory, sizeof(directory), "/tmp/atrinik-peer-quic-XXXXXX");
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
    peer_client_t clients[PEER_COUNT] = {
        peer_client_create("127.0.0.2", port),
        peer_client_create("127.0.0.3", port),
    };
    socket_t *accepted[PEER_COUNT] = {0};
    size_t connected = 0;
    size_t accepted_count = 0;
    uint64_t deadline = datetime_monotonic_ms() + TIMEOUT_MS;
    while ((connected != PEER_COUNT || accepted_count != PEER_COUNT) &&
           datetime_monotonic_ms() < deadline) {
        for (size_t i = 0; i < PEER_COUNT; i++) {
            if (!clients[i].connected) {
                int result = SSL_connect(clients[i].socket->quic);
                if (result == 1) {
                    char actual_fingerprint[65];
                    REQUIRE(socket_certificate_sha256(clients[i].socket, actual_fingerprint));
                    REQUIRE(strcmp(actual_fingerprint, fingerprint) == 0);
                    clients[i].connected = true;
                    connected++;
                } else {
                    int error = SSL_get_error(clients[i].socket->quic, result);
                    REQUIRE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
                }
            } else {
                REQUIRE(SSL_handle_events(clients[i].socket->quic) == 1);
            }
        }
        socket_wait(listener, true, false, 1);
        REQUIRE(SSL_handle_events(listener->quic) == 1);
        socket_t *connection;
        while ((connection = socket_accept(listener)) != NULL) {
            REQUIRE(accepted_count < PEER_COUNT);
            REQUIRE(connection->handle == listener->handle);
            REQUIRE(!connection->owns_handle);
            size_t i;
            for (i = 0; i < PEER_COUNT; i++) {
                if (peer_matches(connection, &clients[i].source)) {
                    break;
                }
            }
            REQUIRE(i < PEER_COUNT);
            REQUIRE(accepted[i] == NULL);
            accepted[i] = connection;
            accepted_count++;
        }
        for (size_t i = 0; i < PEER_COUNT; i++) {
            if (accepted[i] != NULL) {
                REQUIRE(SSL_handle_events(accepted[i]->quic) == 1);
                REQUIRE(socket_refresh_peer_addr(accepted[i]));
                REQUIRE(peer_matches(accepted[i], &clients[i].source));
                if (observe != NULL) {
                    observe(accepted[i], i);
                }
            }
        }
    }
    REQUIRE(connected == PEER_COUNT && accepted_count == PEER_COUNT);
    for (unsigned int round = 0; round < ROUNDS; round++) {
        peer_exchange(accepted, clients, round, observe);
    }
    for (size_t i = 0; i < PEER_COUNT; i++) {
        socket_destroy(clients[i].socket);
        socket_destroy(accepted[i]);
    }
    socket_destroy(listener);
    REQUIRE(unlink(identity) == 0);
#ifdef WIN32
    REQUIRE(RemoveDirectoryA(directory));
#else
    REQUIRE(rmdir(directory) == 0);
#endif
    toolkit_deinit();
    return 0;
}
