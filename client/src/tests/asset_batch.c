/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Atrinik Development Team                         *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/** @file Deterministic negotiated face-batch scheduler and parser tests. */

#include <global.h>
#include <client_socket.h>
#include <network_graph.h>
#include <toolkit/packet.h>
#include <wrapper.h>
#include <openssl/evp.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

#define TEST_RESPONSE_MAX (ASSET_FACE_BATCH_MAX * (SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE + 64U))
#define TEST_OPEN_MAX 32U

typedef enum test_response_mode {
    TEST_RESPONSE_OK,
    TEST_RESPONSE_NOT_FOUND_FIRST,
    TEST_RESPONSE_WRONG_FACE,
    TEST_RESPONSE_TRUNCATED,
    TEST_RESPONSE_BAD_DIGEST_FIRST,
    TEST_RESPONSE_SURPLUS,
    TEST_RESPONSE_RESET_AFTER_RECORDS,
} test_response_mode_t;

struct sock_struct {
    bool quic;
};

struct socket_stream {
    socket_stream_kind_t kind;
    size_t record_index;
    uint8_t request[SOCKET_ASSET_REQUEST_MAX_SIZE];
    size_t request_size;
    uint8_t response[TEST_RESPONSE_MAX];
    size_t response_size;
    size_t response_pos;
    bool concluded;
    bool destroyed;
};

typedef struct test_open_record {
    socket_stream_kind_t kind;
    size_t count;
    uint16_t faces[ASSET_FACE_BATCH_MAX];
} test_open_record_t;

Client_Player cpl;

static test_response_mode_t test_response_mode;
static test_open_record_t test_opens[TEST_OPEN_MAX];
static size_t test_open_count;
static size_t test_active;
static size_t test_maximum_active;
static size_t test_cancel_resets;
static size_t test_protocol_resets;

char *file_path(const char *path, const char *mode) {
    (void)mode;
    return xstrdup(path);
}

void network_graph_update(int type, int traffic, size_t bytes) {
    (void)type;
    (void)traffic;
    (void)bytes;
}

bool socket_is_quic(socket_t *sc) {
    return sc != NULL && sc->quic;
}

static void test_body(uint16_t face, uint8_t body[64], size_t *size) {
    *size = 13U + face % 17U;
    for (size_t i = 0; i < *size; i++) {
        body[i] = (uint8_t)(face + i);
    }
}

static void test_response_append(packet_struct *packet, uint16_t face, size_t index) {
    if (test_response_mode == TEST_RESPONSE_NOT_FOUND_FIRST && index == 0) {
        TEST_CHECK(
            socket_face_batch_response_append(packet, face, ASSET_STATUS_NOT_FOUND, 0, NULL));
        return;
    }

    uint8_t body[64];
    size_t body_size = 0;
    test_body(face, body, &body_size);
    uint8_t digest[ASSET_DIGEST_SIZE];
    unsigned int digest_size = 0;
    TEST_CHECK(EVP_Digest(body, body_size, digest, &digest_size, EVP_sha256(), NULL) == 1);
    TEST_CHECK(digest_size == sizeof(digest));
    if (test_response_mode == TEST_RESPONSE_BAD_DIGEST_FIRST && index == 0) {
        digest[0] ^= 0x80U;
    }
    uint16_t echoed =
        test_response_mode == TEST_RESPONSE_WRONG_FACE && index == 0 ? face + 1U : face;
    TEST_CHECK(socket_face_batch_response_append(packet,
                                                 echoed,
                                                 ASSET_STATUS_OK,
                                                 (uint32_t)body_size,
                                                 digest));
    packet_writer_write_bytes(packet, body, body_size);
}

