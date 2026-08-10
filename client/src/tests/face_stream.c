/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/** @file Live QUIC cold-cache isolation regression. */

#include <global.h>
#include <asset.h>
#include <client_socket.h>
#include <network_graph.h>
#include <toolkit/datetime.h>
#include <toolkit/packet.h>
#include <toolkit/path.h>
#include <toolkit/string.h>
#include <openssl/evp.h>

#define FACE_COUNT 6U
#define FACE_BODY_SIZE 32768U
#define FACE_CHUNK_SIZE 256U
#define GENERIC_ASSET_PATH "tests/generic.bin"
#define GENERIC_REQUEST_ID UINT16_MAX
#define INITIAL_FACE_COUNT ASSET_STREAM_ACTIVE_MAX
#define REQUEST_COUNT (FACE_COUNT + 1U)
#define TEST_TIMEOUT_MS UINT64_C(15000)

Client_Player cpl;
client_socket_t csocket;

char *file_path(const char *path, const char *mode) {
    (void)mode;
    return xstrdup(path);
}

void network_graph_update(int type, int traffic, size_t bytes) {
    (void)type;
    (void)traffic;
    (void)bytes;
}

typedef struct test_asset_stream {
    socket_stream_t *stream;
    uint8_t request[SOCKET_ASSET_REQUEST_MAX_SIZE];
    size_t request_size;
    packet_struct *header;
    size_t header_pos;
    size_t body_pos;
    uint16_t request_id;
    bool request_complete;
} test_asset_stream_t;

typedef struct test_server {
    socket_t *listener;
    uint8_t body[FACE_BODY_SIZE];
    uint8_t digest[ASSET_DIGEST_SIZE];
    unsigned int maximum_active;
    unsigned int requested;
    unsigned int completed;
    unsigned int gameplay_echoes;
    uint16_t request_order[REQUEST_COUNT];
    uint16_t completion_order[REQUEST_COUNT];
    bool failed;
} test_server_t;

static void test_stream_clear(test_asset_stream_t *state) {
    if (state->stream != NULL) {
        socket_stream_destroy(state->stream);
    }
    if (state->header != NULL) {
        packet_free(state->header);
    }
    memset(state, 0, sizeof(*state));
}

static bool test_stream_prepare(test_asset_stream_t *state, test_server_t *server) {
    socket_asset_request_t request;
    uint16_t face = 0;
    if (!socket_asset_request_parse(state->request, state->request_size, 0, &request)) {
        return false;
    }
    bool valid_face =
        socket_asset_face_path_parse(request.path, &face) && face != 0 && face <= FACE_COUNT;
    if (!valid_face && strcmp(request.path, GENERIC_ASSET_PATH) != 0) {
        return false;
    }
    if (server->requested >= REQUEST_COUNT) {
        return false;
    }
    state->request_id = valid_face ? face : GENERIC_REQUEST_ID;
    server->request_order[server->requested++] = state->request_id;
    state->header = packet_new(0, SOCKET_ASSET_RESPONSE_HEADER_SIZE, 0);
    socket_asset_response_append_ok(state->header, FACE_BODY_SIZE, server->digest);
    if (!packet_writer_finish(state->header)) {
        return false;
    }
    state->request_complete = true;
    return true;
}

static bool test_stream_service(test_asset_stream_t *state, test_server_t *server) {
    if (!state->request_complete) {
        size_t amount = 0;
        socket_stream_result_t result =
            socket_stream_read(state->stream,
                               state->request + state->request_size,
                               sizeof(state->request) - state->request_size,
                               &amount);
        if (result == SOCKET_STREAM_RESULT_ERROR) {
            return false;
        }
        state->request_size += amount;
        if (result == SOCKET_STREAM_RESULT_FINISHED) {
            return test_stream_prepare(state, server);
        }
        return state->request_size < sizeof(state->request) || amount == 0;
    }

    const uint8_t *data;
    size_t remaining;
    if (state->header != NULL) {
        data = state->header->data + state->header_pos;
        remaining = state->header->len - state->header_pos;
    } else {
        data = server->body + state->body_pos;
        remaining = MIN((size_t)FACE_CHUNK_SIZE, sizeof(server->body) - state->body_pos);
    }
    size_t amount = 0;
    socket_stream_result_t result = socket_stream_write(state->stream, data, remaining, &amount);
    if (result == SOCKET_STREAM_RESULT_ERROR || result == SOCKET_STREAM_RESULT_FINISHED) {
        return false;
    }
    if (state->header != NULL) {
        state->header_pos += amount;
        if (state->header_pos == state->header->len) {
            packet_free(state->header);
            state->header = NULL;
        }
    } else {
        state->body_pos += amount;
        if (state->body_pos == sizeof(server->body)) {
            return socket_stream_conclude(state->stream);
        }
    }
    return true;
}

