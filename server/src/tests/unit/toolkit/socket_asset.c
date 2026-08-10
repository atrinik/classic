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

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <metaserver_internal.h>
#include <initialization.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <toolkit/datetime.h>
#include <toolkit/packet.h>
#include <toolkit/path.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdatomic.h>

#if OPENSSL_VERSION_NUMBER >= 0x30500000L
#define ASSET_LOOPBACK_TIMEOUT_MS UINT64_C(5000)
#define ASSET_LOOPBACK_SERVICE_INTERVAL_MS (MAX_TIME / 1000U)
#define ASSET_LOOPBACK_COMPLETION_CALL_MAX 2U

typedef struct asset_loopback_server {
    socket_t *listener;
    atomic_bool stop;
    atomic_bool accepted;
    atomic_bool asset_service_enabled;
    atomic_bool partial_service_requested;
    atomic_bool partial_service_paused;
    atomic_uint partial_service_max_time_us;
    atomic_bool pending_poll_requested;
    atomic_bool pending_poll_completed;
    atomic_uint pending_classified_streams;
    atomic_uint gameplay_bytes;
    atomic_uint active_asset_streams;
    atomic_uint network_service_calls;
    atomic_uint max_productive_passes;
    bool failed;
    bool served;
    uint8_t transport_capabilities;
    unsigned int completion_service_calls;
} asset_loopback_server_t;

static socket_t *asset_loopback_listener_create(char *directory,
                                                char *identity,
                                                size_t identity_size,
                                                uint16_t *port,
                                                char fingerprint[65]) {
    if (mkdtemp(directory) == NULL) {
        return NULL;
    }
    int length = snprintf(identity, identity_size, "%s/identity.pem", directory);
    if (length < 0 || (size_t)length >= identity_size) {
        rmdir(directory);
        return NULL;
    }

    socket_t *listener = socket_quic_server_create("127.0.0.1", 0, false, identity);
    if (listener != NULL && socket_local_port(listener, port) && *port != 0 &&
        socket_certificate_sha256(listener, fingerprint)) {
        return listener;
    }
    if (listener != NULL) {
        socket_destroy(listener);
    }
    unlink(identity);
    rmdir(directory);
    return NULL;
}

static void *asset_loopback_server_main(void *data) {
    asset_loopback_server_t *server = data;
    uint64_t deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;
    socket_t *connection = NULL;
    while (!atomic_load(&server->stop) && datetime_monotonic_ms() < deadline &&
           connection == NULL) {
        socket_wait(server->listener, true, false, 10);
        connection = socket_accept(server->listener);
    }
    if (connection == NULL) {
        server->failed = !atomic_load(&server->stop);
        return NULL;
    }
    atomic_store(&server->accepted, true);
    deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;

    socket_struct ns = {
        .sc = connection,
        .socket_version = SOCKET_VERSION,
        .join_authenticated = true,
        .setup_completed = true,
        .state = ST_LOGIN,
    };
    socket_assets_connection_register(&ns);
    server->transport_capabilities = ns.asset_transport_capabilities;
    uint64_t next_asset_service_ms = datetime_monotonic_ms();
    unsigned int asset_service_calls = 0;
    unsigned int request_service_call = 0;
    while (!atomic_load(&server->stop) && datetime_monotonic_ms() < deadline) {
        bool ready = socket_wait(connection, true, true, 2);
        socket_quic_service(connection, ready, true);
        atomic_fetch_add(&server->network_service_calls, 1);
        if (atomic_exchange(&server->pending_poll_requested, false)) {
            atomic_store(&server->pending_classified_streams,
                         (unsigned int)socket_stream_poll_pending(connection));
            atomic_store(&server->pending_poll_completed, true);
        }
        uint8_t gameplay[16];
        size_t gameplay_size = 0;
        if (!socket_read(connection, gameplay, sizeof(gameplay), &gameplay_size)) {
            server->failed = true;
            break;
        }
        atomic_fetch_add(&server->gameplay_bytes, (unsigned int)gameplay_size);
        uint64_t now = datetime_monotonic_ms();
        if (atomic_load(&server->asset_service_enabled) && now >= next_asset_service_ms) {
            asset_service_calls++;
            bool partial_service = atomic_load(&server->partial_service_requested);
            long saved_max_time = max_time;
            int saved_max_time_multiplier = max_time_multiplier;
            if (partial_service) {
                max_time = (long)atomic_load(&server->partial_service_max_time_us);
                max_time_multiplier = 1;
            }
            socket_assets_service();
            if (partial_service) {
                max_time = saved_max_time;
                max_time_multiplier = saved_max_time_multiplier;
            }
            atomic_store(&server->active_asset_streams, (unsigned int)ns.asset_stream_count);
            unsigned int maximum = atomic_load(&server->max_productive_passes);
            while (maximum < ns.asset_service_productive_passes &&
                   !atomic_compare_exchange_weak(&server->max_productive_passes,
                                                 &maximum,
                                                 ns.asset_service_productive_passes)) {}
            if (partial_service && ns.asset_service_productive_passes != 0) {
                atomic_store(&server->partial_service_requested, false);
                atomic_store(&server->asset_service_enabled, false);
                atomic_store(&server->partial_service_paused, true);
            }
            if (ns.state == ST_DEAD || ns.state == ST_ZOMBIE) {
                server->failed = true;
                break;
            }
            if (request_service_call == 0 && ns.asset_window_requests != 0) {
                request_service_call = asset_service_calls;
            }
            if (!server->served && request_service_call != 0 && ns.asset_stream_count == 0) {
                server->served = true;
                server->completion_service_calls = asset_service_calls - request_service_call + 1U;
            }
            next_asset_service_ms = now + ASSET_LOOPBACK_SERVICE_INTERVAL_MS;
        }
    }
    if (!atomic_load(&server->stop)) {
        server->failed = true;
    }

    socket_assets_connection_clear(&ns);
    atomic_store(&server->active_asset_streams, 0);
    socket_destroy(connection);
    return NULL;
}

static bool asset_loopback_progress(socket_t *connection, bool write_pending, uint64_t deadline) {
    if (datetime_monotonic_ms() >= deadline) {
        return false;
    }
    bool ready = socket_wait(connection, true, write_pending, 2);
    socket_quic_service(connection, ready, write_pending);
    return true;
}

static bool asset_loopback_poll_pending(socket_t *connection,
                                        asset_loopback_server_t *server,
                                        unsigned int expected,
                                        uint64_t deadline) {
    while (datetime_monotonic_ms() < deadline) {
        atomic_store(&server->pending_poll_completed, false);
        atomic_store(&server->pending_poll_requested, true);
        while (!atomic_load(&server->pending_poll_completed) &&
               datetime_monotonic_ms() < deadline) {
            if (!asset_loopback_progress(connection, true, deadline)) {
                return false;
            }
        }
        if (atomic_load(&server->pending_poll_completed) &&
            atomic_load(&server->pending_classified_streams) == expected) {
            return true;
        }
    }
    return false;
}

static bool asset_loopback_send_request(socket_t *connection,
                                        socket_stream_kind_t kind,
                                        const uint8_t *request,
                                        size_t request_size,
                                        socket_stream_t **stream,
                                        uint64_t deadline) {
    HARD_ASSERT(stream != NULL);

    while (*stream == NULL && datetime_monotonic_ms() < deadline) {
        *stream = socket_stream_open(connection, kind);
        if (*stream == NULL && !asset_loopback_progress(connection, false, deadline)) {
            return false;
        }
    }
    if (*stream == NULL) {
        return false;
    }
    size_t request_pos = 0;
    while (request_pos < request_size) {
        size_t amount = 0;
        socket_stream_result_t result = socket_stream_write(*stream,
                                                            request + request_pos,
                                                            request_size - request_pos,
                                                            &amount);
        if (result == SOCKET_STREAM_RESULT_ERROR || result == SOCKET_STREAM_RESULT_FINISHED) {
            return false;
        }
        request_pos += amount;
        if (amount == 0 && !asset_loopback_progress(connection, true, deadline)) {
            return false;
        }
    }
    return socket_stream_conclude(*stream);
}

static bool asset_loopback_read_exact(socket_t *connection,
                                      socket_stream_t *stream,
                                      uint8_t *data,
                                      size_t size,
                                      uint64_t deadline) {
    size_t pos = 0;
    while (pos < size) {
        size_t amount = 0;
        socket_stream_result_t result = socket_stream_read(stream, data + pos, size - pos, &amount);
        if (result == SOCKET_STREAM_RESULT_ERROR || result == SOCKET_STREAM_RESULT_FINISHED) {
            return false;
        }
        pos += amount;
        if (amount == 0 && !asset_loopback_progress(connection, false, deadline)) {
            return false;
        }
    }
    return true;
}

static bool
asset_loopback_expect_finished(socket_t *connection, socket_stream_t *stream, uint64_t deadline) {
    while (datetime_monotonic_ms() < deadline) {
        uint8_t surplus;
        size_t amount = 0;
        socket_stream_result_t result =
            socket_stream_read(stream, &surplus, sizeof(surplus), &amount);
        if (result == SOCKET_STREAM_RESULT_FINISHED) {
            return amount == 0;
        }
        if (result == SOCKET_STREAM_RESULT_ERROR || amount != 0 ||
            !asset_loopback_progress(connection, false, deadline)) {
            return false;
        }
    }
    return false;
}

static bool asset_loopback_receive_face(socket_t *connection,
                                        uint16_t face,
                                        const uint8_t *expected,
                                        uint32_t expected_size,
                                        const uint8_t expected_digest[ASSET_DIGEST_SIZE]) {
    char path[32];
    if (!socket_asset_face_path_format(VS(path), face)) {
        return false;
    }
    packet_struct *request = packet_new(0, 128, 0);
    static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
    socket_asset_request_append(request, path, 0, empty_digest, 0);
    bool success = packet_writer_finish(request);
    uint64_t deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;
    socket_stream_t *stream = NULL;
    success = success && asset_loopback_send_request(connection,
                                                     SOCKET_STREAM_ASSET,
                                                     request->data,
                                                     request->len,
                                                     &stream,
                                                     deadline);
    packet_free(request);

    uint8_t response_header[SOCKET_ASSET_RESPONSE_HEADER_SIZE];
    success = success && asset_loopback_read_exact(connection,
                                                   stream,
                                                   response_header,
                                                   sizeof(response_header),
                                                   deadline);

    socket_asset_response_t response;
    success = success &&
              socket_asset_response_parse(response_header, sizeof(response_header), 0, &response) &&
              response.status == ASSET_STATUS_OK && response.total_size == expected_size &&
              memcmp(response.digest, expected_digest, ASSET_DIGEST_SIZE) == 0;
    uint8_t *body = success ? xmalloc(expected_size) : NULL;
    success =
        success && asset_loopback_read_exact(connection, stream, body, expected_size, deadline);
    success = success && memcmp(body, expected, expected_size) == 0;

    bool finished = success && asset_loopback_expect_finished(connection, stream, deadline);

    free(body);
    if (stream != NULL) {
        if (!finished) {
            socket_stream_reset(stream, SOCKET_STREAM_ERROR_CANCELLED);
        }
        socket_stream_destroy(stream);
    }
    return success && finished;
}