static bool test_stream_prepare(socket_stream_t *stream) {
    TEST_CHECK(stream->record_index < test_open_count);
    test_open_record_t *record = &test_opens[stream->record_index];
    packet_struct *response = packet_new(0, TEST_RESPONSE_MAX, 0);
    if (stream->kind == SOCKET_STREAM_FACE_BATCH) {
        socket_face_batch_request_t request;
        TEST_CHECK(
            socket_face_batch_request_parse(stream->request, stream->request_size, 0, &request));
        record->count = request.count;
        memcpy(record->faces, request.faces, request.count * sizeof(*request.faces));
        for (size_t i = 0; i < request.count; i++) {
            test_response_append(response, request.faces[i], i);
        }
    } else {
        socket_asset_request_t request;
        TEST_CHECK(socket_asset_request_parse(stream->request, stream->request_size, 0, &request));
        uint16_t face = 0;
        if (socket_asset_face_path_parse(request.path, &face)) {
            record->count = 1;
            record->faces[0] = face;
        }
        uint8_t body[64];
        size_t body_size = 0;
        test_body(face, body, &body_size);
        uint8_t digest[ASSET_DIGEST_SIZE];
        unsigned int digest_size = 0;
        TEST_CHECK(EVP_Digest(body, body_size, digest, &digest_size, EVP_sha256(), NULL) == 1);
        TEST_CHECK(digest_size == sizeof(digest));
        socket_asset_response_append_ok(response, (uint32_t)body_size, digest);
        packet_writer_write_bytes(response, body, body_size);
    }
    TEST_CHECK(packet_writer_finish(response));
    TEST_CHECK(response->len <= sizeof(stream->response));
    stream->response_size = response->len;
    memcpy(stream->response, response->data, response->len);
    packet_free(response);
    if (test_response_mode == TEST_RESPONSE_TRUNCATED) {
        stream->response_size = SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE / 2U;
    } else if (test_response_mode == TEST_RESPONSE_SURPLUS) {
        TEST_CHECK(stream->response_size < sizeof(stream->response));
        stream->response[stream->response_size++] = 0xa5U;
    }
    return true;
}

socket_stream_t *socket_stream_open(socket_t *sc, socket_stream_kind_t kind) {
    TEST_CHECK(sc != NULL);
    TEST_CHECK(sc->quic);
    TEST_CHECK(kind == SOCKET_STREAM_ASSET || kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_open_count < TEST_OPEN_MAX);
    socket_stream_t *stream = xcalloc(1, sizeof(*stream));
    stream->kind = kind;
    stream->record_index = test_open_count;
    test_opens[test_open_count++].kind = kind;
    test_active++;
    test_maximum_active = MAX(test_maximum_active, test_active);
    return stream;
}

socket_stream_result_t
socket_stream_write(socket_stream_t *stream, const void *buf, size_t len, size_t *amt) {
    TEST_CHECK(stream != NULL);
    TEST_CHECK(buf != NULL);
    TEST_CHECK(amt != NULL);
    size_t amount = MIN(len, sizeof(stream->request) - stream->request_size);
    memcpy(stream->request + stream->request_size, buf, amount);
    stream->request_size += amount;
    *amt = amount;
    return SOCKET_STREAM_RESULT_OK;
}

bool socket_stream_conclude(socket_stream_t *stream) {
    TEST_CHECK(stream != NULL);
    TEST_CHECK(!stream->concluded);
    stream->concluded = true;
    return test_stream_prepare(stream);
}

socket_stream_result_t
socket_stream_read(socket_stream_t *stream, void *buf, size_t len, size_t *amt) {
    TEST_CHECK(stream != NULL);
    TEST_CHECK(buf != NULL);
    TEST_CHECK(amt != NULL);
    if (stream->response_pos == stream->response_size) {
        *amt = 0;
        if (test_response_mode == TEST_RESPONSE_RESET_AFTER_RECORDS) {
            return SOCKET_STREAM_RESULT_ERROR;
        }
        return SOCKET_STREAM_RESULT_FINISHED;
    }
    size_t amount = MIN(len, MIN((size_t)7U, stream->response_size - stream->response_pos));
    memcpy(buf, stream->response + stream->response_pos, amount);
    stream->response_pos += amount;
    *amt = amount;
    return SOCKET_STREAM_RESULT_OK;
}

void socket_stream_reset(socket_stream_t *stream, uint64_t error_code) {
    TEST_CHECK(stream != NULL);
    if (error_code == SOCKET_STREAM_ERROR_CANCELLED) {
        test_cancel_resets++;
    } else if (error_code == SOCKET_STREAM_ERROR_CLIENT_PROTOCOL) {
        test_protocol_resets++;
    }
}