static void *test_server_main(void *data) {
    test_server_t *server = data;
    uint64_t deadline = datetime_monotonic_ms() + TEST_TIMEOUT_MS;
    socket_t *connection = NULL;
    while (connection == NULL && datetime_monotonic_ms() < deadline) {
        socket_wait(server->listener, true, false, 10);
        connection = socket_accept(server->listener);
    }
    if (connection == NULL) {
        server->failed = true;
        return NULL;
    }

    test_asset_stream_t streams[ASSET_STREAM_ACTIVE_MAX] = {0};
    uint64_t completed_at = 0;
    while (datetime_monotonic_ms() < deadline &&
           (completed_at == 0 || datetime_monotonic_ms() - completed_at < 500U)) {
        bool ready = socket_wait(connection, true, true, 2);
        socket_quic_service(connection, ready, true);

        uint8_t gameplay[64];
        size_t gameplay_size = 0;
        if (!socket_read(connection, gameplay, sizeof(gameplay), &gameplay_size)) {
            server->failed = server->completed != REQUEST_COUNT;
            break;
        }
        size_t gameplay_pos = 0;
        while (gameplay_pos < gameplay_size) {
            size_t amount = 0;
            if (!socket_write(connection,
                              gameplay + gameplay_pos,
                              gameplay_size - gameplay_pos,
                              &amount)) {
                server->failed = true;
                break;
            }
            gameplay_pos += amount;
            server->gameplay_echoes += (unsigned int)amount;
            if (amount == 0) {
                break;
            }
        }

        unsigned int active = 0;
        for (size_t i = 0; i < arraysize(streams); i++) {
            if (streams[i].stream == NULL) {
                streams[i].stream = socket_stream_accept(connection, SOCKET_STREAM_ASSET);
            }
            if (streams[i].stream == NULL) {
                continue;
            }
            active++;
            if (!test_stream_service(&streams[i], server)) {
                server->failed = true;
                break;
            }
            if (streams[i].request_complete && streams[i].header == NULL &&
                streams[i].body_pos == sizeof(server->body)) {
                if (server->completed >= REQUEST_COUNT) {
                    server->failed = true;
                    break;
                }
                server->completion_order[server->completed] = streams[i].request_id;
                test_stream_clear(&streams[i]);
                server->completed++;
                active--;
                if (server->completed == REQUEST_COUNT) {
                    completed_at = datetime_monotonic_ms();
                }
            }
        }
        server->maximum_active = MAX(server->maximum_active, active);
        if (server->failed) {
            break;
        }
        usleep(1000);
    }

    for (size_t i = 0; i < arraysize(streams); i++) {
        test_stream_clear(&streams[i]);
    }
    if (server->completed != REQUEST_COUNT) {
        server->failed = true;
    }
    socket_destroy(connection);
    return NULL;
}

