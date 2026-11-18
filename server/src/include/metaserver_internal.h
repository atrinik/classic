/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#ifndef METASERVER_INTERNAL_H
#define METASERVER_INTERNAL_H

#include <toolkit/curl.h>
#include <toolkit/socket.h>
#include <toolkit/rendezvous.h>
#include <toolkit/metaserver_publisher.h>
#include <server_clock.h>
#include <access_tokens.h>
#include <curl/curl.h>

#define METASERVER_RENDEZVOUS_AUTH_JOBS_MAX 64U
#define METASERVER_RENDEZVOUS_RETRY_BASE_MS 5000U
#define METASERVER_RENDEZVOUS_RETRY_MAX_MS 3600000U
#define METASERVER_RENDEZVOUS_RETRY_AFTER_MAX_SECONDS 86400U
#define METASERVER_RENDEZVOUS_STABLE_MS 60000U
/* Control PING cadence; the first PING is sent as soon as the control opens. */
#define METASERVER_RENDEZVOUS_PING_INTERVAL_MS 30000U
/* Close the control when no frame follows a PING for this long. */
#define METASERVER_RENDEZVOUS_PONG_TIMEOUT_MS 20000U
/* Minimum spacing of the monitored "Sent rendezvous control heartbeat" line. */
#define METASERVER_RENDEZVOUS_HEARTBEAT_MS 300000U
/* Retry delay for a PING that libcurl could not write yet (CURLE_AGAIN). */
#define METASERVER_RENDEZVOUS_PING_RETRY_MS 20U
#define METASERVER_RENDEZVOUS_CONNECT_TIMEOUT_MS 2000L

/* At most one PING is outstanding when its deadline expires. */
_Static_assert(METASERVER_RENDEZVOUS_PONG_TIMEOUT_MS < METASERVER_RENDEZVOUS_PING_INTERVAL_MS,
               "pong timeout must be shorter than the ping interval");
/* A dead control is detected within a minute. */
_Static_assert(METASERVER_RENDEZVOUS_PING_INTERVAL_MS + METASERVER_RENDEZVOUS_PONG_TIMEOUT_MS <=
                   60000U,
               "dead-control detection must stay within a minute");
/* The heartbeat line falls on a PING, well inside the 15-minute stale alert. */
_Static_assert(METASERVER_RENDEZVOUS_HEARTBEAT_MS % METASERVER_RENDEZVOUS_PING_INTERVAL_MS == 0U &&
                   METASERVER_RENDEZVOUS_HEARTBEAT_MS < 900000U,
               "heartbeat line cadence must match the ping cadence and monitoring");
#define METASERVER_RENDEZVOUS_UPGRADE_TIMEOUT_MS 10000L

#define METASERVER_PUBLISH_DEBOUNCE_SECONDS 10U
#define METASERVER_ATTEMPT_RATE_CAPACITY 2U
#define METASERVER_ATTEMPT_RATE_REFILL_SECONDS 1920U
#define METASERVER_PUBLISH_RETRY_BASE_MS 60000U
#define METASERVER_PUBLISH_RETRY_MAX_MS 3600000U
#define METASERVER_PUBLISH_RETRY_AFTER_MAX_SECONDS 86400U
#define METASERVER_PUBLISH_TIMEOUT_MS 30000L
#define METASERVER_PUBLISH_RESPONSE_BODY_MAX 1024U
#define METASERVER_PUBLISH_ERROR_CODE_MAX 64U
#define METASERVER_PUBLISH_HEARTBEAT_DEFAULT_SECONDS 9000U
#define METASERVER_PUBLISH_HEARTBEAT_MIN_SECONDS 60U
#define METASERVER_PUBLISH_HEARTBEAT_MAX_SECONDS 10800U

/* Sanitized signed-presence state; private maintenance carries no activity/address. */
typedef struct metaserver_public_snapshot {
    char name[MAX_BUF];
    char description[MAX_BUF];
    char hostname[MAX_BUF];
    uint32_t players_count;
    uint16_t port;
    bool is_public;
    bool access_required;
} metaserver_public_snapshot_t;

void metaserver_public_snapshot(metaserver_public_snapshot_t *snapshot);
bool metaserver_public_snapshot_body(const metaserver_public_snapshot_t *snapshot,
                                      const char *server_id,
                                      const char *certificate,
                                      char body[METASERVER_PUBLISH_BODY_MAX + 1U],
                                      size_t *body_size);

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