static bool asset_loopback_read_face_batch(socket_t *connection,
                                           socket_stream_t *stream,
                                           const uint16_t *faces,
                                           size_t count,
                                           uint64_t deadline) {
    HARD_ASSERT(connection != NULL);
    HARD_ASSERT(stream != NULL);

    bool success = true;
    uint32_t aggregate_size = 0;
    for (size_t i = 0; success && i < count; i++) {
        uint8_t header[SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE];
        success = asset_loopback_read_exact(connection, stream, header, sizeof(header), deadline);
        socket_face_batch_response_t response = {0};
        success = success &&
                  socket_face_batch_response_parse(header, sizeof(header), 0, &response) &&
                  response.face == faces[i];

        const uint8_t *expected = NULL;
        const uint8_t *expected_digest = NULL;
        uint32_t expected_size = 0;
        bool found = face_get_asset(faces[i], &expected, &expected_size, &expected_digest);
        if (found) {
            success = success && response.status == ASSET_STATUS_OK &&
                      response.body_size == expected_size &&
                      memcmp(response.digest, expected_digest, ASSET_DIGEST_SIZE) == 0;
            aggregate_size += response.body_size;
            success = success && aggregate_size <= ASSET_FACE_BATCH_MAX_SIZE;
            uint8_t *body = success ? xmalloc(response.body_size) : NULL;
            success =
                success &&
                asset_loopback_read_exact(connection, stream, body, response.body_size, deadline) &&
                memcmp(body, expected, response.body_size) == 0;
            free(body);
        } else {
            static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
            success = success && response.status == ASSET_STATUS_NOT_FOUND &&
                      response.body_size == 0 &&
                      memcmp(response.digest, empty_digest, ASSET_DIGEST_SIZE) == 0;
        }
    }

    return success && asset_loopback_expect_finished(connection, stream, deadline);
}

static bool
asset_loopback_receive_face_batch(socket_t *connection, const uint16_t *faces, size_t count) {
    packet_struct *request = packet_new(0, SOCKET_FACE_BATCH_REQUEST_MAX_SIZE, 0);
    bool success = socket_face_batch_request_append(request, faces, count);
    uint64_t deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;
    socket_stream_t *stream = NULL;
    success = success && asset_loopback_send_request(connection,
                                                     SOCKET_STREAM_FACE_BATCH,
                                                     request->data,
                                                     request->len,
                                                     &stream,
                                                     deadline);
    packet_free(request);

    bool finished =
        success && asset_loopback_read_face_batch(connection, stream, faces, count, deadline);
    if (stream != NULL) {
        if (!finished) {
            socket_stream_reset(stream, SOCKET_STREAM_ERROR_CANCELLED);
        }
        socket_stream_destroy(stream);
    }
    return success && finished;
}

static bool asset_loopback_preempt_full_face_batch(socket_t *connection,
                                                   asset_loopback_server_t *server) {
    static const uint16_t speculative_faces[ASSET_FACE_BATCH_MAX] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const uint16_t replacement_faces[] = {1};
    socket_stream_t *speculative[ASSET_STREAM_ACTIVE_MAX] = {0};
    socket_stream_t *replacement = NULL;
    uint64_t deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;

    packet_struct *request = packet_new(0, SOCKET_FACE_BATCH_REQUEST_MAX_SIZE, 0);
    bool success =
        socket_face_batch_request_append(request, speculative_faces, arraysize(speculative_faces));
    atomic_store(&server->asset_service_enabled, false);
    for (size_t i = 0; success && i < arraysize(speculative); i++) {
        success = asset_loopback_send_request(connection,
                                              SOCKET_STREAM_FACE_BATCH,
                                              request->data,
                                              request->len,
                                              &speculative[i],
                                              deadline);
    }
    packet_free(request);

    /* Admit all three physical streams, then pause after an eight-byte write.
     * The server publishes the active count only after restoring the scheduler
     * timing globals, making this a deterministic full-capacity barrier. */
    atomic_store(&server->partial_service_paused, false);
    atomic_store(&server->partial_service_max_time_us, 1);
    atomic_store(&server->partial_service_requested, true);
    atomic_store(&server->asset_service_enabled, true);
    while (success && !atomic_load(&server->partial_service_paused) &&
           datetime_monotonic_ms() < deadline) {
        success = asset_loopback_progress(connection, false, deadline);
    }
    success = success && atomic_load(&server->partial_service_paused) &&
              atomic_load(&server->active_asset_streams) == ASSET_STREAM_ACTIVE_MAX;

    /* Deliver the isolated replacement while every speculative slot is still
     * live. The next synchronized scheduler cycle must leave that exact stream
     * pending rather than consume and reject it against the active limit. */
    request = packet_new(0, SOCKET_FACE_BATCH_REQUEST_MAX_SIZE, 0);
    success =
        success &&
        socket_face_batch_request_append(request, replacement_faces, arraysize(replacement_faces));
    unsigned int network_before = atomic_load(&server->network_service_calls);
    success = success && asset_loopback_send_request(connection,
                                                     SOCKET_STREAM_FACE_BATCH,
                                                     request->data,
                                                     request->len,
                                                     &replacement,
                                                     deadline);
    packet_free(request);

    unsigned int gameplay_before = atomic_load(&server->gameplay_bytes);
    uint8_t gameplay = 0xa5;
    size_t gameplay_pos = 0;
    while (success && gameplay_pos < sizeof(gameplay)) {
        size_t amount = 0;
        success = socket_write(connection,
                               &gameplay + gameplay_pos,
                               sizeof(gameplay) - gameplay_pos,
                               &amount);
        gameplay_pos += amount;
        if (success) {
            success = asset_loopback_progress(connection, true, deadline);
        }
    }
    while (success &&
           (atomic_load(&server->network_service_calls) - network_before < 2U ||
            atomic_load(&server->gameplay_bytes) == gameplay_before) &&
           datetime_monotonic_ms() < deadline) {
        success = asset_loopback_progress(connection, true, deadline);
    }
    success = success && atomic_load(&server->gameplay_bytes) > gameplay_before;
    success = success && asset_loopback_poll_pending(connection, server, 1, deadline);

    /* Sixteen microseconds at the production 8 MiB/s rate is enough to inspect
     * all three partially written headers, but too small to complete any full
     * eight-face batch. The published active count therefore proves a service
     * cycle ran while the victim remained live. */
    atomic_store(&server->partial_service_paused, false);
    atomic_store(&server->partial_service_max_time_us, 16);
    atomic_store(&server->partial_service_requested, true);
    atomic_store(&server->asset_service_enabled, true);
    while (success && !atomic_load(&server->partial_service_paused) &&
           datetime_monotonic_ms() < deadline) {
        success = asset_loopback_progress(connection, false, deadline);
    }
    success = success && atomic_load(&server->partial_service_paused) &&
              atomic_load(&server->active_asset_streams) == ASSET_STREAM_ACTIVE_MAX;
    success = success && asset_loopback_poll_pending(connection, server, 1, deadline);

    unsigned int reset_network_before = atomic_load(&server->network_service_calls);
    if (success) {
        socket_stream_reset(speculative[0], SOCKET_STREAM_ERROR_CANCELLED);
        socket_stream_destroy(speculative[0]);
        speculative[0] = NULL;
    }
    gameplay_before = atomic_load(&server->gameplay_bytes);
    gameplay = 0xb6;
    gameplay_pos = 0;
    while (success && gameplay_pos < sizeof(gameplay)) {
        size_t amount = 0;
        success = socket_write(connection,
                               &gameplay + gameplay_pos,
                               sizeof(gameplay) - gameplay_pos,
                               &amount);
        gameplay_pos += amount;
        if (success) {
            success = asset_loopback_progress(connection, true, deadline);
        }
    }
    while (success &&
           (atomic_load(&server->network_service_calls) - reset_network_before < 2U ||
            atomic_load(&server->gameplay_bytes) == gameplay_before) &&
           datetime_monotonic_ms() < deadline) {
        success = asset_loopback_progress(connection, true, deadline);
    }
    success = success && atomic_load(&server->gameplay_bytes) > gameplay_before;

    atomic_store(&server->partial_service_requested, false);
    atomic_store(&server->partial_service_paused, false);
    atomic_store(&server->partial_service_max_time_us, 1);
    atomic_store(&server->asset_service_enabled, true);
    if (replacement != NULL) {
        bool completed = success && asset_loopback_read_face_batch(connection,
                                                                   replacement,
                                                                   replacement_faces,
                                                                   arraysize(replacement_faces),
                                                                   deadline);
        if (!completed) {
            socket_stream_reset(replacement, SOCKET_STREAM_ERROR_CANCELLED);
        }
        socket_stream_destroy(replacement);
        replacement = NULL;
        success = success && completed;
    } else {
        success = false;
    }

    for (size_t i = 1; i < arraysize(speculative); i++) {
        if (speculative[i] == NULL) {
            success = false;
            continue;
        }
        bool completed = success && asset_loopback_read_face_batch(connection,
                                                                   speculative[i],
                                                                   speculative_faces,
                                                                   arraysize(speculative_faces),
                                                                   deadline);
        if (!completed) {
            socket_stream_reset(speculative[i], SOCKET_STREAM_ERROR_CANCELLED);
        }
        socket_stream_destroy(speculative[i]);
        speculative[i] = NULL;
        success = success && completed;
    }

    atomic_store(&server->asset_service_enabled, true);
    atomic_store(&server->partial_service_requested, false);
    atomic_store(&server->partial_service_paused, false);
    atomic_store(&server->partial_service_max_time_us, 1);
    for (size_t i = 0; i < arraysize(speculative); i++) {
        if (speculative[i] != NULL) {
            socket_stream_reset(speculative[i], SOCKET_STREAM_ERROR_CANCELLED);
            socket_stream_destroy(speculative[i]);
        }
    }
    if (replacement != NULL) {
        socket_stream_reset(replacement, SOCKET_STREAM_ERROR_CANCELLED);
        socket_stream_destroy(replacement);
    }
    return success;
}

static bool
asset_loopback_expect_rejected(socket_t *connection, const uint8_t *request, size_t request_size) {
    uint64_t deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;
    socket_stream_t *stream = NULL;
    bool success = asset_loopback_send_request(connection,
                                               SOCKET_STREAM_FACE_BATCH,
                                               request,
                                               request_size,
                                               &stream,
                                               deadline);
    bool rejected = false;
    while (success && datetime_monotonic_ms() < deadline) {
        uint8_t data[SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE];
        size_t amount = 0;
        socket_stream_result_t result = socket_stream_read(stream, VS(data), &amount);
        if (result == SOCKET_STREAM_RESULT_ERROR) {
            rejected = true;
            break;
        }
        if (result == SOCKET_STREAM_RESULT_FINISHED || amount != 0) {
            break;
        }
        success = asset_loopback_progress(connection, false, deadline);
    }
    if (stream != NULL) {
        if (!rejected) {
            socket_stream_reset(stream, SOCKET_STREAM_ERROR_CANCELLED);
        }
        socket_stream_destroy(stream);
    }
    return success && rejected;
}

static bool asset_loopback_cancel_face_batch_in_progress(socket_t *connection,
                                                         asset_loopback_server_t *server) {
    static const uint16_t faces[] = {1, 2};
    packet_struct *request = packet_new(0, SOCKET_FACE_BATCH_REQUEST_MAX_SIZE, 0);
    bool success = socket_face_batch_request_append(request, faces, arraysize(faces));
    uint64_t deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;
    socket_stream_t *stream = NULL;
    long expected_max_time = max_time;
    int expected_max_time_multiplier = max_time_multiplier;
    atomic_store(&server->partial_service_paused, false);
    atomic_store(&server->partial_service_max_time_us, 1);
    atomic_store(&server->partial_service_requested, true);
    success = success && asset_loopback_send_request(connection,
                                                     SOCKET_STREAM_FACE_BATCH,
                                                     request->data,
                                                     request->len,
                                                     &stream,
                                                     deadline);
    packet_free(request);

    while (success && !atomic_load(&server->partial_service_paused)) {
        success = asset_loopback_progress(connection, false, deadline);
    }
    success = success && atomic_load(&server->active_asset_streams) == 1 &&
              max_time == expected_max_time && max_time_multiplier == expected_max_time_multiplier;

    uint8_t partial_response = 0;
    size_t partial_size = 0;
    while (success && partial_size == 0 && datetime_monotonic_ms() < deadline) {
        socket_stream_result_t result =
            socket_stream_read(stream, &partial_response, sizeof(partial_response), &partial_size);
        success = result != SOCKET_STREAM_RESULT_ERROR && result != SOCKET_STREAM_RESULT_FINISHED;
        if (success && partial_size == 0) {
            success = asset_loopback_progress(connection, false, deadline);
        }
    }
    success = success && partial_size == sizeof(partial_response) &&
              atomic_load(&server->active_asset_streams) == 1;

    if (stream != NULL) {
        socket_stream_reset(stream, SOCKET_STREAM_ERROR_CANCELLED);
        socket_stream_destroy(stream);
    }

    unsigned int network_before = atomic_load(&server->network_service_calls);
    while (success && atomic_load(&server->network_service_calls) - network_before < 2U) {
        success = asset_loopback_progress(connection, true, deadline);
    }
    atomic_store(&server->partial_service_paused, false);
    atomic_store(&server->partial_service_max_time_us, 1);
    atomic_store(&server->partial_service_requested, true);
    atomic_store(&server->asset_service_enabled, true);
    while (success && atomic_load(&server->active_asset_streams) != 0 &&
           !atomic_load(&server->partial_service_paused)) {
        success = asset_loopback_progress(connection, false, deadline);
    }
    bool cleared = atomic_load(&server->active_asset_streams) == 0;

    atomic_store(&server->partial_service_requested, false);
    atomic_store(&server->asset_service_enabled, true);
    atomic_store(&server->partial_service_paused, false);
    return success && cleared;
}