int main(void) {
#if OPENSSL_VERSION_NUMBER < 0x30500000L
    return 77;
#else
    toolkit_import(logger);
    toolkit_import(packet);
    toolkit_import(path);
    toolkit_import(socket);

    char directory[] = "/tmp/atrinik-face-stream-XXXXXX";
    if (mkdtemp(directory) == NULL) {
        return 1;
    }
    char identity[HUGE_BUF];
    snprintf(VS(identity), "%s/identity.pem", directory);

    test_server_t server = {0};
    for (size_t i = 0; i < sizeof(server.body); i++) {
        server.body[i] = (uint8_t)i;
    }
    unsigned int digest_size = 0;
    if (EVP_Digest(server.body,
                   sizeof(server.body),
                   server.digest,
                   &digest_size,
                   EVP_sha256(),
                   NULL) != 1 ||
        digest_size != sizeof(server.digest)) {
        return 1;
    }
    server.listener = socket_quic_server_create("127.0.0.1", 0, false, identity);
    uint16_t port = 0;
    char fingerprint[65];
    if (server.listener == NULL || !socket_local_port(server.listener, &port) || port == 0 ||
        !socket_certificate_sha256(server.listener, fingerprint)) {
        return 1;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, test_server_main, &server) != 0) {
        return 1;
    }
    socket_connect_failure_t failure = {0};
    csocket.sc = socket_quic_client_create("127.0.0.1",
                                           port,
                                           fingerprint,
                                           NULL,
                                           NULL,
                                           NULL,
                                           SOCKET_CONNECTION_PREFERENCE_DIRECTORY,
                                           &failure);
    cpl.asset_transport = true;
    bool ok = csocket.sc != NULL;
    char unavailable_path[32];
    if (ok) {
        ok = !asset_requests_available() &&
             socket_asset_face_path_format(VS(unavailable_path), FACE_COUNT + 1U) &&
             asset_request_start_bounded(unavailable_path, ASSET_FACE_MAX_SIZE) == NULL;
    }
    if (ok) {
        asset_requests_connect(csocket.sc);
        ok = asset_requests_available();
    }
    asset_request_t *requests[FACE_COUNT] = {0};
    for (uint16_t face = 1; ok && face <= INITIAL_FACE_COUNT; face++) {
        char path[32];
        ok = socket_asset_face_path_format(VS(path), face);
        requests[face - 1U] = ok ? asset_request_start_bounded(path, ASSET_FACE_MAX_SIZE) : NULL;
        ok = requests[face - 1U] != NULL;
    }

    /* Saturate the three stream slots before interleaving a generic asset and
     * the remainder of the face burst. This makes the server-observed request
     * order a direct regression for shared-scheduler FIFO fairness. */
    if (ok) {
        bool write_pending = false;
        asset_requests_service(csocket.sc, &write_pending);
    }
    asset_request_t *generic = ok ? asset_request_start(GENERIC_ASSET_PATH) : NULL;
    ok = ok && generic != NULL;
    for (uint16_t face = INITIAL_FACE_COUNT + 1U; ok && face <= FACE_COUNT; face++) {
        char path[32];
        ok = socket_asset_face_path_format(VS(path), face);
        requests[face - 1U] = ok ? asset_request_start_bounded(path, ASSET_FACE_MAX_SIZE) : NULL;
        ok = requests[face - 1U] != NULL;
    }
    char duplicate_path[32];
    asset_request_t *duplicate = NULL;
    if (ok) {
        ok = socket_asset_face_path_format(VS(duplicate_path), 1);
        if (ok) {
            duplicate = asset_request_start_bounded(duplicate_path, ASSET_FACE_MAX_SIZE);
            ok = duplicate == requests[0];
        }
    }
    asset_request_free(duplicate);

    uint64_t deadline = datetime_monotonic_ms() + TEST_TIMEOUT_MS;
    unsigned int gameplay_sent = 0;
    unsigned int gameplay_received = 0;
    bool gameplay_during_transfer = false;
    while (ok && gameplay_received < 32U && datetime_monotonic_ms() < deadline) {
        uint8_t tick = (uint8_t)gameplay_sent;
        size_t amount = 0;
        if (gameplay_sent < 64U && socket_write(csocket.sc, &tick, 1, &amount)) {
            gameplay_sent += (unsigned int)amount;
        }
        uint8_t echo[64];
        amount = 0;
        ok = socket_read(csocket.sc, echo, sizeof(echo), &amount);
        gameplay_received += (unsigned int)amount;

        bool write_pending = false;
        asset_requests_service(csocket.sc, &write_pending);
        bool all_complete = true;
        for (size_t i = 0; i < arraysize(requests); i++) {
            all_complete &= asset_request_get_state(requests[i]) == ASSET_REQUEST_COMPLETE;
        }
        all_complete &= asset_request_get_state(generic) == ASSET_REQUEST_COMPLETE;
        gameplay_during_transfer |= amount != 0 && !all_complete;
        bool ready = socket_wait(csocket.sc, true, write_pending, 2);
        socket_quic_service(csocket.sc, ready, write_pending);
    }

    for (size_t i = 0; ok && i < arraysize(requests); i++) {
        while (asset_request_get_state(requests[i]) == ASSET_REQUEST_PENDING &&
               datetime_monotonic_ms() < deadline) {
            bool write_pending = false;
            asset_requests_service(csocket.sc, &write_pending);
            bool ready = socket_wait(csocket.sc, true, write_pending, 2);
            socket_quic_service(csocket.sc, ready, write_pending);
        }
        size_t size = 0;
        const uint8_t *body = asset_request_get_data(requests[i], &size);
        ok = asset_request_get_state(requests[i]) == ASSET_REQUEST_COMPLETE &&
             size == sizeof(server.body) && body != NULL &&
             memcmp(body, server.body, sizeof(server.body)) == 0;
    }
    while (ok && asset_request_get_state(generic) == ASSET_REQUEST_PENDING &&
           datetime_monotonic_ms() < deadline) {
        bool write_pending = false;
        asset_requests_service(csocket.sc, &write_pending);
        bool ready = socket_wait(csocket.sc, true, write_pending, 2);
        socket_quic_service(csocket.sc, ready, write_pending);
    }
    size_t generic_size = 0;
    const uint8_t *generic_body = asset_request_get_data(generic, &generic_size);
    ok = ok && asset_request_get_state(generic) == ASSET_REQUEST_COMPLETE &&
         generic_size == sizeof(server.body) && generic_body != NULL &&
         memcmp(generic_body, server.body, sizeof(server.body)) == 0;

    asset_requests_disconnect();
    ok = ok && !asset_requests_available() &&
         asset_request_start_bounded(unavailable_path, ASSET_FACE_MAX_SIZE) == NULL;
    for (size_t i = 0; i < arraysize(requests); i++) {
        asset_request_free(requests[i]);
    }
    asset_request_free(generic);
    asset_requests_deinit();
    if (csocket.sc != NULL) {
        socket_destroy(csocket.sc);
        csocket.sc = NULL;
    }
    pthread_join(thread, NULL);
    socket_destroy(server.listener);
    unlink(identity);
    rmdir(directory);
    toolkit_deinit();

    size_t generic_request = REQUEST_COUNT;
    size_t final_face_request = REQUEST_COUNT;
    size_t generic_completion = REQUEST_COUNT;
    size_t final_face_completion = REQUEST_COUNT;
    for (size_t i = 0; i < server.requested; i++) {
        if (server.request_order[i] == GENERIC_REQUEST_ID) {
            generic_request = i;
        } else if (server.request_order[i] == FACE_COUNT) {
            final_face_request = i;
        }
    }
    for (size_t i = 0; i < server.completed; i++) {
        if (server.completion_order[i] == GENERIC_REQUEST_ID) {
            generic_completion = i;
        } else if (server.completion_order[i] == FACE_COUNT) {
            final_face_completion = i;
        }
    }
    bool ordering = server.requested == REQUEST_COUNT &&
                    generic_request < INITIAL_FACE_COUNT + ASSET_STREAM_ACTIVE_MAX &&
                    generic_request < final_face_request &&
                    generic_completion < INITIAL_FACE_COUNT + ASSET_STREAM_ACTIVE_MAX &&
                    generic_completion < final_face_completion;
    bool passed = ok && gameplay_during_transfer && gameplay_received >= 32U && !server.failed &&
                  server.maximum_active == ASSET_STREAM_ACTIVE_MAX &&
                  server.completed == REQUEST_COUNT && ordering;
    if (!passed) {
        fprintf(stderr,
                "face-stream regression failed: ok=%d concurrent_gameplay=%d gameplay=%u "
                "server_failed=%d maximum_active=%u requested=%u completed=%u ordering=%d\n",
                ok,
                gameplay_during_transfer,
                gameplay_received,
                server.failed,
                server.maximum_active,
                server.requested,
                server.completed,
                ordering);
    }
    return passed ? 0 : 1;
#endif
}