void metaserver_public_snapshot_succeeded(metaserver_publish_cadence_t *cadence,
                                           const metaserver_public_snapshot_t *snapshot,
                                           server_monotonic_t now,
                                           uint32_t heartbeat_seconds,
                                           uint32_t random_value);

typedef struct metaserver_rendezvous_headers {
    rendezvous_websocket_protocol_t protocol;
    uint32_t retry_after_seconds;
    bool has_retry_after;
} metaserver_rendezvous_headers_t;

typedef struct metaserver_rendezvous_auth_job {
    char ticket[RENDEZVOUS_TICKET_HEX_SIZE + 1U];
    uint64_t deadline_ms;
    rendezvous_server_auth_state_t state;
    bool active;
} metaserver_rendezvous_auth_job_t;

typedef enum metaserver_rendezvous_auth_claim {
    METASERVER_RENDEZVOUS_AUTH_CLAIM_OK,
    METASERVER_RENDEZVOUS_AUTH_CLAIM_INVALID,
    METASERVER_RENDEZVOUS_AUTH_CLAIM_DUPLICATE,
    METASERVER_RENDEZVOUS_AUTH_CLAIM_FULL
} metaserver_rendezvous_auth_claim_t;

/**
 * Liveness of one connected rendezvous control, in datetime_monotonic_ms()
 * milliseconds. Owned by the rendezvous thread for that connection only.
 *
 * A PING is outstanding from the first attempt to write it until any frame
 * arrives. The pong deadline is enforced only after this connection has
 * received a PONG, so a peer or libcurl that never surfaces PONGs keeps the
 * control open as before instead of reconnecting on every PING.
 */
typedef struct metaserver_rendezvous_liveness {
    uint64_t next_ping_ms;
    uint64_t next_heartbeat_log_ms;
    uint64_t ping_sent_ms; ///< First attempt of the oldest unanswered PING.
    bool ping_outstanding;
    bool pong_seen;
    bool unanswered_reported;
} metaserver_rendezvous_liveness_t;

typedef enum metaserver_rendezvous_liveness_result {
    METASERVER_RENDEZVOUS_LIVENESS_OK,
    /** A PONG-capable control missed its deadline; close it and retry. */
    METASERVER_RENDEZVOUS_LIVENESS_EXPIRED,
    /** First deadline lapse without any PONG on this connection; not enforced. */
    METASERVER_RENDEZVOUS_LIVENESS_UNANSWERED
} metaserver_rendezvous_liveness_result_t;

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
/** Start tracking a control that connected at `now_ms`; its first PING is due at once. */
void metaserver_rendezvous_liveness_init(metaserver_rendezvous_liveness_t *liveness,
                                         uint64_t now_ms);
bool metaserver_rendezvous_liveness_ping_due(const metaserver_rendezvous_liveness_t *liveness,
                                             uint64_t now_ms);
/**
 * Record a PING written at `now_ms` and schedule the next one. Returns whether
 * the monitored heartbeat line is due, at most once per HEARTBEAT_MS.
 */
bool metaserver_rendezvous_liveness_ping_sent(metaserver_rendezvous_liveness_t *liveness,
                                              uint64_t now_ms);
/** Retry a PING that could not be written yet (CURLE_AGAIN); its deadline still runs. */
void metaserver_rendezvous_liveness_ping_blocked(metaserver_rendezvous_liveness_t *liveness,
                                                 uint64_t now_ms);
/** Any received frame answers the outstanding PING; `pongs` also proves PONG support. */
void metaserver_rendezvous_liveness_received(metaserver_rendezvous_liveness_t *liveness,
                                             uint32_t pongs);
/**
 * Evaluate the pong deadline after receive has been drained. `waited_ms`
 * receives the age of the unanswered PING when the result is not OK.
 */
metaserver_rendezvous_liveness_result_t
metaserver_rendezvous_liveness_poll(metaserver_rendezvous_liveness_t *liveness,
                                    uint64_t now_ms,
                                    uint64_t *waited_ms);
/**
 * Apply one socket_websocket_receive_ex() outcome. Counted PONGs and text
 * (PARTIAL or MESSAGE) answer the outstanding PING. The deadline is judged
 * only on EMPTY, after every arrived frame has been drained, so a late wakeup
 * cannot miss a buffered PONG; other states return OK.
 */