static bool asset_loopback_shared_stream_limit(socket_t *connection,
                                               asset_loopback_server_t *server) {
    packet_struct *generic = packet_new(0, SOCKET_ASSET_REQUEST_MAX_SIZE, 0);
    static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
    socket_asset_request_append(generic, "faces/1.png", 0, empty_digest, 0);
    bool success = packet_writer_finish(generic);
    packet_struct *batch = packet_new(0, SOCKET_FACE_BATCH_REQUEST_MAX_SIZE, 0);
    const uint16_t face = 1;
    success = success && socket_face_batch_request_append(batch, &face, 1);

    socket_stream_t *streams[ASSET_STREAM_ACTIVE_MAX + 1U] = {0};
    uint64_t deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;
    atomic_store(&server->asset_service_enabled, false);
    for (size_t i = 0; success && i < arraysize(streams); i++) {
        packet_struct *request = i % 2U == 0 ? generic : batch;
        socket_stream_kind_t kind = i % 2U == 0 ? SOCKET_STREAM_ASSET : SOCKET_STREAM_FACE_BATCH;
        success = asset_loopback_send_request(connection,
                                              kind,
                                              request->data,
                                              request->len,
                                              &streams[i],
                                              deadline);
    }

    unsigned int gameplay_before = atomic_load(&server->gameplay_bytes);
    uint8_t gameplay = 0x5a;
    size_t gameplay_pos = 0;
    while (success && gameplay_pos < sizeof(gameplay)) {
        size_t amount = 0;
        success = socket_write(connection,
                               &gameplay + gameplay_pos,
                               sizeof(gameplay) - gameplay_pos,
                               &amount);
        gameplay_pos += amount;
        if (success && amount == 0) {
            success = asset_loopback_progress(connection, true, deadline);
        }
    }
    while (success && atomic_load(&server->gameplay_bytes) == gameplay_before) {
        success = asset_loopback_progress(connection, false, deadline);
    }
    atomic_store(&server->partial_service_paused, false);
    atomic_store(&server->partial_service_max_time_us, 1);
    atomic_store(&server->partial_service_requested, true);
    atomic_store(&server->asset_service_enabled, true);
    while (success && !atomic_load(&server->partial_service_paused) &&
           datetime_monotonic_ms() < deadline) {
        success = asset_loopback_progress(connection, false, deadline);
    }
    success = success && atomic_load(&server->partial_service_paused) &&
              atomic_load(&server->active_asset_streams) == ASSET_STREAM_ACTIVE_MAX;

    atomic_store(&server->partial_service_requested, false);
    atomic_store(&server->partial_service_paused, false);
    atomic_store(&server->asset_service_enabled, true);

    size_t completed = 0;
    size_t rejected = 0;
    for (size_t i = 0; success && i < arraysize(streams); i++) {
        bool done = false;
        while (!done && datetime_monotonic_ms() < deadline) {
            uint8_t data[4096];
            size_t amount = 0;
            socket_stream_result_t result = socket_stream_read(streams[i], VS(data), &amount);
            if (result == SOCKET_STREAM_RESULT_ERROR) {
                rejected++;
                done = true;
            } else if (result == SOCKET_STREAM_RESULT_FINISHED) {
                completed++;
                done = true;
            } else if (amount == 0) {
                success = asset_loopback_progress(connection, false, deadline);
            }
        }
        success = success && done;
    }
    success = success && completed == arraysize(streams) && rejected == 0 &&
              atomic_load(&server->gameplay_bytes) > gameplay_before;

    for (size_t i = 0; i < arraysize(streams); i++) {
        if (streams[i] != NULL) {
            socket_stream_destroy(streams[i]);
        }
    }
    packet_free(batch);
    packet_free(generic);
    return success;
}

static bool asset_loopback_generic_wave_reuses_connection_pass(socket_t *connection,
                                                               asset_loopback_server_t *server) {
    static const char *const paths[] = {
        "resources/paintings/hill.jpg",
        "resources/paintings/ruins.jpg",
        "resources/paintings/wolf.jpg",
    };
    typedef struct asset_wave_receive {
        uint8_t header[SOCKET_ASSET_RESPONSE_HEADER_SIZE];
        socket_asset_response_t response;
        size_t header_pos;
        uint32_t body_pos;
        bool finished;
    } asset_wave_receive_t;
    socket_stream_t *streams[arraysize(paths)] = {0};
    asset_wave_receive_t receives[arraysize(paths)] = {0};
    uint64_t deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;
    bool success = true;

    atomic_store(&server->asset_service_enabled, false);
    atomic_store(&server->max_productive_passes, 0);
    for (size_t i = 0; success && i < arraysize(paths); i++) {
        packet_struct *request = packet_new(0, SOCKET_ASSET_REQUEST_MAX_SIZE, 0);
        static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
        socket_asset_request_append(request, paths[i], 0, empty_digest, 0);
        success = packet_writer_finish(request) && asset_loopback_send_request(connection,
                                                                               SOCKET_STREAM_ASSET,
                                                                               request->data,
                                                                               request->len,
                                                                               &streams[i],
                                                                               deadline);
        packet_free(request);
    }
    atomic_store(&server->asset_service_enabled, true);

    uint64_t aggregate = 0;
    size_t finished = 0;
    while (success && finished < arraysize(streams) && datetime_monotonic_ms() < deadline) {
        for (size_t i = 0; success && i < arraysize(streams); i++) {
            asset_wave_receive_t *receive = &receives[i];
            if (receive->finished) {
                continue;
            }

            size_t amount = 0;
            socket_stream_result_t result;
            if (receive->header_pos < sizeof(receive->header)) {
                result = socket_stream_read(streams[i],
                                            receive->header + receive->header_pos,
                                            sizeof(receive->header) - receive->header_pos,
                                            &amount);
                success =
                    result != SOCKET_STREAM_RESULT_ERROR && result != SOCKET_STREAM_RESULT_FINISHED;
                receive->header_pos += amount;
                if (success && receive->header_pos == sizeof(receive->header)) {
                    success = socket_asset_response_parse(receive->header,
                                                          sizeof(receive->header),
                                                          0,
                                                          &receive->response) &&
                              receive->response.status == ASSET_STATUS_OK &&
                              receive->response.total_size != 0;
                    if (success) {
                        aggregate += receive->response.total_size;
                    }
                }
            } else if (receive->body_pos < receive->response.total_size) {
                uint8_t body[4096];
                size_t remaining = receive->response.total_size - receive->body_pos;
                result =
                    socket_stream_read(streams[i], body, MIN(sizeof(body), remaining), &amount);
                success =
                    result != SOCKET_STREAM_RESULT_ERROR && result != SOCKET_STREAM_RESULT_FINISHED;
                receive->body_pos += (uint32_t)amount;
            } else {
                uint8_t surplus;
                result = socket_stream_read(streams[i], &surplus, sizeof(surplus), &amount);
                success = result != SOCKET_STREAM_RESULT_ERROR && amount == 0;
                if (success && result == SOCKET_STREAM_RESULT_FINISHED) {
                    receive->finished = true;
                    finished++;
                }
            }
        }
        if (success && finished < arraysize(streams)) {
            success = asset_loopback_progress(connection, false, deadline);
        }
    }
    success = success && finished == arraysize(streams) &&
              aggregate > ASSET_FACE_BATCH_MAX_SIZE +
                              ASSET_FACE_BATCH_MAX * SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE &&
              atomic_load(&server->max_productive_passes) >= 2;

    for (size_t i = 0; i < arraysize(streams); i++) {
        if (streams[i] != NULL) {
            socket_stream_destroy(streams[i]);
        }
    }
    return success;
}
#endif

START_TEST(test_socket_asset_request_round_trip) {
    uint8_t digest[ASSET_DIGEST_SIZE];
    memset(digest, 0x89, sizeof(digest));
    packet_struct *packet = packet_new(0, 0, 0);
    socket_asset_request_append(packet, "client-maps/test.png", 123456, digest, 0);

    socket_asset_request_t request;
    ck_assert(socket_asset_request_parse(packet->data, packet->len, 0, &request));
    ck_assert_str_eq(request.path, "client-maps/test.png");
    ck_assert_uint_eq(request.cached_size, 123456);
    ck_assert_uint_eq(request.flags, 0);
    ck_assert_mem_eq(request.cached_digest, digest, sizeof(digest));
    packet_free(packet);
}
END_TEST

START_TEST(test_socket_asset_face_path_round_trip_and_malformed) {
    char path[32];
    ck_assert(socket_asset_face_path_format(VS(path), 1));
    ck_assert_str_eq(path, "faces/1.png");

    uint16_t face = 99;
    ck_assert(socket_asset_face_path_parse(path, &face));
    ck_assert_uint_eq(face, 1);
    ck_assert(socket_asset_face_path_format(VS(path), UINT16_MAX));
    ck_assert(socket_asset_face_path_parse(path, &face));
    ck_assert_uint_eq(face, UINT16_MAX);

    static const char *invalid[] = {
        "",
        "faces/",
        "faces/0.png",
        "faces/01.png",
        "faces/01.png.extra",
        "faces/-1.png",
        "faces/65536.png",
        "faces/1",
        "faces/1.PNG",
        "other/1.png",
    };
    for (size_t i = 0; i < arraysize(invalid); i++) {
        face = 99;
        ck_assert(!socket_asset_face_path_parse(invalid[i], &face));
        ck_assert_uint_eq(face, 99);
    }
    ck_assert(!socket_asset_face_path_format(NULL, sizeof(path), 1));
    ck_assert(!socket_asset_face_path_format(VS(path), 0));
    ck_assert(!socket_asset_face_path_format(path, 4, 1));
}
END_TEST

START_TEST(test_socket_face_asset_snapshot_is_bounded_and_authenticated) {
    const uint8_t *data = NULL;
    const uint8_t *digest = NULL;
    uint32_t size = 0;
    ck_assert(face_get_asset(1, &data, &size, &digest));
    ck_assert_ptr_nonnull(data);
    ck_assert_ptr_nonnull(digest);
    ck_assert_uint_gt(size, 0);
    ck_assert_uint_le(size, ASSET_FACE_MAX_SIZE);

    uint8_t calculated[ASSET_DIGEST_SIZE];
    unsigned int calculated_size = 0;
    ck_assert_int_eq(EVP_Digest(data, size, calculated, &calculated_size, EVP_sha256(), NULL), 1);
    ck_assert_uint_eq(calculated_size, ASSET_DIGEST_SIZE);
    ck_assert_mem_eq(calculated, digest, sizeof(calculated));
    ck_assert(!face_get_asset(0, NULL, NULL, NULL));
    ck_assert(!face_get_asset(UINT16_MAX, NULL, NULL, NULL));
}
END_TEST

