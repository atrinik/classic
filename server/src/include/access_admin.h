/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ATRINIK_ACCESS_ADMIN_H
#define ATRINIK_ACCESS_ADMIN_H
#include <access_tokens.h>
#define ACCESS_ADMIN_REQUEST_MAX 1024
#define ACCESS_ADMIN_RESPONSE_MAX 32768
typedef enum { ACCESS_ADMIN_ISSUE, ACCESS_ADMIN_LIST, ACCESS_ADMIN_HISTORY,
    ACCESS_ADMIN_REVOKE, ACCESS_ADMIN_REMOVE, ACCESS_ADMIN_STATUS,
    ACCESS_ADMIN_RESULT } access_admin_operation_t;
typedef struct {
    access_admin_operation_t operation;
    char request_id[33], target_request_id[33], token_id[33];
    char label[ACCESS_LABEL_BYTES + 1];
    uint64_t expected_revision, revision;
    size_t offset, limit;
    bool has_expiry, has_revision;
    int64_t expires_at;
} access_admin_request_t;
bool access_admin_parse(const char *, size_t, access_admin_request_t *);
/* Synchronous, worker-only; output is caller-owned and may contain a one-time
 * code. Cleanse after sending, including disconnect/error paths. */
bool access_admin_execute(access_store_t *, const char *, size_t,
    access_route_callback_t, void *, char *, size_t, size_t *);
bool access_admin_status_encode(const access_status_t *, bool, char *, size_t, size_t *);
bool access_admin_execute_absent(const access_status_t *, const char *, size_t,
    char *, size_t, size_t *);
#endif