void socket_stream_destroy(socket_stream_t *stream) {
    if (stream == NULL) {
        return;
    }
    TEST_CHECK(!stream->destroyed);
    stream->destroyed = true;
    TEST_CHECK(test_active != 0);
    test_active--;
    free(stream);
}

static asset_request_t *test_face_start(uint16_t face) {
    char path[32];
    TEST_CHECK(socket_asset_face_path_format(VS(path), face));
    asset_request_t *request = asset_request_start_bounded(path, ASSET_FACE_MAX_SIZE);
    TEST_CHECK(request != NULL);
    return request;
}

static asset_request_t *test_face_start_priority(uint16_t face) {
    char path[32];
    TEST_CHECK(socket_asset_face_path_format(VS(path), face));
    asset_request_t *request = asset_request_start_bounded_priority(path, ASSET_FACE_MAX_SIZE);
    TEST_CHECK(request != NULL);
    return request;
}

static bool test_terminal(asset_request_t *request) {
    return asset_request_get_state(request) != ASSET_REQUEST_PENDING;
}

static void test_service_until(socket_t *socket, asset_request_t **requests, size_t count) {
    for (size_t pass = 0; pass < 10000U; pass++) {
        bool complete = true;
        for (size_t i = 0; i < count; i++) {
            complete &= requests[i] == NULL || test_terminal(requests[i]);
        }
        if (complete) {
            return;
        }
        bool write_pending = false;
        asset_requests_service(socket, &write_pending);
    }
    TEST_CHECK(false);
}

static void test_begin(socket_t *socket, uint8_t capabilities, test_response_mode_t mode) {
    memset(test_opens, 0, sizeof(test_opens));
    test_open_count = 0;
    test_active = 0;
    test_maximum_active = 0;
    test_cancel_resets = 0;
    test_protocol_resets = 0;
    test_response_mode = mode;
    cpl.asset_transport = (capabilities & ASSET_TRANSPORT_CAP_GENERIC) != 0;
    asset_requests_connect(socket);
    asset_requests_set_capabilities(capabilities);
    TEST_CHECK(asset_requests_available());
    TEST_CHECK(asset_face_batch_available() ==
               ((capabilities & ASSET_TRANSPORT_CAP_FACE_BATCH) != 0));
}

static void test_capability_lifecycle(socket_t *socket) {
    asset_requests_connect(socket);
    TEST_CHECK(!asset_requests_available());
    TEST_CHECK(!asset_face_batch_available());

    asset_requests_set_capabilities((uint8_t)(ASSET_TRANSPORT_CAP_ALL | 0x80U));
    TEST_CHECK(asset_requests_available());
    TEST_CHECK(asset_face_batch_available());
    asset_requests_disconnect();
    TEST_CHECK(!asset_requests_available());
    TEST_CHECK(!asset_face_batch_available());

    asset_requests_connect(socket);
    asset_requests_set_capabilities(ASSET_TRANSPORT_CAP_FACE_BATCH);
    TEST_CHECK(!asset_requests_available());
    TEST_CHECK(!asset_face_batch_available());
    asset_requests_disconnect();
    asset_requests_deinit();
}

static void test_end(asset_request_t **requests, size_t count) {
    for (size_t i = 0; i < count; i++) {
        asset_request_free(requests[i]);
    }
    asset_requests_disconnect();
    TEST_CHECK(!asset_requests_available());
    TEST_CHECK(!asset_face_batch_available());
    asset_requests_deinit();
    TEST_CHECK(test_active == 0);
}

