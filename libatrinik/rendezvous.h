/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TOOLKIT_RENDEZVOUS_H
#define TOOLKIT_RENDEZVOUS_H
#include "toolkit.h"
#define RENDEZVOUS_ACCESS_SUBPROTOCOL "atrinik-access-rendezvous-v1"
#define RENDEZVOUS_SERVER_ID_HEX_SIZE 64U
#define RENDEZVOUS_TICKET_HEX_SIZE 64U
#define RENDEZVOUS_FRAME_MAX 512U
#define RENDEZVOUS_GRANT_LIFETIME_MAX 15U
/* Caller-owned sensitive, single-attempt grant. Never persist or log it.
 * Fields are canonical lowercase hex. Independent grants are thread-safe;
 * an attempt owns its copy and clears it on completion or failure. */
typedef struct rendezvous_access_grant {
    char server_id[65];
    char generation[65];
    char client_nonce[65];
    char grant[65];
    uint64_t expiry;
} rendezvous_access_grant_t;
typedef struct rendezvous_websocket_protocol {
    unsigned int echoes;
    bool invalid;
} rendezvous_websocket_protocol_t;
typedef enum rendezvous_server_auth_state {
    RENDEZVOUS_SERVER_AUTH_NEW,
    RENDEZVOUS_SERVER_AUTH_AUTHORIZED,
    RENDEZVOUS_SERVER_AUTH_DENIED,
    RENDEZVOUS_SERVER_AUTH_CONSUMED
} rendezvous_server_auth_state_t;
void rendezvous_access_grant_clear(rendezvous_access_grant_t *grant);
bool rendezvous_access_grant_valid(const rendezvous_access_grant_t *grant,
                                   const char *server_id, uint64_t now);
size_t rendezvous_websocket_protocol_header(char *data, size_t size, size_t count, void *user_data);
bool rendezvous_websocket_protocol_valid(const rendezvous_websocket_protocol_t *protocol);
bool rendezvous_server_auth_candidate_consume(rendezvous_server_auth_state_t *state);
/* Borrowed inputs; caller-owned output, cleared on failure. */
bool rendezvous_access_init_render(char *frame, size_t frame_size,
                                   const rendezvous_access_grant_t *grant);
bool rendezvous_access_ready_parse(const char *frame);
#endif
