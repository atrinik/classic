/* Copyright (c) 2026 The Atrinik Project. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ATRINIK_ACCESS_TOKENS_H
#define ATRINIK_ACCESS_TOKENS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACCESS_TOKEN_LIMIT 1024
#define ACCESS_PAGE_LIMIT 64
#define ACCESS_HISTORY_LIMIT 16
#define ACCESS_OUTBOX_LIMIT 32
#define ACCESS_RECEIPT_LIMIT 4096
#define ACCESS_TOMBSTONE_LIMIT 4096
#define ACCESS_LABEL_BYTES 128
#define ACCESS_STORE_SCHEMA 1
#define ACCESS_STORE_FILENAME "access-tokens.snapshot"

typedef struct access_store access_store_t;
typedef enum {
    ACCESS_COMMITTED,
    ACCESS_PENDING,
    ACCESS_LOCALLY_REVOKED,
    ACCESS_CONFLICT,
    ACCESS_DENIED,
    ACCESS_UNAVAILABLE,
    ACCESS_SAVE_FAILED,
    ACCESS_INDETERMINATE,
    ACCESS_SECRET_UNAVAILABLE,
    ACCESS_LIMIT,
    ACCESS_INVALID,
    ACCESS_NOT_FOUND,
    ACCESS_ROUTE_COLLISION
} access_outcome_t;
typedef enum {
    ACCESS_TOKEN_PENDING = 1,
    ACCESS_TOKEN_ACTIVE,
    ACCESS_TOKEN_REVOKED,
    ACCESS_TOKEN_EXPIRED
} access_token_state_t;
typedef enum {
    ACCESS_OP_ISSUE = 1,
    ACCESS_OP_REVOKE,
    ACCESS_OP_REMOVE
} access_operation_t;

typedef struct {
    char token_id[33];
    uint64_t revision;
} access_token_ref_t;

typedef struct {
    access_token_ref_t ref;
    char label[ACCESS_LABEL_BYTES + 1];
    int64_t created_at;
    bool has_expiry;
    int64_t expires_at;
    access_token_state_t state;
    bool route_pending;
    int64_t last_admitted_at; /* zero means no admission; valid clocks are > 0 */
    size_t history_count;
    int64_t history[ACCESS_HISTORY_LIMIT]; /* oldest first */
} access_token_info_t;

typedef struct {
    access_outcome_t outcome;
    uint64_t revision;
    access_token_ref_t token;
    bool route_pending;
    char code[17]; /* ONLY initial successful issue; caller must cleanse */
} access_result_t;

typedef struct {
    unsigned schema_version;
    uint8_t server_identity[32];
    bool protected_policy;
    bool integrity_ok;
    bool durability_ok;
    bool fenced;
    uint64_t revision;
    size_t pending_route_sync;
} access_status_t;

typedef struct {
    char request_id[33];
    access_token_ref_t token;
    uint8_t index[32]; /* private route adapter only; never operator output */
    bool has_expiry;
    int64_t expires_at;
    bool revoke;
    int64_t deadline_monotonic_ms; /* transient; zero for caller-owned outbox deadline */
} access_route_t;

/* The route callback must reserve then activate the exact supplied registration,
 * or confirm revocation. It may return COMMITTED only after authenticated remote
 * durable acknowledgement. No callback means pending, never success. Callbacks
 * may permit concurrent mutations; only a still-pending matching token activates.
 * ACCESS_ROUTE_COLLISION is legal ONLY for reserve ownership collision proving
 * this tuple never registered. Other reserve/activate conflicts are ambiguous.
 * Enforce the supplied monotonic deadline across all retry phases (<=30s).
 * Store APIs can perform disk I/O: invoke on the serialized bounded admin/auth
 * worker, never on the simulation thread. Public methods serialize internally.
 */
typedef access_outcome_t (*access_route_callback_t)(void *, const access_route_t *);

/* Linux private directory must already exist, be euid-owned 0700 and contain no
 * symlink path components. Relative paths are anchored at getcwd without
 * following links; embedded dot/dotdot path components are rejected. initialize=true is explicit
 * bootstrap and refuses an existing snapshot. Ordinary open never recreates a missing/corrupt
 * snapshot. The store holds an exclusive directory flock until close. Startup durably revokes
 * interrupted pending issues; it never reconstructs or releases codes.
 */
access_outcome_t access_store_open(access_store_t **out,
                                   const char *directory,
                                   const uint8_t identity[32],
                                   bool protected_policy,
                                   bool initialize);