START_TEST(test_socket_asset_global_work_budget_rotates) {
    enum {
        CONNECTION_COUNT = 600
    };
    socket_struct *connections = xcalloc(CONNECTION_COUNT, sizeof(*connections));
    for (size_t i = 0; i < CONNECTION_COUNT; i++) {
        connections[i].state = ST_DEAD;
        socket_assets_connection_register(&connections[i]);
    }

    socket_assets_service();
    size_t first_tick_serviced = 0;
    for (size_t i = 0; i < CONNECTION_COUNT; i++) {
        first_tick_serviced += connections[i].asset_service_generation != 0;
    }
    ck_assert_uint_eq(first_tick_serviced, 512U);

    socket_assets_service();
    for (size_t i = 0; i < CONNECTION_COUNT; i++) {
        ck_assert_uint_ne(connections[i].asset_service_generation, 0);
    }

    for (size_t i = 0; i < CONNECTION_COUNT; i++) {
        socket_assets_connection_clear(&connections[i]);
    }
    free(connections);
}
END_TEST

START_TEST(test_socket_asset_byte_budget_tracks_processing_rate) {
    long saved_max_time = max_time;
    int saved_multiplier = max_time_multiplier;

    max_time = 125000;
    max_time_multiplier = 1;
    ck_assert_uint_eq(socket_assets_tick_byte_budget(), 1024U * 1024U);
    max_time_multiplier = 2;
    ck_assert_uint_eq(socket_assets_tick_byte_budget(), 512U * 1024U);
    max_time = 250000;
    ck_assert_uint_eq(socket_assets_tick_byte_budget(), 1024U * 1024U);
    max_time = 2000000;
    max_time_multiplier = 1;
    ck_assert_uint_eq(socket_assets_tick_byte_budget(), 8U * 1024U * 1024U);
    max_time = LONG_MAX;
    ck_assert_uint_eq(socket_assets_tick_byte_budget(), 8U * 1024U * 1024U);

    max_time = saved_max_time;
    max_time_multiplier = saved_multiplier;
}
END_TEST

START_TEST(test_socket_asset_connection_budget_is_fair_and_work_conserving) {
    long saved_max_time = max_time;
    int saved_multiplier = max_time_multiplier;
    max_time = 125000;
    max_time_multiplier = 1;

    size_t remaining = socket_assets_tick_byte_budget();
    size_t first = socket_assets_connection_pass_byte_budget(remaining);
    remaining -= first;
    size_t second = socket_assets_connection_pass_byte_budget(remaining);
    remaining -= second;
    size_t third = socket_assets_connection_pass_byte_budget(remaining);

    ck_assert_uint_ge(first,
                      ASSET_FACE_BATCH_MAX_SIZE +
                          ASSET_FACE_BATCH_MAX * SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE);
    ck_assert_uint_eq(second, first);
    ck_assert_uint_gt(third, 0);

    size_t aggregate = socket_assets_tick_byte_budget();
    remaining = aggregate;
    size_t redistributed = 0;
    size_t quanta = 0;
    while (remaining != 0) {
        size_t allowance = socket_assets_connection_pass_byte_budget(remaining);
        ck_assert_uint_gt(allowance, 0);
        remaining -= allowance;
        redistributed += allowance;
        quanta++;
    }
    ck_assert_uint_eq(redistributed, aggregate);
    ck_assert_uint_gt(quanta, 1);

    max_time = saved_max_time;
    max_time_multiplier = saved_multiplier;
}
END_TEST

START_TEST(test_socket_face_batch_worst_case_service_bound) {
    uint32_t sizes[ASSET_FACE_BATCH_MAX];
    for (size_t i = 0; i < arraysize(sizes); i++) {
        sizes[i] = ASSET_FACE_MAX_SIZE;
    }
    size_t expected =
        2U + ASSET_FACE_BATCH_MAX *
                 (1U + (ASSET_FACE_MAX_SIZE + ASSET_STREAM_QUANTUM - 1U) / ASSET_STREAM_QUANTUM);
    ck_assert_uint_eq(socket_assets_face_batch_service_rounds(sizes, arraysize(sizes)), expected);

    sizes[0] = 0;
    ck_assert_uint_eq(socket_assets_face_batch_service_rounds(sizes, arraysize(sizes)),
                      expected -
                          (ASSET_FACE_MAX_SIZE + ASSET_STREAM_QUANTUM - 1U) / ASSET_STREAM_QUANTUM);
    sizes[0] = ASSET_FACE_MAX_SIZE + 1U;
    ck_assert_uint_eq(socket_assets_face_batch_service_rounds(sizes, arraysize(sizes)), 0);
    ck_assert_uint_eq(socket_assets_face_batch_service_rounds(sizes, 0), 0);
}
END_TEST

START_TEST(test_socket_asset_logical_rate_accounting_is_atomic) {
    socket_struct ns = {.state = ST_LOGIN};

    ck_assert(socket_assets_request_rate_allow(&ns, SOCKET_ASSET_REQUEST_RATE_MAX - 8U));
    ck_assert_uint_eq(ns.asset_window_requests, SOCKET_ASSET_REQUEST_RATE_MAX - 8U);
    ck_assert(socket_assets_request_rate_allow(&ns, 8));
    ck_assert_uint_eq(ns.asset_window_requests, SOCKET_ASSET_REQUEST_RATE_MAX);
    ck_assert(!socket_assets_request_rate_allow(&ns, 1));
    ck_assert_uint_eq(ns.asset_window_requests, SOCKET_ASSET_REQUEST_RATE_MAX);
    ck_assert_int_eq(ns.state, ST_ZOMBIE);
}
END_TEST

#if OPENSSL_VERSION_NUMBER >= 0x30500000L
START_TEST(test_socket_face_asset_borrowed_body_loopback) {
    const uint8_t *expected = NULL;
    const uint8_t *expected_digest = NULL;
    uint32_t expected_size = 0;
    ck_assert(face_get_asset(1, &expected, &expected_size, &expected_digest));

    char directory[] = "/tmp/atrinik-server-face-stream-XXXXXX";
    char identity[HUGE_BUF];

    asset_loopback_server_t server = {0};
    atomic_init(&server.stop, false);
    atomic_init(&server.accepted, false);
    atomic_init(&server.asset_service_enabled, true);
    atomic_init(&server.partial_service_requested, false);
    atomic_init(&server.partial_service_paused, false);
    atomic_init(&server.partial_service_max_time_us, 1);
    atomic_init(&server.pending_poll_requested, false);
    atomic_init(&server.pending_poll_completed, false);
    atomic_init(&server.pending_classified_streams, 0);
    atomic_init(&server.gameplay_bytes, 0);
    atomic_init(&server.active_asset_streams, 0);
    atomic_init(&server.network_service_calls, 0);
    atomic_init(&server.max_productive_passes, 0);
    uint16_t port = 0;
    char fingerprint[65];
    server.listener = asset_loopback_listener_create(directory, VS(identity), &port, fingerprint);
    ck_assert_ptr_nonnull(server.listener);

    pthread_t thread;
    ck_assert_int_eq(pthread_create(&thread, NULL, asset_loopback_server_main, &server), 0);

    socket_connect_failure_t failure = {0};
    socket_t *client = socket_quic_client_create("127.0.0.1",
                                                 port,
                                                 fingerprint,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 SOCKET_CONNECTION_PREFERENCE_DIRECTORY,
                                                 &failure);
    uint64_t accept_deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;
    uint8_t gameplay = 0;
    size_t gameplay_written = 0;
    while (client != NULL && gameplay_written == 0 && datetime_monotonic_ms() < accept_deadline) {
        if (!socket_write(client, &gameplay, sizeof(gameplay), &gameplay_written)) {
            break;
        }
        asset_loopback_progress(client, gameplay_written == 0, accept_deadline);
    }
    while (client != NULL && !atomic_load(&server.accepted) &&
           datetime_monotonic_ms() < accept_deadline) {
        asset_loopback_progress(client, false, accept_deadline);
    }
    bool success = client != NULL && gameplay_written == sizeof(gameplay) &&
                   asset_loopback_receive_face(client, 1, expected, expected_size, expected_digest);
    atomic_store(&server.stop, true);
    ck_assert_int_eq(pthread_join(thread, NULL), 0);

    if (client != NULL) {
        socket_destroy(client);
    }
    socket_destroy(server.listener);
    ck_assert_int_eq(unlink(identity), 0);
    ck_assert_int_eq(rmdir(directory), 0);
    ck_assert(!server.failed);
    ck_assert(success);
    ck_assert(server.served);
    ck_assert_uint_eq(server.transport_capabilities, ASSET_TRANSPORT_CAP_ALL);
    ck_assert_uint_gt(server.completion_service_calls, 0);
    ck_assert_uint_le(server.completion_service_calls, ASSET_LOOPBACK_COMPLETION_CALL_MAX);
}
END_TEST

START_TEST(test_socket_face_batch_production_loopback) {
    /* Unit-test startup intentionally skips the production asset cache. Load
     * it here so the scheduler regression streams real, borrowed catalog
     * bodies instead of synthetic buffers. */
    socket_assets_init();

    char directory[] = "/tmp/atrinik-server-face-batch-XXXXXX";
    char identity[HUGE_BUF];

    asset_loopback_server_t server = {0};
    atomic_init(&server.stop, false);
    atomic_init(&server.accepted, false);
    atomic_init(&server.asset_service_enabled, true);
    atomic_init(&server.partial_service_requested, false);
    atomic_init(&server.partial_service_paused, false);
    atomic_init(&server.partial_service_max_time_us, 1);
    atomic_init(&server.pending_poll_requested, false);
    atomic_init(&server.pending_poll_completed, false);
    atomic_init(&server.pending_classified_streams, 0);
    atomic_init(&server.gameplay_bytes, 0);
    atomic_init(&server.active_asset_streams, 0);
    atomic_init(&server.network_service_calls, 0);
    atomic_init(&server.max_productive_passes, 0);
    uint16_t port = 0;
    char fingerprint[65];
    server.listener = asset_loopback_listener_create(directory, VS(identity), &port, fingerprint);
    ck_assert_ptr_nonnull(server.listener);

    pthread_t thread;
    ck_assert_int_eq(pthread_create(&thread, NULL, asset_loopback_server_main, &server), 0);

    socket_connect_failure_t failure = {0};
    socket_t *client = socket_quic_client_create("127.0.0.1",
                                                 port,
                                                 fingerprint,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 SOCKET_CONNECTION_PREFERENCE_DIRECTORY,
                                                 &failure);
    uint64_t accept_deadline = datetime_monotonic_ms() + ASSET_LOOPBACK_TIMEOUT_MS;
    uint8_t gameplay = 0;
    size_t gameplay_written = 0;
    while (client != NULL && gameplay_written == 0 && datetime_monotonic_ms() < accept_deadline) {
        if (!socket_write(client, &gameplay, sizeof(gameplay), &gameplay_written)) {
            break;
        }
        asset_loopback_progress(client, gameplay_written == 0, accept_deadline);
    }
    while (client != NULL && !atomic_load(&server.accepted) &&
           datetime_monotonic_ms() < accept_deadline) {
        asset_loopback_progress(client, false, accept_deadline);
    }

    static const uint16_t full_batch[ASSET_FACE_BATCH_MAX] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const uint16_t not_found_batch[] = {1, UINT16_MAX, 2};
    bool success =
        client != NULL && gameplay_written == sizeof(gameplay) &&
        asset_loopback_receive_face_batch(client, full_batch, arraysize(full_batch)) &&
        asset_loopback_receive_face_batch(client, not_found_batch, arraysize(not_found_batch));
    success =
        success && asset_loopback_cancel_face_batch_in_progress(client, &server) &&
        asset_loopback_receive_face_batch(client, not_found_batch, arraysize(not_found_batch));
    success = success && asset_loopback_preempt_full_face_batch(client, &server);

    static const uint8_t malformed[][SOCKET_FACE_BATCH_REQUEST_MAX_SIZE] = {
        {0},
        {9},
        {2, 0, 1},
        {1, 0, 0},
        {2, 0, 1, 0, 1},
        {1, 0, 1, 0},
    };
    static const size_t malformed_sizes[] = {1, 1, 3, 3, 5, 4};
    CASSERT_ARRAY(malformed, arraysize(malformed_sizes));
    for (size_t i = 0; success && i < arraysize(malformed); i++) {
        success = asset_loopback_expect_rejected(client, malformed[i], malformed_sizes[i]);
    }
    success = success && asset_loopback_shared_stream_limit(client, &server);
    success = success && asset_loopback_generic_wave_reuses_connection_pass(client, &server);

    atomic_store(&server.stop, true);
    ck_assert_int_eq(pthread_join(thread, NULL), 0);
    if (client != NULL) {
        socket_destroy(client);
    }
    socket_destroy(server.listener);
    ck_assert_int_eq(unlink(identity), 0);
    ck_assert_int_eq(rmdir(directory), 0);
    ck_assert(!server.failed);
    ck_assert(success);
    ck_assert(server.served);
    ck_assert_uint_eq(server.transport_capabilities, ASSET_TRANSPORT_CAP_ALL);
    ck_assert_uint_gt(server.completion_service_calls, 0);
    ck_assert_uint_le(server.completion_service_calls, ASSET_LOOPBACK_COMPLETION_CALL_MAX);
}
END_TEST
#endif