metaserver_rendezvous_liveness_result_t
metaserver_rendezvous_liveness_after_receive(metaserver_rendezvous_liveness_t *liveness,
                                             socket_websocket_receive_state_t state,
                                             uint32_t pongs,
                                             uint64_t now_ms,
                                             uint64_t *waited_ms);
void metaserver_attempt_budget_init(metaserver_attempt_budget_t *budget, server_monotonic_t now);
bool metaserver_attempt_budget_consume(metaserver_attempt_budget_t *budget,
                                       server_monotonic_t now,
                                       uint32_t *wait_ms);
/** Restore the full reconnect allowance after a stable rendezvous session. */
void metaserver_attempt_budget_reset(metaserver_attempt_budget_t *budget, server_monotonic_t now);
/**
 * Consume like metaserver_attempt_budget_consume(). `window` receives the
 * budget window of the consumed token, for metaserver_attempt_budget_refund().
 */
bool metaserver_attempt_budget_consume_refundable(metaserver_attempt_budget_t *budget,
                                                  server_monotonic_t now,
                                                  uint32_t *wait_ms,
                                                  server_monotonic_t *window);
/**
 * Return one token taken by metaserver_attempt_budget_consume_refundable() in
 * `window`, exactly undoing that consume. Call at most once per consume: a
 * window that held several consumes cannot tell them apart. Nothing is returned
 * once a refill, reset or init has started another window, or when the budget
 * is already full. Returns whether a token was returned.
 */
bool metaserver_attempt_budget_refund(metaserver_attempt_budget_t *budget,
                                      server_monotonic_t window);
/**
 * Whether a rendezvous upgrade produced no HTTP response at all: the transfer
 * failed and no status was received (a DNS, connect or TLS failure, or a
 * request that drew no response before it failed or timed out). Any HTTP
 * status, including 101, 429 and 503, is a response.
 */
bool metaserver_rendezvous_upgrade_unreachable(CURLcode result, long http_code);
/**
 * Settle the budget token of a finished upgrade attempt that did not connect:
 * an attempt without any HTTP response returns its token from `window`; any
 * other attempt keeps it spent. The retry backoff is unaffected either way.
 * Returns whether a token was returned.
 */
bool metaserver_rendezvous_attempt_refund(metaserver_attempt_budget_t *budget,
                                          server_monotonic_t window,
                                          CURLcode result,
                                          long http_code);

uint32_t metaserver_publish_heartbeat_delay_seconds(uint32_t heartbeat_seconds,
                                                    uint32_t random_value);
uint32_t metaserver_publish_retry_delay_ms(uint32_t failures,
                                           uint32_t retry_after_seconds,
                                           uint32_t random_value);
bool metaserver_publish_retry_after(const char *headers,
                                    size_t headers_size,
                                    uint32_t *retry_after_seconds);
/** Parse the bounded canonical error envelope without retaining its body. */
bool metaserver_publish_error_code(const char *body,
                                   size_t body_size,
                                   char *error_code,
                                   size_t error_code_size);
bool metaserver_publish_response_retryable(curl_state_t state, int http_code);
metaserver_publish_failure_action_t metaserver_publish_failure_action(curl_state_t state,
                                                                      int http_code);
void metaserver_publish_cadence_init(metaserver_publish_cadence_t *cadence, server_monotonic_t now);
void metaserver_publish_cadence_activity(metaserver_publish_cadence_t *cadence,
                                         server_monotonic_t now,
                                         bool is_public,
                                         bool previously_public);
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

/* Strict bounded acknowledgment parser; reservation changes only on success. */
access_outcome_t metaserver_access_response_parse(const char *body,
                                                  size_t body_size,
                                                  const char *request_id,
                                                  uint64_t token_revision,
                                                  const char *operation,
                                                  char reservation[33],
                                                  uint64_t now);
/* Serialized access worker only; bounded remote route mutation. Context borrows
 * an optional curl_cancel_t through return. Cancellation keeps remote outcome
 * ambiguous (ACCESS_PENDING), retaining durable receipt/outbox recovery. */
access_outcome_t metaserver_access_route(void *context, const access_route_t *route);

#endif