void access_store_close(access_store_t *store);
/* Same validation and exclusive lock, with no creation, recovery or writes.
 * Caller also holds the outer data-directory state lock to verify absence. */
access_outcome_t access_store_inspect(const char *directory,
                                      const uint8_t identity[32],
                                      bool protected_policy,
                                      access_status_t *status);
int access_state_lock(const char *data_directory);
/* True only for an absent final leaf under an existing trusted parent. */
bool access_store_absent(const char *directory);
void access_state_unlock(int descriptor);
access_status_t access_store_status(access_store_t *store);
access_outcome_t access_store_list(access_store_t *store,
                                   uint64_t revision,
                                   size_t offset,
                                   size_t limit,
                                   access_token_info_t *rows,
                                   size_t *count,
                                   size_t *next_offset);
access_outcome_t access_store_history(access_store_t *store,
                                      const char *token_id,
                                      uint64_t revision,
                                      access_token_info_t *row);
access_result_t access_store_result(access_store_t *store, const char *request_id);
access_result_t access_store_issue(access_store_t *store,
                                   const char *request_id,
                                   uint64_t expected_revision,
                                   const char *label,
                                   bool has_expiry,
                                   int64_t expires_at,
                                   int64_t now,
                                   access_route_callback_t route,
                                   void *context);
access_result_t access_store_revoke(access_store_t *store,
                                    const char *request_id,
                                    uint64_t expected_revision,
                                    const char *token_id,
                                    int64_t now);
/* Remove stages local revoke and keeps the original request receipt pending.
 * Route acknowledgement finishes erasure/tombstone atomically; result recovers
 * completion. At tombstone capacity, identical retry can finish after pruning. */
access_result_t access_store_remove(access_store_t *store,
                                    const char *request_id,
                                    uint64_t expected_revision,
                                    const char *token_id,
                                    int64_t now);
/* Outbox dispatch page <=32; retained per-token pending state <=1024.
 * Items include private I, never C/R/V; do not expose in operator UI. */
access_outcome_t
access_store_outbox(access_store_t *store, access_route_t *rows, size_t capacity, size_t *count);
access_outcome_t
access_store_route_ack(access_store_t *store, const access_route_t *ack, int64_t now);
access_outcome_t access_store_authorize(access_store_t *store,
                                        const char code[16],
                                        const uint8_t identity[32],
                                        int64_t now,
                                        access_token_ref_t *ref);
typedef enum {
    ACCESS_SESSION_VALID,
    ACCESS_SESSION_DENIED,
    ACCESS_SESSION_BUSY
} access_session_state_t;
/* Nonblocking game-thread query: BUSY defers dispatch, never grants access. */
access_session_state_t
access_store_session_check(access_store_t *store, const access_token_ref_t *ref, int64_t now);
/* Serialized worker: durable expiry latch before publishing the observed expiry
 * tick. A tick-only wall-clock comparison is not durable rollback protection. */
access_outcome_t access_store_expire(access_store_t *store, int64_t now);
bool access_store_session_valid(access_store_t *store, const access_token_ref_t *ref, int64_t now);
/* Fences further mutations/admissions before checked fsync. Sessions must drain
 * through ordinary checked player saving. Close/reopen starts a new authority. */
access_outcome_t access_store_flush_for_shutdown(access_store_t *store);
void access_result_cleanse(access_result_t *result);
const char *access_outcome_name(access_outcome_t outcome);

/* Root-owned regular no-symlink file (0600 for root, or 0440/0640
 * with effective service group), <= 16 KiB; canonical account
 * names one per LF line, no comments/blank lines/duplicates. Missing or invalid
 * file denies. Exact canonical account identity, never character/display/OP.
 * Account rename requires an explicit root-managed allowlist update.
 */
typedef enum {
    ACCESS_OPERATOR_UNAVAILABLE,
    ACCESS_OPERATOR_UNLISTED,
    ACCESS_OPERATOR_LISTED
} access_operator_lookup_t;
/* Distinguishes a valid unlisted identity from an unavailable trust anchor.
 * Public registration must reject unavailable and listed results. An empty,
 * securely owned file is valid and lists no identities. */
access_operator_lookup_t access_operator_lookup(const char *allowlist_path,
                                                const char *canonical_account);
bool access_operator_allowed(const char *allowlist_path, const char *canonical_account);

#endif
