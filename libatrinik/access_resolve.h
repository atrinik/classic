/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TOOLKIT_ACCESS_RESOLVE_H
#define TOOLKIT_ACCESS_RESOLVE_H
#include "access_code.h"
#include "curl.h"
#include "rendezvous.h"
typedef struct access_resolved {
    char server_id[65];
    char name[81];
    char hostname[254];
    uint16_t port;
    rendezvous_access_grant_t grant;
} access_resolved_t;
/* Caller owns output (cleared on failure); inputs borrowed, never retained.
 * Independent calls are thread-safe after process-wide curl initialization.
 * Blocks at most the bounded HTTPS attempt; call on a connection worker.
 * Result is one-attempt private state, never a public directory/cache entry.
 * The configured origin is the trusted first-contact routing authority.
 * Existing pins must be checked separately before sending C to the server. */
bool access_resolve(const char *origin,
                    const char code[ACCESS_CODE_LENGTH],
                    access_resolved_t *out);
/* Same ownership as access_resolve; cancellation is borrowed until return.
 * Aborted transfers clear output and do not attempt another endpoint. */
bool access_resolve_cancellable(const char *origin,
                                const char code[ACCESS_CODE_LENGTH],
                                access_resolved_t *out,
                                const curl_cancel_t *cancel);
bool access_resolve_parse(const char *body,
                          size_t size,
                          const char nonce[65],
                          uint64_t now,
                          access_resolved_t *out);
void access_resolved_clear(access_resolved_t *resolved);
#endif