START_TEST(test_socket_stream_preface_round_trip_and_malformed) {
    uint8_t preface[SOCKET_STREAM_PREFACE_SIZE];
    socket_stream_kind_t kind = SOCKET_STREAM_ASSET;
    socket_stream_preface_encode(preface, SOCKET_STREAM_GAME);
    ck_assert(socket_stream_preface_decode(preface, sizeof(preface), &kind));
    ck_assert_int_eq(kind, SOCKET_STREAM_GAME);

    for (size_t truncated = 0; truncated < sizeof(preface); truncated++) {
        kind = SOCKET_STREAM_ASSET;
        ck_assert(!socket_stream_preface_decode(preface, truncated, &kind));
        ck_assert_int_eq(kind, SOCKET_STREAM_ASSET);
    }

    static const size_t malformed_offsets[] = {0, 4, 5, 6, 7};
    for (size_t i = 0; i < arraysize(malformed_offsets); i++) {
        socket_stream_preface_encode(preface, SOCKET_STREAM_GAME);
        preface[malformed_offsets[i]] ^= 0xff;
        kind = SOCKET_STREAM_ASSET;
        ck_assert(!socket_stream_preface_decode(preface, sizeof(preface), &kind));
        ck_assert_int_eq(kind, SOCKET_STREAM_ASSET);
    }

    socket_stream_preface_encode(preface, SOCKET_STREAM_ASSET);
    ck_assert(socket_stream_preface_decode(preface, sizeof(preface), &kind));
    ck_assert_int_eq(kind, SOCKET_STREAM_ASSET);
    socket_stream_preface_encode(preface, SOCKET_STREAM_FACE_BATCH);
    ck_assert(socket_stream_preface_decode(preface, sizeof(preface), &kind));
    ck_assert_int_eq(kind, SOCKET_STREAM_FACE_BATCH);
}
END_TEST

START_TEST(test_metaserver_rendezvous_token_bounds) {
    static const char token[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char response[128];
    int length = snprintf(VS(response), "{\"status\":\"ok\",\"rendezvousToken\":\"%s\"}", token);
    ck_assert_int_gt(length, 0);
    ck_assert_int_lt(length, (int)sizeof(response));

    char parsed[65];
    ck_assert(metaserver_rendezvous_token_parse(response, (size_t)length, parsed));
    ck_assert_str_eq(parsed, token);

    for (size_t truncated = 0; truncated < (size_t)length - 1; truncated++) {
        ck_assert(!metaserver_rendezvous_token_parse(response, truncated, parsed));
    }

    response[sizeof("{\"status\":\"ok\",\"rendezvousToken\":\"") - 1] = 'A';
    ck_assert(!metaserver_rendezvous_token_parse(response, (size_t)length, parsed));
    static const char cleared[sizeof(parsed)];
    ck_assert_mem_eq(parsed, cleared, sizeof(parsed));
}
END_TEST

START_TEST(test_metaserver_rendezvous_retry_policy) {
    ck_assert_int_gt(METASERVER_RENDEZVOUS_UPGRADE_TIMEOUT_MS,
                     METASERVER_RENDEZVOUS_CONNECT_TIMEOUT_MS);
    ck_assert_int_le(METASERVER_RENDEZVOUS_UPGRADE_TIMEOUT_MS, 30000L);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(0, 0, 0), 3750);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(0, 0, 2500), 6250);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(1, 0, 0), 7500);
    uint32_t capped_rendezvous = metaserver_rendezvous_retry_delay_ms(UINT32_MAX, 0, UINT32_MAX);
    ck_assert_uint_ge(capped_rendezvous, METASERVER_RENDEZVOUS_RETRY_MAX_MS * 3U / 4U);
    ck_assert_uint_le(capped_rendezvous, METASERVER_RENDEZVOUS_RETRY_MAX_MS);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(0, 60, 0), 60000);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(0, UINT32_MAX, 0),
                      METASERVER_RENDEZVOUS_RETRY_AFTER_MAX_SECONDS * 1000U);
    ck_assert_uint_eq(metaserver_rendezvous_retry_failures(4, METASERVER_RENDEZVOUS_STABLE_MS - 1U),
                      4);
    ck_assert_uint_eq(metaserver_rendezvous_retry_failures(4, METASERVER_RENDEZVOUS_STABLE_MS), 0);

    metaserver_rendezvous_headers_t headers = {0};
    char status[] = "HTTP/1.1 429 Too Many Requests\r\n";
    char retry[] = "Retry-After: 120\r\n";
    char protocol[] = "Sec-WebSocket-Protocol: " RENDEZVOUS_INVITE_SUBPROTOCOL "\r\n";
    ck_assert(metaserver_rendezvous_protocol_allows(&headers, false));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, true));
    ck_assert_uint_eq(metaserver_rendezvous_header(status, 1, strlen(status), &headers),
                      strlen(status));
    ck_assert_uint_eq(metaserver_rendezvous_header(retry, 1, strlen(retry), &headers),
                      strlen(retry));
    ck_assert_uint_eq(metaserver_rendezvous_header(protocol, 1, strlen(protocol), &headers),
                      strlen(protocol));
    ck_assert(headers.has_retry_after);
    ck_assert_uint_eq(headers.retry_after_seconds, 120);
    ck_assert(rendezvous_websocket_protocol_valid(&headers.protocol));
    ck_assert(metaserver_rendezvous_protocol_allows(&headers, true));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, false));

    ck_assert_uint_eq(metaserver_rendezvous_header(protocol, 1, strlen(protocol), &headers),
                      strlen(protocol));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, true));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, false));

    char wrong_protocol[] = "Sec-WebSocket-Protocol: atrinik-rendezvous-v0\r\n";
    metaserver_rendezvous_header(status, 1, strlen(status), &headers);
    metaserver_rendezvous_header(wrong_protocol, 1, strlen(wrong_protocol), &headers);
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, true));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, false));

    char second_status[] = "HTTP/1.1 503 Service Unavailable\r\n";
    char invalid_retry[] = "Retry-After: tomorrow\r\n";
    char bounded_retry[] = "Retry-After: 999999999999999999999\r\n";
    metaserver_rendezvous_header(second_status, 1, strlen(second_status), &headers);
    ck_assert(!headers.has_retry_after);
    ck_assert(metaserver_rendezvous_protocol_allows(&headers, false));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, true));
    metaserver_rendezvous_header(invalid_retry, 1, strlen(invalid_retry), &headers);
    ck_assert(!headers.has_retry_after);
    metaserver_rendezvous_header(bounded_retry, 1, strlen(bounded_retry), &headers);
    ck_assert(headers.has_retry_after);
    ck_assert_uint_eq(headers.retry_after_seconds, METASERVER_RENDEZVOUS_RETRY_AFTER_MAX_SECONDS);
}
END_TEST

START_TEST(test_metaserver_publish_cadence) {
    server_monotonic_t now = {UINT64_C(1000000)};
    metaserver_publish_cadence_t cadence;
    metaserver_publish_cadence_init(&cadence, now);
    ck_assert(metaserver_publish_cadence_needs_snapshot(&cadence, now));
    ck_assert(metaserver_publish_cadence_due(&cadence, now, true));
    ck_assert_uint_eq(cadence.rate_budget.tokens, 2);

    ck_assert(metaserver_publish_cadence_attempted(&cadence, now));
    ck_assert_uint_eq(cadence.rate_budget.tokens, 1);
    metaserver_publish_cadence_succeeded(&cadence,
                                         now,
                                         true,
                                         METASERVER_PUBLISH_HEARTBEAT_DEFAULT_SECONDS,
                                         0);
    ck_assert_uint_eq(cadence.heartbeat_deadline.microseconds,
                      now.microseconds + UINT64_C(8100) * UINT64_C(1000000));
    ck_assert(!metaserver_publish_cadence_due(&cadence, now, false));

    server_monotonic_t changed = {now.microseconds + UINT64_C(1000000)};
    metaserver_publish_cadence_changed(&cadence, changed, true);
    ck_assert(!metaserver_publish_cadence_needs_snapshot(&cadence, changed));
    ck_assert(!metaserver_publish_cadence_due(&cadence, changed, true));
    server_monotonic_t debounce = {changed.microseconds +
                                   METASERVER_PUBLISH_DEBOUNCE_SECONDS * UINT64_C(1000000)};
    ck_assert(metaserver_publish_cadence_due(&cadence, debounce, true));
    ck_assert(metaserver_publish_cadence_attempted(&cadence, debounce));
    ck_assert_uint_eq(cadence.rate_budget.tokens, 0);

    metaserver_publish_cadence_changed(&cadence, debounce, true);
    server_monotonic_t refill = {now.microseconds +
                                 METASERVER_ATTEMPT_RATE_REFILL_SECONDS * UINT64_C(1000000)};
    ck_assert(!metaserver_publish_cadence_due(&cadence,
                                              (server_monotonic_t){refill.microseconds - 1U},
                                              true));
    ck_assert(metaserver_publish_cadence_due(&cadence, refill, true));
    ck_assert_uint_eq(cadence.rate_budget.tokens, 1);

    metaserver_publish_cadence_suspend(&cadence);
    cadence.dirty = false;
    ck_assert(!metaserver_publish_cadence_needs_snapshot(&cadence, refill));
    ck_assert(!metaserver_publish_cadence_due(&cadence, refill, true));
    metaserver_publish_cadence_changed(&cadence, refill, false);
    ck_assert(metaserver_publish_cadence_needs_snapshot(&cadence, refill));
    ck_assert(cadence.suspended);
    metaserver_publish_cadence_changed(&cadence, refill, true);
    ck_assert(!cadence.suspended);
    ck_assert(!metaserver_publish_cadence_due(&cadence, refill, true));
    ck_assert(metaserver_publish_cadence_due(
        &cadence,
        (server_monotonic_t){refill.microseconds +
                             METASERVER_PUBLISH_DEBOUNCE_SECONDS * UINT64_C(1000000)},
        true));

    ck_assert(metaserver_publish_cadence_recover_replay(&cadence));
    ck_assert(!metaserver_publish_cadence_recover_replay(&cadence));

    metaserver_publish_cadence_init(&cadence, now);
    ck_assert(metaserver_publish_cadence_attempted(&cadence, now));
    metaserver_publish_cadence_changed(&cadence, changed, false);
    metaserver_publish_cadence_suspend(&cadence);
    ck_assert(cadence.suspended);
    ck_assert(cadence.dirty);
    ck_assert(metaserver_publish_cadence_needs_snapshot(&cadence, changed));

    metaserver_publish_cadence_init(&cadence, changed);
    ck_assert(!cadence.suspended);
    ck_assert_uint_eq(cadence.rate_budget.tokens, METASERVER_ATTEMPT_RATE_CAPACITY);
    ck_assert(metaserver_publish_cadence_due(&cadence, changed, true));
}
END_TEST

