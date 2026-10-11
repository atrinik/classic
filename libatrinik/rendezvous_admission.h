/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TOOLKIT_RENDEZVOUS_ADMISSION_H
#define TOOLKIT_RENDEZVOUS_ADMISSION_H
#include "curl.h"
#include "rendezvous.h"
#include <atrinik/protocol/rendezvous_admission.h>

#define RENDEZVOUS_ADMISSION_DIFFICULTY ATRINIK_RENDEZVOUS_ADMISSION_DIFFICULTY
#define RENDEZVOUS_ADMISSION_TOKEN_MAX ATRINIK_RENDEZVOUS_ADMISSION_TOKEN_MAX
#define RENDEZVOUS_ADMISSION_PROOF_MAX ATRINIK_RENDEZVOUS_ADMISSION_PROOF_MAX
#define RENDEZVOUS_ADMISSION_TRIALS_MAX ATRINIK_RENDEZVOUS_ADMISSION_TRIALS_MAX
#define RENDEZVOUS_ADMISSION_BUDGET_MS ATRINIK_RENDEZVOUS_ADMISSION_BUDGET_MS
#define RENDEZVOUS_ADMISSION_RETRY_AFTER_MAX 86400U

typedef struct rendezvous_admission_challenge {
    char token[RENDEZVOUS_ADMISSION_TOKEN_MAX + 1U];
    uint64_t expires_at;
} rendezvous_admission_challenge_t;

typedef enum rendezvous_admission_result {
    RENDEZVOUS_ADMISSION_OK,
    RENDEZVOUS_ADMISSION_UNAVAILABLE,
    RENDEZVOUS_ADMISSION_PROTOCOL,
    RENDEZVOUS_ADMISSION_TIMEOUT,
    RENDEZVOUS_ADMISSION_CANCELLED,
    RENDEZVOUS_ADMISSION_RATE_LIMITED
} rendezvous_admission_result_t;

/* Inputs are borrowed until return; outputs are caller-owned and cleared on
 * failure. Calls retain no pointers and independent calls are thread-safe after
 * process-wide curl/datetime initialization. Inputs and outputs must not overlap.
 * Challenges/proofs and ticket hashes are attempt secrets: never log or persist;
 * cleanse caller copies after use. Parsing checks the canonical response, ticket
 * binding, fixed difficulty and time, not the server-only HMAC. */
bool rendezvous_admission_ticket_hash(const char *ticket, char out[65]);
bool rendezvous_admission_url(const char *websocket_url,
                              const char *server_id,
                              char *out,
                              size_t out_size);
bool rendezvous_admission_parse(const char *body,
                                size_t size,
                                const char *ticket_hash,
                                uint64_t now,
                                rendezvous_admission_challenge_t *out);
/* Blocking, bounded worker operations. The optional cancellation callback and
 * context are borrowed and checked at most every 4096 trials. Solving requires
 * <= 2^27 hashes and <= min(original deadline, start+10s, challenge expiry).
 * No retry, fallback, or difficulty negotiation is performed. */
rendezvous_admission_result_t
rendezvous_admission_solve(const rendezvous_admission_challenge_t *challenge,
                           uint64_t deadline_ms,
                           const curl_cancel_t *cancel,
                           char proof[RENDEZVOUS_ADMISSION_PROOF_MAX + 1U]);
/* Optional caller-owned retry_after_seconds is reset on entry. Only HTTP 429
 * may supply a decimal delay in 1..RENDEZVOUS_ADMISSION_RETRY_AFTER_MAX; absent,
 * malformed, duplicate, date-form or excessive Retry-After fields yield zero.
 * Rate limiting is terminal for this attempt; no automatic retry is performed. */
rendezvous_admission_result_t
rendezvous_admission_prepare(const char *websocket_url,
                             const char *server_id,
                             const char *ticket,
                             uint64_t deadline_ms,
                             const curl_cancel_t *cancel,
                             char proof[RENDEZVOUS_ADMISSION_PROOF_MAX + 1U],
                             uint32_t *retry_after_seconds);
/* Returns an owned secret header list, or NULL. The protected grant must bind
 * this ticket; it supplies only headers, never URL/subprotocol credentials.
 * Release every returned list with the cleansing function below. */
struct curl_slist *rendezvous_admission_headers(const char *proof,
                                                const char *ticket,
                                                const rendezvous_access_grant_t *grant);
void rendezvous_admission_headers_free(struct curl_slist *headers);
#endif
