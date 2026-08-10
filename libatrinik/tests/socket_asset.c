#include <toolkit/packet.h>
#include <toolkit/socket.h>

static void require_at(bool condition, int line) {
    if (!condition) {
        fprintf(stderr, "requirement failed at line %d\n", line);
        abort();
    }
}

#define REQUIRE(condition) require_at((condition), __LINE__)

static void test_face_batch_constants(void) {
    REQUIRE(ASSET_TRANSPORT_CAP_GENERIC == 0x01U);
    REQUIRE(ASSET_TRANSPORT_CAP_FACE_BATCH == 0x02U);
    REQUIRE(ASSET_TRANSPORT_CAP_ALL == 0x03U);
    REQUIRE(ASSET_TRANSPORT_FACE_BATCH_VERSION == 1074);
    REQUIRE(ASSET_FACE_BATCH_MAX == 8U);
    REQUIRE(ASSET_FACE_BATCH_MAX_SIZE == 400000U);
    REQUIRE(SOCKET_FACE_BATCH_REQUEST_MAX_SIZE == 17U);
    REQUIRE(SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE == 39U);
}

static void test_face_batch_request_round_trip(void) {
    static const uint16_t faces[] = {1, UINT16_MAX, 42};
    static const uint8_t expected[] = {3, 0, 1, 0xff, 0xff, 0, 42};
    packet_struct *packet = packet_new(0, 0, 0);
    REQUIRE(socket_face_batch_request_append(packet, faces, arraysize(faces)));
    REQUIRE(packet->len == sizeof(expected));
    REQUIRE(memcmp(packet->data, expected, sizeof(expected)) == 0);

    socket_face_batch_request_t request = {0};
    REQUIRE(socket_face_batch_request_parse(packet->data, packet->len, 0, &request));
    REQUIRE(request.count == arraysize(faces));
    REQUIRE(memcmp(request.faces, faces, sizeof(faces)) == 0);
    for (size_t i = arraysize(faces); i < ASSET_FACE_BATCH_MAX; i++) {
        REQUIRE(request.faces[i] == 0);
    }
    packet_free(packet);

    static const uint16_t maximum_faces[ASSET_FACE_BATCH_MAX] = {1, 2, 3, 4, 5, 6, 7, 8};
    packet = packet_new(0, 0, 0);
    REQUIRE(socket_face_batch_request_append(packet, maximum_faces, arraysize(maximum_faces)));
    REQUIRE(packet->len == SOCKET_FACE_BATCH_REQUEST_MAX_SIZE);
    REQUIRE(socket_face_batch_request_parse(packet->data, packet->len, 0, &request));
    REQUIRE(request.count == ASSET_FACE_BATCH_MAX);
    REQUIRE(memcmp(request.faces, maximum_faces, sizeof(maximum_faces)) == 0);
    packet_free(packet);
}

static void test_face_batch_request_rejects_invalid_encoding(void) {
    static const uint16_t valid[] = {12, 34, 56};
    static const uint16_t duplicate[] = {12, 34, 12};
    static const uint16_t zero[] = {12, 0, 56};
    static const uint16_t too_many[ASSET_FACE_BATCH_MAX + 1U] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    packet_struct *packet = packet_new(0, 0, 0);
    packet_writer_write_uint8(packet, 0xa5);
    size_t unchanged_len = packet->len;
    REQUIRE(!socket_face_batch_request_append(packet, NULL, 1));
    REQUIRE(!socket_face_batch_request_append(packet, valid, 0));
    REQUIRE(!socket_face_batch_request_append(packet, duplicate, arraysize(duplicate)));
    REQUIRE(!socket_face_batch_request_append(packet, zero, arraysize(zero)));
    REQUIRE(!socket_face_batch_request_append(packet, too_many, arraysize(too_many)));
    REQUIRE(packet->len == unchanged_len);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint8(packet, 0xa5);
    packet_writer_set_limit(packet, 4);
    REQUIRE(!socket_face_batch_request_append(packet, valid, arraysize(valid)));
    REQUIRE(packet->len == 1);
    REQUIRE(packet_writer_error(packet) == PACKET_ERROR_LIMIT_EXCEEDED);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    REQUIRE(socket_face_batch_request_append(packet, valid, arraysize(valid)));
    socket_face_batch_request_t request;
    memset(&request, 0xa5, sizeof(request));
    const socket_face_batch_request_t unchanged = request;
    for (size_t truncated = 0; truncated < packet->len; truncated++) {
        REQUIRE(!socket_face_batch_request_parse(packet->data, truncated, 0, &request));
        REQUIRE(memcmp(&request, &unchanged, sizeof(request)) == 0);
    }

    packet_writer_write_uint8(packet, 0);
    REQUIRE(!socket_face_batch_request_parse(packet->data, packet->len, 0, &request));
    REQUIRE(memcmp(&request, &unchanged, sizeof(request)) == 0);
    packet_free(packet);

    static const uint8_t empty[] = {0};
    static const uint8_t oversized[] = {ASSET_FACE_BATCH_MAX + 1U};
    static const uint8_t zero_face[] = {1, 0, 0};
    static const uint8_t duplicate_face[] = {2, 0, 1, 0, 1};
    REQUIRE(!socket_face_batch_request_parse(empty, sizeof(empty), 0, &request));
    REQUIRE(!socket_face_batch_request_parse(oversized, sizeof(oversized), 0, &request));
    REQUIRE(!socket_face_batch_request_parse(zero_face, sizeof(zero_face), 0, &request));
    REQUIRE(!socket_face_batch_request_parse(duplicate_face, sizeof(duplicate_face), 0, &request));
    REQUIRE(!socket_face_batch_request_parse(NULL, sizeof(empty), 0, &request));
    REQUIRE(!socket_face_batch_request_parse(empty, sizeof(empty), 0, NULL));
    REQUIRE(memcmp(&request, &unchanged, sizeof(request)) == 0);
}