START_TEST(test_metaserver_publish_cadence_attempt_is_fail_closed) {
    server_monotonic_t now = {UINT64_C(1000000)};
    metaserver_publish_cadence_t cadence;
    metaserver_publish_cadence_init(&cadence, now);

    ck_assert(metaserver_publish_cadence_attempted(&cadence, now));
    ck_assert_uint_eq(cadence.rate_budget.tokens, 1);

#ifdef NDEBUG
    cadence.dirty = true;
    cadence.retry_deadline = now;
    cadence.rate_budget.tokens = 0;
    ck_assert(!metaserver_publish_cadence_attempted(&cadence, now));
    ck_assert_uint_eq(cadence.rate_budget.tokens, 0);
    ck_assert(cadence.dirty);
    ck_assert_uint_eq(cadence.retry_deadline.microseconds, now.microseconds);
#endif
}
END_TEST

START_TEST(test_metaserver_publish_retry_and_daily_budget) {
    ck_assert_uint_eq(metaserver_publish_retry_delay_ms(0, 0, 0), 45000);
    ck_assert_uint_eq(metaserver_publish_retry_delay_ms(1, 0, 0), 90000);
    uint32_t capped_publish = metaserver_publish_retry_delay_ms(UINT32_MAX, 0, UINT32_MAX);
    ck_assert_uint_ge(capped_publish, METASERVER_PUBLISH_RETRY_MAX_MS * 3U / 4U);
    ck_assert_uint_le(capped_publish, METASERVER_PUBLISH_RETRY_MAX_MS);
    ck_assert_uint_eq(metaserver_publish_retry_delay_ms(0, 120, 0), 120000);
    ck_assert_uint_eq(metaserver_publish_retry_delay_ms(0, UINT32_MAX, 0),
                      METASERVER_PUBLISH_RETRY_AFTER_MAX_SECONDS * 1000U);
    ck_assert_uint_eq(metaserver_publish_heartbeat_delay_seconds(9000, 0), 8100);
    ck_assert_uint_eq(metaserver_publish_heartbeat_delay_seconds(9000, 1800), 9900);

    char headers[] = "HTTP/1.1 100 Continue\r\nRetry-After: 1\r\n\r\n"
                     "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 120\r\n\r\n";
    uint32_t retry_after = 0;
    ck_assert(metaserver_publish_retry_after(headers, strlen(headers), &retry_after));
    ck_assert_uint_eq(retry_after, 120);
    char invalid[] = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: tomorrow\r\n\r\n";
    ck_assert(!metaserver_publish_retry_after(invalid, strlen(invalid), &retry_after));
    ck_assert_uint_eq(retry_after, 0);
    ck_assert(metaserver_publish_response_retryable(CURL_STATE_ERROR, -1));
    ck_assert(metaserver_publish_response_retryable(CURL_STATE_OK, 408));
    ck_assert(metaserver_publish_response_retryable(CURL_STATE_OK, 429));
    ck_assert(metaserver_publish_response_retryable(CURL_STATE_ERROR, 503));
    ck_assert(metaserver_publish_response_retryable(CURL_STATE_ERROR, 200));
    ck_assert(!metaserver_publish_response_retryable(CURL_STATE_OK, 200));
    ck_assert(!metaserver_publish_response_retryable(CURL_STATE_OK, 400));
    ck_assert(!metaserver_publish_response_retryable(CURL_STATE_ERROR, 401));
    ck_assert(!metaserver_publish_response_retryable(CURL_STATE_OK, 403));
    ck_assert(!metaserver_publish_response_retryable(CURL_STATE_OK, 404));
    ck_assert(!metaserver_publish_response_retryable(CURL_STATE_OK, 409));
    ck_assert_int_eq(metaserver_publish_failure_action(CURL_STATE_ERROR, 409),
                     METASERVER_PUBLISH_FAILURE_REPLAY);
    ck_assert_int_eq(metaserver_publish_failure_action(CURL_STATE_ERROR, 503),
                     METASERVER_PUBLISH_FAILURE_RETRY);
    ck_assert_int_eq(metaserver_publish_failure_action(CURL_STATE_ERROR, 401),
                     METASERVER_PUBLISH_FAILURE_SUSPEND);

    metaserver_publish_cadence_t private_cadence;
    metaserver_publish_cadence_init(&private_cadence, (server_monotonic_t){UINT64_C(1000000)});
    ck_assert(metaserver_publish_cadence_attempted(&private_cadence,
                                                   (server_monotonic_t){UINT64_C(1000000)}));
    metaserver_publish_cadence_succeeded(&private_cadence,
                                         (server_monotonic_t){UINT64_C(1000000)},
                                         false,
                                         METASERVER_PUBLISH_HEARTBEAT_DEFAULT_SECONDS,
                                         0);
    ck_assert(private_cadence.published);
    ck_assert(!server_monotonic_is_set(private_cadence.heartbeat_deadline));
    ck_assert(!metaserver_publish_cadence_needs_snapshot(
        &private_cadence,
        (server_monotonic_t){UINT64_C(86401) * UINT64_C(1000000)}));
    ck_assert(
        !metaserver_publish_cadence_due(&private_cadence,
                                        (server_monotonic_t){UINT64_C(86401) * UINT64_C(1000000)},
                                        true));

    metaserver_publish_cadence_t cadence;
    server_monotonic_t now = {UINT64_C(1000000)};
    metaserver_publish_cadence_init(&cadence, now);
    unsigned int attempts = 0;
    while (now.microseconds <= UINT64_C(86401) * UINT64_C(1000000)) {
        if (metaserver_publish_cadence_due(&cadence, now, true)) {
            ck_assert(metaserver_publish_cadence_attempted(&cadence, now));
            attempts++;
            metaserver_publish_cadence_changed(&cadence, now, true);
        }
        now.microseconds += UINT64_C(1000000);
    }
    ck_assert_uint_eq(attempts, 47);

    metaserver_publish_cadence_init(&cadence, (server_monotonic_t){UINT64_C(1000000)});
    ck_assert(
        metaserver_publish_cadence_attempted(&cadence, (server_monotonic_t){UINT64_C(1000000)}));
    metaserver_publish_cadence_failed(&cadence, (server_monotonic_t){UINT64_C(1000000)}, 120, 0);
    ck_assert(
        !metaserver_publish_cadence_due(&cadence, (server_monotonic_t){UINT64_C(120999999)}, true));
    ck_assert(
        metaserver_publish_cadence_due(&cadence, (server_monotonic_t){UINT64_C(121000000)}, true));

    ck_assert(metaserver_rendezvous_upgrade_retryable(CURLE_COULDNT_CONNECT, 0, false));
    ck_assert(metaserver_rendezvous_upgrade_retryable(CURLE_HTTP_RETURNED_ERROR, 408, false));
    ck_assert(metaserver_rendezvous_upgrade_retryable(CURLE_HTTP_RETURNED_ERROR, 429, false));
    ck_assert(metaserver_rendezvous_upgrade_retryable(CURLE_HTTP_RETURNED_ERROR, 503, false));
    ck_assert(metaserver_rendezvous_upgrade_retryable(CURLE_OK, 429, false));
    ck_assert(metaserver_rendezvous_upgrade_retryable(CURLE_OK, 503, false));
    ck_assert(!metaserver_rendezvous_upgrade_retryable(CURLE_HTTP_RETURNED_ERROR, 401, false));
    ck_assert(!metaserver_rendezvous_upgrade_retryable(CURLE_HTTP_RETURNED_ERROR, 403, false));
    ck_assert(!metaserver_rendezvous_upgrade_retryable(CURLE_HTTP_RETURNED_ERROR, 404, false));
    ck_assert(!metaserver_rendezvous_upgrade_retryable(CURLE_HTTP_RETURNED_ERROR, 409, false));
    ck_assert(!metaserver_rendezvous_upgrade_retryable(CURLE_WRITE_ERROR, 401, false));
    ck_assert(!metaserver_rendezvous_upgrade_retryable(CURLE_OK, 401, false));
    ck_assert(!metaserver_rendezvous_upgrade_retryable(CURLE_OK, 409, false));
    ck_assert(!metaserver_rendezvous_upgrade_retryable(CURLE_OK, 101, false));

    metaserver_attempt_budget_t budget;
    server_monotonic_t budget_now = {UINT64_C(1000000)};
    uint32_t wait_ms = UINT32_MAX;
    metaserver_attempt_budget_init(&budget, budget_now);
    ck_assert(metaserver_attempt_budget_consume(&budget, budget_now, &wait_ms));
    ck_assert_uint_eq(wait_ms, 0);
    ck_assert(metaserver_attempt_budget_consume(&budget, budget_now, &wait_ms));
    ck_assert(!metaserver_attempt_budget_consume(&budget, budget_now, &wait_ms));
    ck_assert_uint_eq(wait_ms, METASERVER_ATTEMPT_RATE_REFILL_SECONDS * 1000U);
    server_monotonic_t budget_refill = {budget_now.microseconds +
                                        METASERVER_ATTEMPT_RATE_REFILL_SECONDS * UINT64_C(1000000)};
    ck_assert(metaserver_attempt_budget_consume(&budget, budget_refill, &wait_ms));

    metaserver_attempt_budget_init(&budget, budget_now);
    unsigned int rendezvous_attempts = 0;
    server_monotonic_t rendezvous_now = budget_now;
    while (rendezvous_now.microseconds <=
           budget_now.microseconds + UINT64_C(86400) * UINT64_C(1000000)) {
        if (metaserver_attempt_budget_consume(&budget, rendezvous_now, &wait_ms)) {
            rendezvous_attempts++;
        }
        rendezvous_now.microseconds += UINT64_C(1000000);
    }
    ck_assert_uint_eq(rendezvous_attempts, 47);
    ck_assert(!metaserver_attempt_budget_consume(&budget, rendezvous_now, &wait_ms));

    uint64_t attempt_times[128];
    size_t attempt_times_count = 0;
    metaserver_attempt_budget_init(&budget, budget_now);
    for (uint64_t second = 0; second <= UINT64_C(172800); second++) {
        server_monotonic_t sample = {
            budget_now.microseconds + second * UINT64_C(1000000),
        };
        if (metaserver_attempt_budget_consume(&budget, sample, &wait_ms)) {
            ck_assert_uint_lt(attempt_times_count, arraysize(attempt_times));
            attempt_times[attempt_times_count++] = sample.microseconds;
        }
    }
    static const uint64_t window_offsets[] = {0, 1, 1919, 1920, 1921, 3839, 3840, 86399};
    for (size_t i = 0; i < arraysize(window_offsets); i++) {
        uint64_t window_start = budget_now.microseconds + window_offsets[i] * UINT64_C(1000000);
        uint64_t window_end = window_start + UINT64_C(86400) * UINT64_C(1000000);
        unsigned int window_attempts = 0;
        for (size_t j = 0; j < attempt_times_count; j++) {
            if (attempt_times[j] >= window_start && attempt_times[j] <= window_end) {
                window_attempts++;
            }
        }
        ck_assert_uint_le(window_attempts, 47);
        if (window_offsets[i] == 0) {
            ck_assert_uint_eq(window_attempts, 47);
        }
    }

    metaserver_attempt_budget_init(&budget, budget_now);
    ck_assert(metaserver_attempt_budget_consume(&budget, budget_now, &wait_ms));
    ck_assert(metaserver_attempt_budget_consume(&budget, budget_now, &wait_ms));
    uint64_t rendezvous_generation = 1;
    rendezvous_generation++;
    ck_assert(metaserver_rendezvous_generation_allows(rendezvous_generation,
                                                      rendezvous_generation,
                                                      false));
    ck_assert(!metaserver_attempt_budget_consume(&budget, budget_now, &wait_ms));
    ck_assert(metaserver_attempt_budget_consume(&budget, budget_refill, &wait_ms));
}
END_TEST