static void test_batch_order_fairness_and_dedup(socket_t *socket) {
    test_begin(socket, ASSET_TRANSPORT_CAP_ALL, TEST_RESPONSE_OK);
    asset_request_t *requests[10] = {test_face_start(1), test_face_start(2)};
    asset_request_t *duplicate = test_face_start(1);
    TEST_CHECK(duplicate == requests[0]);
    asset_request_free(duplicate);
    asset_request_t *generic = asset_request_start("news/test.txt");
    TEST_CHECK(generic != NULL);
    for (size_t face = 3; face <= arraysize(requests); face++) {
        requests[face - 1U] = test_face_start(face);
    }

    bool write_pending = false;
    asset_requests_service(socket, &write_pending);
    TEST_CHECK(test_open_count == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_opens[0].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[0].count == 2);
    TEST_CHECK(test_opens[0].faces[0] == 1 && test_opens[0].faces[1] == 2);
    TEST_CHECK(test_opens[1].kind == SOCKET_STREAM_ASSET);
    TEST_CHECK(test_opens[2].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[2].count == ASSET_FACE_BATCH_MAX);

    asset_request_t *all[11];
    memcpy(all, requests, sizeof(requests));
    all[10] = generic;
    for (size_t pass = 0; pass < 10000U && !test_terminal(generic); pass++) {
        asset_requests_service(socket, &write_pending);
    }
    TEST_CHECK(asset_request_get_state(generic) == ASSET_REQUEST_COMPLETE);
    TEST_CHECK(asset_request_get_state(requests[9]) == ASSET_REQUEST_PENDING);
    test_service_until(socket, all, arraysize(all));
    for (size_t i = 0; i < arraysize(all); i++) {
        TEST_CHECK(asset_request_get_state(all[i]) == ASSET_REQUEST_COMPLETE);
    }
    TEST_CHECK(test_maximum_active == ASSET_STREAM_ACTIVE_MAX);
    test_end(all, arraysize(all));
}

static void test_partial_and_total_cancellation(socket_t *socket) {
    test_begin(socket, ASSET_TRANSPORT_CAP_ALL, TEST_RESPONSE_OK);
    asset_request_t *first = test_face_start(11);
    asset_request_t *second = test_face_start(12);
    bool write_pending = false;
    asset_requests_service(socket, &write_pending);
    asset_request_free(first);
    asset_request_t *live[] = {second};
    test_service_until(socket, live, arraysize(live));
    TEST_CHECK(asset_request_get_state(second) == ASSET_REQUEST_COMPLETE);
    TEST_CHECK(test_cancel_resets == 0);
    test_end(live, arraysize(live));

    test_begin(socket, ASSET_TRANSPORT_CAP_ALL, TEST_RESPONSE_OK);
    first = test_face_start(13);
    second = test_face_start(14);
    asset_requests_service(socket, &write_pending);
    asset_request_free(first);
    asset_request_free(second);
    asset_requests_service(socket, &write_pending);
    TEST_CHECK(test_cancel_resets == 1);
    TEST_CHECK(test_active == 0);
    test_end(NULL, 0);
}

static void test_fallback(socket_t *socket) {
    test_begin(socket, ASSET_TRANSPORT_CAP_GENERIC, TEST_RESPONSE_OK);
    asset_request_t *requests[4];
    for (uint16_t face = 1; face <= arraysize(requests); face++) {
        requests[face - 1U] = test_face_start(face);
    }
    test_service_until(socket, requests, arraysize(requests));
    TEST_CHECK(test_open_count == arraysize(requests));
    for (size_t i = 0; i < test_open_count; i++) {
        TEST_CHECK(test_opens[i].kind == SOCKET_STREAM_ASSET);
        TEST_CHECK(test_opens[i].count == 1);
    }
    TEST_CHECK(test_maximum_active == ASSET_STREAM_ACTIVE_MAX);
    test_end(requests, arraysize(requests));
}

static void test_disconnect_clears_batch_lifecycle(socket_t *socket) {
    test_begin(socket, ASSET_TRANSPORT_CAP_ALL, TEST_RESPONSE_OK);
    asset_request_t *requests[] = {test_face_start(15), test_face_start(16)};
    bool write_pending = false;
    asset_requests_service(socket, &write_pending);
    asset_requests_disconnect();
    TEST_CHECK(test_cancel_resets == 1);
    TEST_CHECK(test_active == 0);
    TEST_CHECK(!asset_requests_available());
    TEST_CHECK(!asset_face_batch_available());
    for (size_t i = 0; i < arraysize(requests); i++) {
        TEST_CHECK(asset_request_get_state(requests[i]) == ASSET_REQUEST_ERROR);
    }

    asset_requests_connect(socket);
    TEST_CHECK(!asset_requests_available());
    TEST_CHECK(!asset_face_batch_available());
    test_end(requests, arraysize(requests));
}

