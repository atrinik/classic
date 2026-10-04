/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "rendezvous.h"
#include "string.h"
#include <openssl/crypto.h>

size_t
rendezvous_websocket_protocol_header(char *data, size_t size, size_t count, void *user_data) {
    rendezvous_websocket_protocol_t *protocol = user_data;
    if (size != 0 && count > SIZE_MAX / size) {
        return 0;
    }
    size_t bytes = size * count;
    static const char status_prefix[] = "HTTP/";
    static const char name[] = "Sec-WebSocket-Protocol:";
    if (protocol == NULL || (bytes != 0 && data == NULL)) {
        return 0;
    }
    if (bytes >= sizeof(status_prefix) - 1U &&
        memcmp(data, status_prefix, sizeof(status_prefix) - 1U) == 0) {
        protocol->echoes = 0;
        protocol->invalid = false;
        return bytes;
    }
    if (bytes < sizeof(name) - 1U || strncasecmp(data, name, sizeof(name) - 1U) != 0) {
        return bytes;
    }
    const char *value = data + sizeof(name) - 1U;
    const char *end = data + bytes;
    while (value < end && (*value == ' ' || *value == '\t')) {
        value++;
    }
    while (end > value &&
           (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    protocol->echoes++;
    if ((size_t)(end - value) != sizeof(RENDEZVOUS_ACCESS_SUBPROTOCOL) - 1U ||
        memcmp(value, RENDEZVOUS_ACCESS_SUBPROTOCOL, sizeof(RENDEZVOUS_ACCESS_SUBPROTOCOL) - 1U) !=
            0) {
        protocol->invalid = true;
    }
    return bytes;
}

bool rendezvous_websocket_protocol_valid(const rendezvous_websocket_protocol_t *protocol) {
    return protocol != NULL && protocol->echoes == 1U && !protocol->invalid;
}

void rendezvous_access_grant_clear(rendezvous_access_grant_t *grant) {
    if (grant != NULL)
        OPENSSL_cleanse(grant, sizeof(*grant));
}

bool rendezvous_access_grant_valid(const rendezvous_access_grant_t *grant,
                                   const char *server_id,
                                   uint64_t now) {
    return grant != NULL && string_is_hex_fixed(server_id, 64, true) &&
           string_is_hex_fixed(grant->server_id, 64, true) &&
           CRYPTO_memcmp(server_id, grant->server_id, 64) == 0 &&
           string_is_hex_fixed(grant->generation, 64, true) &&
           string_is_hex_fixed(grant->client_nonce, 64, true) &&
           string_is_hex_fixed(grant->grant, 64, true) && grant->expiry > now &&
           grant->expiry - now <= RENDEZVOUS_GRANT_LIFETIME_MAX;
}

bool rendezvous_server_auth_candidate_consume(rendezvous_server_auth_state_t *state) {
    if (state == NULL || *state != RENDEZVOUS_SERVER_AUTH_AUTHORIZED)
        return false;
    *state = RENDEZVOUS_SERVER_AUTH_CONSUMED;
    return true;
}

bool rendezvous_access_init_render(char *frame,
                                   size_t frame_size,
                                   const rendezvous_access_grant_t *grant) {
    if (frame == NULL || frame_size == 0)
        return false;
    frame[0] = 0;
    if (grant == NULL || !string_is_hex_fixed(grant->grant, 64, true) ||
        !string_is_hex_fixed(grant->client_nonce, 64, true))
        return false;
    int n = snprintf(
        frame,
        frame_size,
        "{\"type\":\"access_init\",\"version\":1,\"grant\":\"%s\",\"client_nonce\":\"%s\"}",
        grant->grant,
        grant->client_nonce);
    if (n <= 0 || (size_t)n >= frame_size || n > 256) {
        OPENSSL_cleanse(frame, frame_size);
        return false;
    }
    return true;
}

bool rendezvous_access_ready_parse(const char *frame) {
    return frame != NULL && strcmp(frame, "{\"type\":\"access_ready\",\"version\":1}") == 0;
}