START_TEST(test_metaserver_rendezvous_ticket_isolation) {
    static const char ticket_a[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char ticket_b[] =
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    metaserver_rendezvous_auth_job_t jobs[2] = {0};
    metaserver_rendezvous_auth_job_t *job_a = NULL, *job_b = NULL, *claimed = NULL;
    ck_assert_int_eq(metaserver_rendezvous_auth_claim(jobs, arraysize(jobs), ticket_a, 100, &job_a),
                     METASERVER_RENDEZVOUS_AUTH_CLAIM_OK);
    ck_assert_int_eq(metaserver_rendezvous_auth_claim(jobs, arraysize(jobs), ticket_b, 200, &job_b),
                     METASERVER_RENDEZVOUS_AUTH_CLAIM_OK);
    ck_assert_ptr_nonnull(job_a);
    ck_assert_ptr_nonnull(job_b);
    job_a->state = RENDEZVOUS_SERVER_AUTH_WAIT_PROOF;
    job_b->state = RENDEZVOUS_SERVER_AUTH_WAIT_PROOF;

    ck_assert_int_eq(
        metaserver_rendezvous_auth_claim(jobs, arraysize(jobs), ticket_a, 300, &claimed),
        METASERVER_RENDEZVOUS_AUTH_CLAIM_DUPLICATE);
    ck_assert_ptr_null(claimed);
    ck_assert_ptr_eq(metaserver_rendezvous_auth_find(jobs,
                                                     arraysize(jobs),
                                                     ticket_b,
                                                     RENDEZVOUS_SERVER_AUTH_WAIT_PROOF),
                     job_b);
    ck_assert_ptr_null(metaserver_rendezvous_auth_find(jobs,
                                                       arraysize(jobs),
                                                       "malformed",
                                                       RENDEZVOUS_SERVER_AUTH_WAIT_PROOF));
    ck_assert_int_eq(
        metaserver_rendezvous_auth_claim(jobs, arraysize(jobs), "malformed", 300, &claimed),
        METASERVER_RENDEZVOUS_AUTH_CLAIM_INVALID);
    ck_assert(job_a->active);
    ck_assert(job_b->active);

    metaserver_rendezvous_auth_expire(jobs, arraysize(jobs), 150);
    ck_assert(!job_a->active);
    ck_assert(job_b->active);
    ck_assert_str_eq(job_b->ticket, ticket_b);

    ck_assert_int_eq(metaserver_rendezvous_auth_claim(jobs, 1, ticket_a, 300, &claimed),
                     METASERVER_RENDEZVOUS_AUTH_CLAIM_OK);
    ck_assert_int_eq(metaserver_rendezvous_auth_claim(jobs, 1, ticket_b, 300, &claimed),
                     METASERVER_RENDEZVOUS_AUTH_CLAIM_FULL);
    ck_assert(job_b->active);
    ck_assert_str_eq(job_b->ticket, ticket_b);
    for (size_t i = 0; i < arraysize(jobs); i++) {
        metaserver_rendezvous_auth_clear(&jobs[i]);
    }

    metaserver_rendezvous_auth_job_t full_jobs[METASERVER_RENDEZVOUS_AUTH_JOBS_MAX] = {0};
    char generated_ticket[RENDEZVOUS_TICKET_HEX_SIZE + 1U];
    for (size_t i = 0; i < arraysize(full_jobs); i++) {
        ck_assert_int_eq(snprintf(VS(generated_ticket), "%064zx", i), RENDEZVOUS_TICKET_HEX_SIZE);
        ck_assert_int_eq(metaserver_rendezvous_auth_claim(full_jobs,
                                                          arraysize(full_jobs),
                                                          generated_ticket,
                                                          1000,
                                                          &claimed),
                         METASERVER_RENDEZVOUS_AUTH_CLAIM_OK);
    }
    ck_assert_int_eq(snprintf(VS(generated_ticket), "%064x", METASERVER_RENDEZVOUS_AUTH_JOBS_MAX),
                     RENDEZVOUS_TICKET_HEX_SIZE);
    ck_assert_int_eq(metaserver_rendezvous_auth_claim(full_jobs,
                                                      arraysize(full_jobs),
                                                      generated_ticket,
                                                      1000,
                                                      &claimed),
                     METASERVER_RENDEZVOUS_AUTH_CLAIM_FULL);
    ck_assert_ptr_null(claimed);
    for (size_t i = 0; i < arraysize(full_jobs); i++) {
        ck_assert(full_jobs[i].active);
        metaserver_rendezvous_auth_clear(&full_jobs[i]);
    }
}
END_TEST

START_TEST(test_metaserver_generation_cancellation) {
    const rendezvous_server_auth_state_t stages[] = {
        RENDEZVOUS_SERVER_AUTH_NEW,
        RENDEZVOUS_SERVER_AUTH_WAIT_PROOF,
        RENDEZVOUS_SERVER_AUTH_AUTHORIZED,
    };
    for (size_t i = 0; i < arraysize(stages); i++) {
        ck_assert_int_ne(stages[i], RENDEZVOUS_SERVER_AUTH_CONSUMED);
        ck_assert(metaserver_rendezvous_generation_allows(7, 7, false));
        ck_assert(!metaserver_rendezvous_generation_allows(8, 7, false));
        ck_assert(!metaserver_rendezvous_generation_allows(7, 7, true));
    }
}
END_TEST

START_TEST(test_metaserver_raw_endpoint_not_published) {
    char host[65] = "sentinel";
    uint16_t port = 0;
    ck_assert(!metaserver_public_endpoint_from_config("1.1.1.1", 1730, VS(host), &port));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 1730);

    ck_assert(!metaserver_public_endpoint_from_config("", 1731, VS(host), &port));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 1731);
    ck_assert(!metaserver_public_endpoint_from_config("192.168.1.10", 1732, VS(host), &port));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 1732);
    ck_assert(
        !metaserver_public_endpoint_from_config("server.example.invalid", 1733, VS(host), &port));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 1733);
#ifdef HAVE_IPV6
    ck_assert(
        !metaserver_public_endpoint_from_config("2606:4700:4700::1111", 1734, VS(host), &port));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 1734);
#endif
}
END_TEST

START_TEST(test_path_secret_reader) {
    char path[] = "/tmp/atrinik-secret-test.XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ne(fd, -1);
    static const char value[] = "correct horse\r\n";
    ck_assert_int_eq(write(fd, value, sizeof(value) - 1), (ssize_t)(sizeof(value) - 1));
#ifndef WIN32
    ck_assert_int_eq(fchmod(fd, 0644), 0);
#endif
    ck_assert_int_eq(close(fd), 0);

    char secret[32];
    bool permissive = false;
    ck_assert_int_eq(path_read_secret(path, VS(secret), &permissive), PATH_SECRET_OK);
    ck_assert_str_eq(secret, "correct horse");
#ifndef WIN32
    ck_assert(permissive);
#endif

    fd = open(path, O_WRONLY | O_TRUNC);
    ck_assert_int_ne(fd, -1);
    static const char too_long[] = "a secret that cannot fit";
    ck_assert_int_eq(write(fd, too_long, sizeof(too_long) - 1), (ssize_t)(sizeof(too_long) - 1));
    ck_assert_int_eq(close(fd), 0);
    char small[8];
    memset(small, 0xaa, sizeof(small));
    ck_assert_int_eq(path_read_secret(path, VS(small), NULL), PATH_SECRET_TOO_LONG);
    static const char cleared[sizeof(small)];
    ck_assert_mem_eq(small, cleared, sizeof(small));

    fd = open(path, O_WRONLY | O_TRUNC);
    ck_assert_int_ne(fd, -1);
    static const char trailing[] = "valid secret\nsecond secret\n";
    ck_assert_int_eq(write(fd, trailing, sizeof(trailing) - 1), (ssize_t)(sizeof(trailing) - 1));
    ck_assert_int_eq(close(fd), 0);
    ck_assert_int_eq(path_read_secret(path, VS(secret), NULL), PATH_SECRET_TRAILING_DATA);
    static const char cleared_secret[sizeof(secret)];
    ck_assert_mem_eq(secret, cleared_secret, sizeof(secret));

#if !defined(WIN32) && defined(O_NOFOLLOW)
    char link_path[sizeof(path) + 8];
    snprintf(VS(link_path), "%s.link", path);
    ck_assert_int_eq(symlink(path, link_path), 0);
    ck_assert_int_eq(path_read_secret(link_path, VS(secret), NULL), PATH_SECRET_UNSAFE_LINK);
    ck_assert_int_eq(unlink(link_path), 0);
#endif

    ck_assert_int_eq(path_read_secret("/tmp", VS(secret), NULL), PATH_SECRET_NOT_REGULAR);
    ck_assert_int_eq(unlink(path), 0);
}
END_TEST

START_TEST(test_path_safe_relative) {
    ck_assert(path_is_safe_relative("client-maps/world.png"));
    ck_assert(path_is_safe_relative("settings/file"));
    ck_assert(!path_is_safe_relative("../outside"));
    ck_assert(!path_is_safe_relative("inside/../outside"));
    ck_assert(!path_is_safe_relative("/absolute"));
    ck_assert(!path_is_safe_relative("C:\\absolute"));
    ck_assert(!path_is_safe_relative("double//component"));
}
END_TEST

START_TEST(test_path_write_atomic_replaces_complete_file) {
    char path[] = "/tmp/atrinik-path-test.XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ne(fd, -1);
    ck_assert_int_eq(close(fd), 0);
    ck_assert_int_eq(unlink(path), 0);

    static const char first[] = "first";
    static const char second[] = "replacement";
    ck_assert(path_write_atomic(path, first, sizeof(first) - 1, 0600));
    ck_assert(path_write_atomic(path, second, sizeof(second) - 1, 0600));

    FILE *fp = fopen(path, "rb");
    ck_assert_ptr_nonnull(fp);
    char contents[sizeof(second)] = {0};
    ck_assert_uint_eq(fread(contents, 1, sizeof(second) - 1, fp), sizeof(second) - 1);
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert_str_eq(contents, second);
#ifndef WIN32
    struct stat sb;
    ck_assert_int_eq(stat(path, &sb), 0);
    ck_assert_int_eq(sb.st_mode & 0777, 0600);
#endif
    ck_assert_int_eq(unlink(path), 0);
}
END_TEST

START_TEST(test_path_secret_create_atomic_no_replace) {
    char path[] = "/tmp/atrinik-secret-create-test.XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ne(fd, -1);
    ck_assert_int_eq(close(fd), 0);
    ck_assert_int_eq(unlink(path), 0);

    static const char first[] = "first secret";
    static const char second[] = "replacement secret";
    ck_assert_int_eq(path_secret_create_atomic(path, first, sizeof(first) - 1U),
                     PATH_SECRET_CREATE_OK);
    ck_assert_int_eq(path_secret_create_atomic(path, second, sizeof(second) - 1U),
                     PATH_SECRET_CREATE_EXISTS);

    char secret[32];
    bool permissive = true;
    ck_assert_int_eq(path_read_secret(path, VS(secret), &permissive), PATH_SECRET_OK);
    ck_assert_str_eq(secret, first);
    ck_assert(!permissive);
#ifndef WIN32
    struct stat metadata;
    ck_assert_int_eq(lstat(path, &metadata), 0);
    ck_assert(S_ISREG(metadata.st_mode));
    ck_assert_uint_eq(metadata.st_uid, geteuid());
    ck_assert_uint_eq(metadata.st_mode & 0777, 0600);
    ck_assert_uint_eq(metadata.st_nlink, 1);
#endif
    ck_assert_int_eq(unlink(path), 0);
}
END_TEST