static void test_member_failure_isolation(socket_t *socket) {
    const test_response_mode_t modes[] = {
        TEST_RESPONSE_NOT_FOUND_FIRST,
        TEST_RESPONSE_BAD_DIGEST_FIRST,
    };
    for (size_t mode = 0; mode < arraysize(modes); mode++) {
        test_begin(socket, ASSET_TRANSPORT_CAP_ALL, modes[mode]);
        asset_request_t *requests[] = {test_face_start(21), test_face_start(22)};
        test_service_until(socket, requests, arraysize(requests));
        TEST_CHECK(asset_request_get_state(requests[0]) == ASSET_REQUEST_ERROR);
        TEST_CHECK(asset_request_get_state(requests[1]) == ASSET_REQUEST_COMPLETE);
        TEST_CHECK(test_protocol_resets == 0);
        test_end(requests, arraysize(requests));
    }
}

static void test_stream_failure_isolation(socket_t *socket) {
    const test_response_mode_t modes[] = {
        TEST_RESPONSE_WRONG_FACE,
        TEST_RESPONSE_TRUNCATED,
    };
    for (size_t mode = 0; mode < arraysize(modes); mode++) {
        test_begin(socket, ASSET_TRANSPORT_CAP_ALL, modes[mode]);
        asset_request_t *requests[] = {test_face_start(31), test_face_start(32)};
        test_service_until(socket, requests, arraysize(requests));
        TEST_CHECK(asset_request_get_state(requests[0]) == ASSET_REQUEST_ERROR);
        TEST_CHECK(asset_request_get_state(requests[1]) == ASSET_REQUEST_ERROR);
        TEST_CHECK(test_protocol_resets == 1);
        test_end(requests, arraysize(requests));
    }
}

static void test_final_record_requires_fin(socket_t *socket) {
    const test_response_mode_t modes[] = {
        TEST_RESPONSE_SURPLUS,
        TEST_RESPONSE_RESET_AFTER_RECORDS,
    };
    for (size_t mode = 0; mode < arraysize(modes); mode++) {
        test_begin(socket, ASSET_TRANSPORT_CAP_ALL, modes[mode]);
        asset_request_t *requests[] = {test_face_start(41), test_face_start(42)};
        test_service_until(socket, requests, arraysize(requests));
        TEST_CHECK(asset_request_get_state(requests[0]) == ASSET_REQUEST_COMPLETE);
        TEST_CHECK(asset_request_get_state(requests[1]) == ASSET_REQUEST_ERROR);
        TEST_CHECK(test_protocol_resets == 1);
        test_end(requests, arraysize(requests));
    }
}

static void test_priority_respects_generic_barrier(socket_t *socket) {
    test_begin(socket, ASSET_TRANSPORT_CAP_ALL, TEST_RESPONSE_OK);
    asset_request_t *speculative = test_face_start(51);
    asset_request_t *generic = asset_request_start("news/priority-barrier.txt");
    TEST_CHECK(generic != NULL);
    asset_request_t *priority = test_face_start_priority(52);

    bool write_pending = false;
    asset_requests_service(socket, &write_pending);
    TEST_CHECK(test_open_count == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_opens[0].kind == SOCKET_STREAM_ASSET);
    TEST_CHECK(test_opens[1].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[1].count == 1U);
    TEST_CHECK(test_opens[1].faces[0] == 52U);
    TEST_CHECK(test_opens[2].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[2].count == 1U);
    TEST_CHECK(test_opens[2].faces[0] == 51U);

    asset_request_t *requests[] = {speculative, generic, priority};
    test_end(requests, arraysize(requests));
}