static void test_face_batch_response_round_trip(void) {
    uint8_t digest[ASSET_DIGEST_SIZE];
    memset(digest, 0x5a, sizeof(digest));
    packet_struct *packet = packet_new(0, 0, 0);
    REQUIRE(socket_face_batch_response_append(packet, UINT16_MAX, ASSET_STATUS_OK, 50000, digest));
    REQUIRE(packet->len == SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE);
    REQUIRE(packet->data[0] == 0xff && packet->data[1] == 0xff);
    REQUIRE(packet->data[2] == ASSET_STATUS_OK);
    REQUIRE(packet->data[3] == 0 && packet->data[4] == 0 && packet->data[5] == 0xc3 &&
            packet->data[6] == 0x50);
    REQUIRE(memcmp(packet->data + 7, digest, sizeof(digest)) == 0);

    socket_face_batch_response_t response = {0};
    REQUIRE(socket_face_batch_response_parse(packet->data, packet->len, 0, &response));
    REQUIRE(response.face == UINT16_MAX);
    REQUIRE(response.status == ASSET_STATUS_OK);
    REQUIRE(response.body_size == ASSET_FACE_MAX_SIZE);
    REQUIRE(memcmp(response.digest, digest, sizeof(digest)) == 0);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    REQUIRE(socket_face_batch_response_append(packet, 1, ASSET_STATUS_NOT_FOUND, 0, NULL));
    REQUIRE(socket_face_batch_response_parse(packet->data, packet->len, 0, &response));
    REQUIRE(response.face == 1);
    REQUIRE(response.status == ASSET_STATUS_NOT_FOUND);
    REQUIRE(response.body_size == 0);
    static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
    REQUIRE(memcmp(response.digest, empty_digest, sizeof(empty_digest)) == 0);
    packet_free(packet);
}