START_TEST(test_socket_rendezvous_messages) {
    static const char server_id[] =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    static const char ticket[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char other_ticket[] =
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    rendezvous_invite_t invite = {
        .server_id = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        .invite_id = "00112233445566778899aabbccddeeff",
        .expiry = UINT64_MAX,
    };
    memset(invite.secret, 0x42, sizeof(invite.secret));
    socket_rendezvous_attempt_t *attempt =
        socket_rendezvous_attempt_create(server_id, ticket, &invite, UINT64_MAX);
    ck_assert_ptr_nonnull(attempt);

    char message[RENDEZVOUS_FRAME_MAX + 1U], proof_frame[RENDEZVOUS_FRAME_MAX + 1U];
    ck_assert(socket_rendezvous_attempt_auth_init(attempt, VS(message)));
    char parsed_ticket[65], invite_id[33];
    ck_assert(rendezvous_auth_init_parse(message, parsed_ticket, invite_id));
    ck_assert_str_eq(parsed_ticket, ticket);
    ck_assert_str_eq(invite_id, invite.invite_id);

    unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE];
    memset(challenge, 0x24, sizeof(challenge));
    ck_assert(rendezvous_auth_challenge_render(VS(message), ticket, challenge));
    rendezvous_server_auth_state_t server_auth = RENDEZVOUS_SERVER_AUTH_NEW;
    ck_assert(rendezvous_server_auth_challenge_sent(&server_auth));
    ck_assert_int_eq(
        socket_rendezvous_attempt_challenge(attempt, message, strlen(message), VS(proof_frame)),
        SOCKET_RENDEZVOUS_FRAME_CHALLENGE);
    unsigned char proof[RENDEZVOUS_PROOF_SIZE];
    ck_assert(rendezvous_auth_proof_parse(proof_frame, ticket, proof));
    ck_assert(rendezvous_auth_result_render(VS(message), ticket, true));
    ck_assert(rendezvous_server_auth_result_sent(&server_auth, true));
    ck_assert_int_eq(socket_rendezvous_attempt_auth_result(attempt, message, strlen(message)),
                     SOCKET_RENDEZVOUS_FRAME_AUTHORIZED);
    ck_assert(socket_rendezvous_attempt_client_candidate(attempt, "192.0.2.10", 1730, VS(message)));

    char host[65];
    uint16_t port;
    rendezvous_server_auth_state_t preauth = RENDEZVOUS_SERVER_AUTH_NEW;
    ck_assert(!socket_rendezvous_client_candidate_parse(message,
                                                        ticket,
                                                        true,
                                                        preauth,
                                                        VS(host),
                                                        &port,
                                                        parsed_ticket));
    ck_assert(socket_rendezvous_client_candidate_parse(message,
                                                       ticket,
                                                       true,
                                                       server_auth,
                                                       VS(host),
                                                       &port,
                                                       parsed_ticket));
    ck_assert_str_eq(host, "192.0.2.10");
    ck_assert_uint_eq(port, 1730);
    ck_assert_str_eq(parsed_ticket, ticket);
    ck_assert(!socket_rendezvous_client_candidate_parse(message,
                                                        other_ticket,
                                                        true,
                                                        server_auth,
                                                        VS(host),
                                                        &port,
                                                        parsed_ticket));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 0);
    ck_assert_str_eq(parsed_ticket, "");

    socket_direct_candidate_t candidate = {
        .host = "2001:db8::1",
        .port = 1730,
        .kind = SOCKET_CANDIDATE_IPV6,
    };
    ck_assert(!socket_rendezvous_server_candidate_render(VS(proof_frame),
                                                         &candidate,
                                                         ticket,
                                                         true,
                                                         preauth));
    ck_assert(socket_rendezvous_server_candidate_render(VS(message),
                                                        &candidate,
                                                        ticket,
                                                        true,
                                                        server_auth));
    memset(&candidate, 0, sizeof(candidate));
    ck_assert_int_eq(
        socket_rendezvous_attempt_server_frame(attempt, message, strlen(message), &candidate),
        SOCKET_RENDEZVOUS_FRAME_CANDIDATE);
    ck_assert_str_eq(candidate.host, "2001:db8::1");
    ck_assert_uint_eq(candidate.port, 1730);
    ck_assert_int_eq(candidate.kind, SOCKET_CANDIDATE_IPV6);

    ck_assert(socket_rendezvous_complete_render(VS(message), ticket));
    ck_assert(socket_rendezvous_complete_parse(message, ticket));
    ck_assert_int_eq(
        socket_rendezvous_attempt_server_frame(attempt, message, strlen(message), &candidate),
        SOCKET_RENDEZVOUS_FRAME_COMPLETE);
    ck_assert(!socket_rendezvous_client_candidate_parse(
        "{\"type\":\"client_candidate\",\"host\":\"example.com\","
        "\"port\":1730,\"ticket\":\"bad\"}",
        NULL,
        false,
        RENDEZVOUS_SERVER_AUTH_NEW,
        VS(host),
        &port,
        parsed_ticket));
    socket_rendezvous_attempt_destroy(attempt);
    rendezvous_invite_cleanse(&invite);
    OPENSSL_cleanse(challenge, sizeof(challenge));
    OPENSSL_cleanse(proof, sizeof(proof));
    OPENSSL_cleanse(proof_frame, sizeof(proof_frame));
}
END_TEST

START_TEST(test_socket_asset_request_rejects_malformed) {
    packet_struct *packet = packet_new(0, 0, 0);
    uint8_t digest[ASSET_DIGEST_SIZE] = {0};
    socket_asset_request_append(packet, "data/listing.txt", 0, digest, 0);

    socket_asset_request_t request;
    memset(&request, 0xa5, sizeof(request));
    const socket_asset_request_t unchanged_request = request;
    for (size_t truncated = 0; truncated < packet->len; truncated++) {
        ck_assert(!socket_asset_request_parse(packet->data, truncated, 0, &request));
        ck_assert_mem_eq(&request, &unchanged_request, sizeof(request));
    }
    packet_writer_write_uint8(packet, 0);
    ck_assert(!socket_asset_request_parse(packet->data, packet->len, 0, &request));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    digest[0] = 3;
    socket_asset_request_append(packet, "data/listing.txt", 2, digest, ASSET_REQUEST_METADATA);
    ck_assert(!socket_asset_request_parse(packet->data, packet->len, 0, &request));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    memset(digest, 0, sizeof(digest));
    socket_asset_request_append(packet, "data/listing.txt", 0, digest, ASSET_REQUEST_METADATA);
    ck_assert(socket_asset_request_parse(packet->data, packet->len, 0, &request));
    ck_assert_uint_eq(request.flags, ASSET_REQUEST_METADATA);
    packet_free(packet);
}
END_TEST

START_TEST(test_socket_asset_response_round_trip) {
    static const uint8_t chunk[] = {0xde, 0xad, 0xbe, 0xef};
    uint8_t digest[ASSET_DIGEST_SIZE];
    memset(digest, 0x12, sizeof(digest));
    packet_struct *packet = packet_new(0, 0, 0);
    socket_asset_response_append_ok(packet, sizeof(chunk), digest);

    socket_asset_response_t response;
    memset(&response, 0xa5, sizeof(response));
    const socket_asset_response_t unchanged_response = response;
    for (size_t truncated = 0; truncated < packet->len; truncated++) {
        ck_assert(!socket_asset_response_parse(packet->data, truncated, 0, &response));
        ck_assert_mem_eq(&response, &unchanged_response, sizeof(response));
    }
    ck_assert(socket_asset_response_parse(packet->data, packet->len, 0, &response));
    ck_assert_uint_eq(response.status, ASSET_STATUS_OK);
    ck_assert_uint_eq(response.total_size, sizeof(chunk));
    ck_assert_mem_eq(response.digest, digest, sizeof(digest));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    socket_asset_response_append_status(packet, ASSET_STATUS_NOT_MODIFIED, sizeof(chunk), digest);
    ck_assert(socket_asset_response_parse(packet->data, packet->len, 0, &response));
    ck_assert_uint_eq(response.status, ASSET_STATUS_NOT_MODIFIED);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    socket_asset_response_append_metadata(packet, sizeof(chunk), digest);
    ck_assert(socket_asset_response_parse(packet->data, packet->len, 0, &response));
    ck_assert_uint_eq(response.status, ASSET_STATUS_METADATA);
    ck_assert_uint_eq(response.total_size, sizeof(chunk));
    ck_assert_mem_eq(response.digest, digest, sizeof(digest));
    packet_free(packet);
}
END_TEST

START_TEST(test_socket_asset_response_rejects_malformed) {
    uint8_t digest[ASSET_DIGEST_SIZE] = {0};
    socket_asset_response_t response;
    uint8_t unknown[] = {0xff, 'x', '\0'};
    ck_assert(!socket_asset_response_parse(unknown, sizeof(unknown), 0, &response));

    packet_struct *packet = packet_new(0, 0, 0);
    socket_asset_response_append_status(packet, ASSET_STATUS_NOT_MODIFIED, 0, digest);
    packet_writer_write_uint8(packet, 0);
    ck_assert(!socket_asset_response_parse(packet->data, packet->len, 0, &response));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    socket_asset_response_append_ok(packet, ASSET_MAX_SIZE + 1U, digest);
    ck_assert(!socket_asset_response_parse(packet->data, packet->len, 0, &response));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    socket_asset_response_append_status(packet, ASSET_STATUS_NOT_FOUND, 1, digest);
    ck_assert(!socket_asset_response_parse(packet->data, packet->len, 0, &response));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    digest[0] = 1;
    socket_asset_response_append_status(packet, ASSET_STATUS_NOT_FOUND, 0, digest);
    ck_assert(!socket_asset_response_parse(packet->data, packet->len, 0, &response));
    packet_free(packet);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("socket_asset");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_socket_asset_request_round_trip);
    tcase_add_test(tc_core, test_socket_asset_face_path_round_trip_and_malformed);
    tcase_add_test(tc_core, test_socket_face_asset_snapshot_is_bounded_and_authenticated);
    tcase_add_test(tc_core, test_socket_asset_global_work_budget_rotates);
    tcase_add_test(tc_core, test_socket_asset_byte_budget_tracks_processing_rate);
    tcase_add_test(tc_core, test_socket_asset_connection_budget_is_fair_and_work_conserving);
    tcase_add_test(tc_core, test_socket_face_batch_worst_case_service_bound);
    tcase_add_test(tc_core, test_socket_asset_logical_rate_accounting_is_atomic);
    tcase_add_test(tc_core, test_socket_stream_preface_round_trip_and_malformed);
    tcase_add_test(tc_core, test_socket_asset_request_rejects_malformed);
    tcase_add_test(tc_core, test_socket_asset_response_round_trip);
    tcase_add_test(tc_core, test_socket_asset_response_rejects_malformed);
    tcase_add_test(tc_core, test_socket_rendezvous_messages);
    tcase_add_test(tc_core, test_metaserver_rendezvous_token_bounds);
    tcase_add_test(tc_core, test_metaserver_rendezvous_retry_policy);
    tcase_add_test(tc_core, test_metaserver_publish_cadence);
    tcase_add_test(tc_core, test_metaserver_publish_cadence_attempt_is_fail_closed);
    tcase_add_test(tc_core, test_metaserver_publish_retry_and_daily_budget);
    tcase_add_test(tc_core, test_metaserver_rendezvous_ticket_isolation);
    tcase_add_test(tc_core, test_metaserver_generation_cancellation);
    tcase_add_test(tc_core, test_metaserver_raw_endpoint_not_published);
    tcase_add_test(tc_core, test_path_write_atomic_replaces_complete_file);
    tcase_add_test(tc_core, test_path_secret_create_atomic_no_replace);
    tcase_add_test(tc_core, test_path_secret_reader);
    tcase_add_test(tc_core, test_path_safe_relative);

#if OPENSSL_VERSION_NUMBER >= 0x30500000L
    TCase *tc_loopback = tcase_create("Loopback");
    tcase_add_unchecked_fixture(tc_loopback, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_loopback, check_test_setup, check_test_teardown);
    tcase_set_timeout(tc_loopback, 20.0);
    tcase_add_test(tc_loopback, test_socket_face_asset_borrowed_body_loopback);
    tcase_add_test(tc_loopback, test_socket_face_batch_production_loopback);
    suite_add_tcase(s, tc_loopback);
#endif

    return s;
}

void check_server_socket_asset(void) {
    check_run_suite(suite(), __FILE__);
}