static void test_priority_faces_batch_without_speculative_members(socket_t *socket) {
    test_begin(socket, ASSET_TRANSPORT_CAP_ALL, TEST_RESPONSE_OK);
    TEST_CHECK(asset_request_start_bounded_priority("news/not-a-face.txt", ASSET_FACE_MAX_SIZE) ==
               NULL);
    TEST_CHECK(asset_request_start_bounded_priority("faces/61.png", ASSET_FACE_MAX_SIZE - 1U) ==
               NULL);
    asset_request_t *priority[10];
    for (size_t i = 0; i < arraysize(priority); i++) {
        priority[i] = test_face_start_priority((uint16_t)(61U + i));
    }

    bool write_pending = false;
    asset_requests_service(socket, &write_pending);
    TEST_CHECK(test_open_count == 2U);
    TEST_CHECK(test_opens[0].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[0].count == ASSET_FACE_BATCH_MAX);
    TEST_CHECK(test_opens[1].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[1].count == 2U);

    asset_request_t *replacement = test_face_start_priority(71);
    bool displace_victim = true;
    TEST_CHECK(!asset_request_preempt(priority[0], replacement, &displace_victim));
    TEST_CHECK(!displace_victim);
    TEST_CHECK(test_cancel_resets == 0U);

    asset_request_t *all[arraysize(priority) + 1U];
    memcpy(all, priority, sizeof(priority));
    all[arraysize(priority)] = replacement;
    test_end(all, arraysize(all));
}

static void test_progressed_replacement_does_not_displace_victim(socket_t *socket) {
    test_begin(socket, ASSET_TRANSPORT_CAP_ALL, TEST_RESPONSE_OK);
    asset_request_t *victim = test_face_start(301);
    asset_request_t *replacement = test_face_start_priority(401);
    bool write_pending = false;
    asset_requests_service(socket, &write_pending);
    TEST_CHECK(test_open_count == 2U);
    TEST_CHECK(test_opens[0].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[0].faces[0] == 401U);
    TEST_CHECK(test_opens[1].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[1].faces[0] == 301U);
    TEST_CHECK(!asset_request_preemption_needed(replacement));
    bool displace_victim = true;
    TEST_CHECK(asset_request_preempt(victim, replacement, &displace_victim));
    TEST_CHECK(!displace_victim);
    TEST_CHECK(test_cancel_resets == 0U);
    TEST_CHECK(asset_request_get_state(victim) == ASSET_REQUEST_PENDING);
    TEST_CHECK(asset_request_get_state(replacement) == ASSET_REQUEST_PENDING);

    asset_request_t *all[] = {victim, replacement};
    test_end(all, arraysize(all));
}