static void test_face_batch_response_rejects_invalid_encoding(void) {
    uint8_t digest[ASSET_DIGEST_SIZE];
    memset(digest, 0x5a, sizeof(digest));
    packet_struct *packet = packet_new(0, 0, 0);
    packet_writer_write_uint8(packet, 0xa5);
    size_t unchanged_len = packet->len;
    REQUIRE(!socket_face_batch_response_append(packet, 0, ASSET_STATUS_OK, 1, digest));
    REQUIRE(!socket_face_batch_response_append(packet, 1, ASSET_STATUS_OK, 0, digest));
    REQUIRE(!socket_face_batch_response_append(packet,
                                               1,
                                               ASSET_STATUS_OK,
                                               ASSET_FACE_MAX_SIZE + 1U,
                                               digest));
    REQUIRE(!socket_face_batch_response_append(packet, 1, ASSET_STATUS_OK, 1, NULL));
    REQUIRE(!socket_face_batch_response_append(packet, 1, ASSET_STATUS_NOT_FOUND, 1, NULL));
    REQUIRE(!socket_face_batch_response_append(packet, 1, ASSET_STATUS_NOT_FOUND, 0, digest));
    REQUIRE(!socket_face_batch_response_append(packet, 1, ASSET_STATUS_NOT_MODIFIED, 0, NULL));
    REQUIRE(packet->len == unchanged_len);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint8(packet, 0xa5);
    packet_writer_set_limit(packet, 10);
    REQUIRE(!socket_face_batch_response_append(packet, 1, ASSET_STATUS_OK, 1, digest));
    REQUIRE(packet->len == 1);
    REQUIRE(packet_writer_error(packet) == PACKET_ERROR_LIMIT_EXCEEDED);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    REQUIRE(socket_face_batch_response_append(packet, 1, ASSET_STATUS_OK, 1, digest));
    socket_face_batch_response_t response;
    memset(&response, 0xa5, sizeof(response));
    const socket_face_batch_response_t unchanged = response;
    for (size_t truncated = 0; truncated < packet->len; truncated++) {
        REQUIRE(!socket_face_batch_response_parse(packet->data, truncated, 0, &response));
        REQUIRE(memcmp(&response, &unchanged, sizeof(response)) == 0);
    }

    packet_writer_write_uint8(packet, 0);
    REQUIRE(!socket_face_batch_response_parse(packet->data, packet->len, 0, &response));
    REQUIRE(memcmp(&response, &unchanged, sizeof(response)) == 0);
    packet->len--;

    packet->data[0] = 0;
    packet->data[1] = 0;
    REQUIRE(!socket_face_batch_response_parse(packet->data, packet->len, 0, &response));
    packet->data[1] = 1;
    packet->data[2] = 0xff;
    REQUIRE(!socket_face_batch_response_parse(packet->data, packet->len, 0, &response));
    packet->data[2] = ASSET_STATUS_OK;
    memset(packet->data + 3, 0, 4);
    REQUIRE(!socket_face_batch_response_parse(packet->data, packet->len, 0, &response));
    packet->data[2] = ASSET_STATUS_NOT_FOUND;
    packet->data[6] = 1;
    memset(packet->data + 7, 0, ASSET_DIGEST_SIZE);
    REQUIRE(!socket_face_batch_response_parse(packet->data, packet->len, 0, &response));
    packet->data[2] = ASSET_STATUS_OK;
    packet->data[3] = 0;
    packet->data[4] = 0;
    packet->data[5] = 0xc3;
    packet->data[6] = 0x51;
    REQUIRE(!socket_face_batch_response_parse(packet->data, packet->len, 0, &response));
    packet->data[2] = ASSET_STATUS_NOT_FOUND;
    memset(packet->data + 3, 0, 4);
    packet->data[7] = 1;
    REQUIRE(!socket_face_batch_response_parse(packet->data, packet->len, 0, &response));
    memset(packet->data + 7, 0, ASSET_DIGEST_SIZE);
    REQUIRE(socket_face_batch_response_parse(packet->data, packet->len, 0, &response));
    REQUIRE(response.status == ASSET_STATUS_NOT_FOUND);
    packet_free(packet);

    REQUIRE(!socket_face_batch_response_parse(NULL,
                                              SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE,
                                              0,
                                              &response));
    REQUIRE(!socket_face_batch_response_parse(NULL, 0, 0, NULL));
}

static void test_face_batch_stream_preface(void) {
    uint8_t preface[SOCKET_STREAM_PREFACE_SIZE];
    socket_stream_preface_encode(preface, SOCKET_STREAM_FACE_BATCH);
    socket_stream_kind_t kind = SOCKET_STREAM_UNKNOWN;
    REQUIRE(socket_stream_preface_decode(preface, sizeof(preface), &kind));
    REQUIRE(kind == SOCKET_STREAM_FACE_BATCH);

    preface[4] = SOCKET_STREAM_FACE_BATCH + 1U;
    kind = SOCKET_STREAM_ASSET;
    REQUIRE(!socket_stream_preface_decode(preface, sizeof(preface), &kind));
    REQUIRE(kind == SOCKET_STREAM_ASSET);
}

static void test_stream_pending_poll_is_safe_for_non_quic(void) {
    socket_t *connection = socket_create("127.0.0.1", 9, SOCKET_ROLE_CLIENT, false);
    REQUIRE(connection != NULL);
    REQUIRE(socket_stream_poll_pending(connection) == 0);
    REQUIRE(socket_stream_accept(connection, SOCKET_STREAM_FACE_BATCH) == NULL);
    socket_destroy(connection);
}

int main(void) {
    toolkit_import(socket);
    toolkit_import(packet);
    test_face_batch_constants();
    test_face_batch_request_round_trip();
    test_face_batch_request_rejects_invalid_encoding();
    test_face_batch_response_round_trip();
    test_face_batch_response_rejects_invalid_encoding();
    test_face_batch_stream_preface();
    test_stream_pending_poll_is_safe_for_non_quic();
    toolkit_deinit();
    return 0;
}
