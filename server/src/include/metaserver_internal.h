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
#include <server_clock.h>
#include <curl/curl.h>

#define METASERVER_RENDEZVOUS_AUTH_JOBS_MAX 64U
#define METASERVER_RENDEZVOUS_RETRY_BASE_MS 5000U
#define METASERVER_RENDEZVOUS_RETRY_MAX_MS 3600000U
#define METASERVER_RENDEZVOUS_RETRY_AFTER_MAX_SECONDS 86400U
#define METASERVER_RENDEZVOUS_STABLE_MS 60000U
#define METASERVER_RENDEZVOUS_HEARTBEAT_MS 300000U
#define METASERVER_RENDEZVOUS_HEARTBEAT_RETRY_MS 20U
#define METASERVER_RENDEZVOUS_CONNECT_TIMEOUT_MS 2000L
#define METASERVER_RENDEZVOUS_UPGRADE_TIMEOUT_MS 10000L

#define METASERVER_PUBLISH_DEBOUNCE_SECONDS 10U
#define METASERVER_ATTEMPT_RATE_CAPACITY 2U
#define METASERVER_ATTEMPT_RATE_REFILL_SECONDS 1920U
#define METASERVER_PUBLISH_RETRY_BASE_MS 60000U
#define METASERVER_PUBLISH_RETRY_MAX_MS 3600000U
#define METASERVER_PUBLISH_RETRY_AFTER_MAX_SECONDS 86400U
#define METASERVER_PUBLISH_TIMEOUT_MS 30000L
#define METASERVER_PUBLISH_HEARTBEAT_DEFAULT_SECONDS 9000U
#define METASERVER_PUBLISH_HEARTBEAT_MIN_SECONDS 60U
#define METASERVER_PUBLISH_HEARTBEAT_MAX_SECONDS 10800U

typedef struct metaserver_attempt_budget {
    server_monotonic_t refill_deadline;
    uint32_t tokens;
} metaserver_attempt_budget_t;

typedef struct metaserver_publish_cadence {
    server_monotonic_t dirty_deadline;
    server_monotonic_t retry_deadline;
    server_monotonic_t heartbeat_deadline;
    metaserver_attempt_budget_t rate_budget;
    uint32_t failures;
    bool dirty;
    bool published;
    bool suspended;
    bool replay_recovered;
} metaserver_publish_cadence_t;

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

typedef enum metaserver_publish_failure_action {
    METASERVER_PUBLISH_FAILURE_REPLAY,
    METASERVER_PUBLISH_FAILURE_RETRY,
    METASERVER_PUBLISH_FAILURE_SUSPEND
} metaserver_publish_failure_action_t;

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
bool metaserver_rendezvous_upgrade_retryable(CURLcode result, long http_code, bool protocol_valid);
void metaserver_attempt_budget_init(metaserver_attempt_budget_t *budget, server_monotonic_t now);
bool metaserver_attempt_budget_consume(metaserver_attempt_budget_t *budget,
                                       server_monotonic_t now,
                                       uint32_t *wait_ms);
/** Restore the full reconnect allowance after a stable rendezvous session. */
void metaserver_attempt_budget_reset(metaserver_attempt_budget_t *budget, server_monotonic_t now);

uint32_t metaserver_publish_heartbeat_delay_seconds(uint32_t heartbeat_seconds,
                                                    uint32_t random_value);
uint32_t metaserver_publish_retry_delay_ms(uint32_t failures,
                                           uint32_t retry_after_seconds,
                                           uint32_t random_value);
bool metaserver_publish_retry_after(const char *headers,
                                    size_t headers_size,
                                    uint32_t *retry_after_seconds);
bool metaserver_publish_response_retryable(curl_state_t state, int http_code);
metaserver_publish_failure_action_t metaserver_publish_failure_action(curl_state_t state,
                                                                      int http_code);
void metaserver_publish_cadence_init(metaserver_publish_cadence_t *cadence, server_monotonic_t now);
void metaserver_publish_cadence_changed(metaserver_publish_cadence_t *cadence,
                                        server_monotonic_t now,
                                        bool actual_change);
bool metaserver_publish_cadence_needs_snapshot(metaserver_publish_cadence_t *cadence,
                                               server_monotonic_t now);
bool metaserver_publish_cadence_due(metaserver_publish_cadence_t *cadence,
                                    server_monotonic_t now,
                                    bool snapshot_changed);
bool metaserver_publish_cadence_attempted(metaserver_publish_cadence_t *cadence,
                                          server_monotonic_t now);
void metaserver_publish_cadence_succeeded(metaserver_publish_cadence_t *cadence,
                                          server_monotonic_t now,
                                          bool heartbeat_enabled,
                                          uint32_t heartbeat_seconds,
                                          uint32_t random_value);
void metaserver_publish_cadence_failed(metaserver_publish_cadence_t *cadence,
                                       server_monotonic_t now,
                                       uint32_t retry_after_seconds,
                                       uint32_t random_value);
void metaserver_publish_cadence_suspend(metaserver_publish_cadence_t *cadence);
bool metaserver_publish_cadence_recover_replay(metaserver_publish_cadence_t *cadence);

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
