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

#ifndef METASERVER_INTERNAL_H
#define METASERVER_INTERNAL_H

#include <toolkit/curl.h>
#include <toolkit/rendezvous.h>

#define METASERVER_RENDEZVOUS_AUTH_JOBS_MAX 64U
#define METASERVER_RENDEZVOUS_RETRY_BASE_MS 5000U
#define METASERVER_RENDEZVOUS_RETRY_MAX_MS 300000U
#define METASERVER_RENDEZVOUS_RETRY_AFTER_MAX_SECONDS 86400U
#define METASERVER_RENDEZVOUS_STABLE_MS 60000U
#define METASERVER_RENDEZVOUS_CONNECT_TIMEOUT_MS 2000L
#define METASERVER_RENDEZVOUS_UPGRADE_TIMEOUT_MS 10000L

typedef struct metaserver_rendezvous_headers {
    rendezvous_websocket_protocol_t protocol;
    uint32_t retry_after_seconds;
    bool has_retry_after;
} metaserver_rendezvous_headers_t;

typedef struct metaserver_rendezvous_auth_job {
    rendezvous_invite_t invite;
    unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE];
    char ticket[RENDEZVOUS_TICKET_HEX_SIZE + 1U];
    uint64_t deadline_ms;
    rendezvous_server_auth_state_t state;
    bool active;
    bool known_invite;
} metaserver_rendezvous_auth_job_t;

typedef enum metaserver_rendezvous_auth_claim {
    METASERVER_RENDEZVOUS_AUTH_CLAIM_OK,
    METASERVER_RENDEZVOUS_AUTH_CLAIM_INVALID,
    METASERVER_RENDEZVOUS_AUTH_CLAIM_DUPLICATE,
    METASERVER_RENDEZVOUS_AUTH_CLAIM_FULL
} metaserver_rendezvous_auth_claim_t;

size_t metaserver_rendezvous_header(char *data, size_t size, size_t count, void *user_data);
bool metaserver_rendezvous_protocol_allows(const metaserver_rendezvous_headers_t *headers,
                                           bool authorization_required);
uint32_t metaserver_rendezvous_retry_delay_ms(uint32_t failures,
                                              uint32_t retry_after_seconds,
                                              uint32_t random_value);
uint32_t metaserver_rendezvous_retry_failures(uint32_t failures, uint64_t connected_ms);
bool metaserver_rendezvous_generation_allows(uint64_t current_generation,
                                             uint64_t requested_generation,
                                             bool shutdown);

void metaserver_rendezvous_auth_clear(metaserver_rendezvous_auth_job_t *job);
void metaserver_rendezvous_auth_expire(metaserver_rendezvous_auth_job_t *jobs,
                                       size_t capacity,
                                       uint64_t now_ms);
metaserver_rendezvous_auth_claim_t
metaserver_rendezvous_auth_claim(metaserver_rendezvous_auth_job_t *jobs,
                                 size_t capacity,
                                 const char *ticket,
                                 uint64_t deadline_ms,
                                 metaserver_rendezvous_auth_job_t **claimed);
metaserver_rendezvous_auth_job_t *
metaserver_rendezvous_auth_find(metaserver_rendezvous_auth_job_t *jobs,
                                size_t capacity,
                                const char *ticket,
                                rendezvous_server_auth_state_t state);

/* Clears the published endpoint. The legacy configuration accepts raw IPs,
 * which are intentionally not a directory-publication opt-in. */
bool metaserver_public_endpoint_from_config(const char *configured_host,
                                            uint16_t configured_port,
                                            char *published_host,
                                            size_t published_host_size,
                                            uint16_t *published_port);

#endif