static void test_priority_preemption_reclaims_distinct_batches(socket_t *socket) {
    test_begin(socket, ASSET_TRANSPORT_CAP_ALL, TEST_RESPONSE_OK);
    asset_request_t *background[ASSET_STREAM_ACTIVE_MAX * ASSET_FACE_BATCH_MAX + 1U];
    for (size_t i = 0; i < arraysize(background); i++) {
        background[i] = test_face_start((uint16_t)(101U + i));
    }

    bool write_pending = false;
    asset_requests_service(socket, &write_pending);
    TEST_CHECK(test_active == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_open_count == ASSET_STREAM_ACTIVE_MAX);
    for (size_t i = 0; i < ASSET_STREAM_ACTIVE_MAX; i++) {
        TEST_CHECK(test_opens[i].kind == SOCKET_STREAM_FACE_BATCH);
        TEST_CHECK(test_opens[i].count == ASSET_FACE_BATCH_MAX);
    }

    asset_request_t *urgent_a = test_face_start_priority(201);
    /* The 25th logical face is queued behind the three physical batches and
     * therefore cannot reclaim capacity. */
    bool displace_victim = true;
    TEST_CHECK(
        !asset_request_preempt(background[arraysize(background) - 1U], urgent_a, &displace_victim));
    TEST_CHECK(!displace_victim);
    TEST_CHECK(asset_request_preemption_needed(urgent_a));
    TEST_CHECK(asset_request_preempt(background[0], urgent_a, &displace_victim));
    TEST_CHECK(displace_victim);
    TEST_CHECK(!asset_request_preempt(background[1], urgent_a, &displace_victim));
    TEST_CHECK(!displace_victim);
    TEST_CHECK(test_active == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_cancel_resets == 0U);

    /* Match image.c's lifetime: the selected logical victims can be released
     * before the I/O owner observes and consumes the preemption intents. */
    asset_request_free(background[0]);
    background[0] = NULL;
    asset_requests_service(socket, &write_pending);

    TEST_CHECK(test_cancel_resets == 1U);
    TEST_CHECK(test_active == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_open_count == ASSET_STREAM_ACTIVE_MAX + 1U);
    TEST_CHECK(test_opens[3].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[3].count == 1U);
    TEST_CHECK(test_opens[3].faces[0] == 201U);
    for (size_t i = 1; i < ASSET_FACE_BATCH_MAX; i++) {
        TEST_CHECK(asset_request_get_state(background[i]) == ASSET_REQUEST_PREEMPTED);
    }

    asset_request_t *urgent_b = test_face_start_priority(202);
    TEST_CHECK(asset_request_preemption_needed(urgent_b));
    TEST_CHECK(asset_request_preempt(background[ASSET_FACE_BATCH_MAX], urgent_b, &displace_victim));
    TEST_CHECK(displace_victim);
    asset_request_t *urgent_c = test_face_start_priority(203);
    TEST_CHECK(asset_request_preemption_needed(urgent_c));
    /* A second handoff before the I/O pass must skip the already-marked batch
     * and reserve a distinct physical stream. */
    TEST_CHECK(
        !asset_request_preempt(background[ASSET_FACE_BATCH_MAX + 1U], urgent_c, &displace_victim));
    TEST_CHECK(!displace_victim);
    TEST_CHECK(
        asset_request_preempt(background[2U * ASSET_FACE_BATCH_MAX], urgent_c, &displace_victim));
    TEST_CHECK(displace_victim);
    asset_request_free(background[ASSET_FACE_BATCH_MAX]);
    background[ASSET_FACE_BATCH_MAX] = NULL;
    asset_request_free(background[2U * ASSET_FACE_BATCH_MAX]);
    background[2U * ASSET_FACE_BATCH_MAX] = NULL;
    asset_requests_service(socket, &write_pending);

    TEST_CHECK(test_cancel_resets == 3U);
    TEST_CHECK(test_active == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_open_count == ASSET_STREAM_ACTIVE_MAX + 3U);
    TEST_CHECK(test_opens[4].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[4].count == 1U);
    TEST_CHECK(test_opens[4].faces[0] == 202U);
    TEST_CHECK(test_opens[5].kind == SOCKET_STREAM_FACE_BATCH);
    TEST_CHECK(test_opens[5].count == 1U);
    TEST_CHECK(test_opens[5].faces[0] == 203U);
    for (size_t i = ASSET_FACE_BATCH_MAX + 1U; i < 2U * ASSET_FACE_BATCH_MAX; i++) {
        TEST_CHECK(asset_request_get_state(background[i]) == ASSET_REQUEST_PREEMPTED);
    }
    for (size_t i = 2U * ASSET_FACE_BATCH_MAX + 1U; i < 3U * ASSET_FACE_BATCH_MAX; i++) {
        TEST_CHECK(asset_request_get_state(background[i]) == ASSET_REQUEST_PREEMPTED);
    }
    TEST_CHECK(asset_request_get_state(urgent_a) == ASSET_REQUEST_PENDING);
    TEST_CHECK(asset_request_get_state(urgent_b) == ASSET_REQUEST_PENDING);

    asset_request_t *all[arraysize(background) + 3U];
    memcpy(all, background, sizeof(background));
    all[arraysize(background)] = urgent_a;
    all[arraysize(background) + 1U] = urgent_b;
    all[arraysize(background) + 2U] = urgent_c;
    test_end(all, arraysize(all));
}

int main(void) {
    toolkit_import(logger);
    toolkit_import(packet);
    socket_t socket = {.quic = true};

    test_capability_lifecycle(&socket);
    test_batch_order_fairness_and_dedup(&socket);
    test_partial_and_total_cancellation(&socket);
    test_fallback(&socket);
    test_disconnect_clears_batch_lifecycle(&socket);
    test_member_failure_isolation(&socket);
    test_stream_failure_isolation(&socket);
    test_final_record_requires_fin(&socket);
    test_priority_respects_generic_barrier(&socket);
    test_priority_faces_batch_without_speculative_members(&socket);
    test_progressed_replacement_does_not_displace_victim(&socket);
    test_priority_preemption_reclaims_distinct_batches(&socket);

    toolkit_deinit();
    return 0;
}
