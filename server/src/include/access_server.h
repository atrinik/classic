/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ATRINIK_ACCESS_SERVER_H
#define ATRINIK_ACCESS_SERVER_H
#include <access_tokens.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* Single bounded worker owns blocking store/route/admin work. No socket or
 * player pointer crosses its boundary. Job IDs are never reused in a process. */
bool access_server_init(const char identity_hex[65]);
void access_server_deinit(void);
bool access_server_shutdown(void);
bool access_server_healthy(void);
void access_server_save_failed(void);
uint64_t access_server_root_submit(const char *, size_t);
uint64_t access_server_admin_submit(const char *, size_t, bool permitted);
bool access_server_admin_poll(uint64_t, char *, size_t, size_t *);
bool access_server_admin_poll_permitted(uint64_t, bool, char *, size_t, size_t *);
void access_server_cancel(uint64_t);
uint64_t access_server_auth_submit(const char code[16]);
/* Consume a completed auth job only after current session authority is known.
 * COMMITTED includes a VALID session check; revoked/expired authority is DENIED.
 * BUSY retains the job and leaves outputs untouched, returning false for retry.
 * The caller must cancel a pending job on disconnect or login timeout. */
bool access_server_auth_poll(uint64_t, access_outcome_t *, access_token_ref_t *);
access_session_state_t access_server_session_check(const access_token_ref_t *);
void access_server_tick(void);
#ifdef ATRINIK_TESTING
/* Configure only while the worker is stopped; NULL restores real route IO. */
void access_server_route_for_test(access_route_callback_t);
void access_server_maintenance_for_test(void);
/* Seed completed jobs only while the worker is stopped. */
uint64_t access_server_auth_result_for_test(access_outcome_t, const access_token_ref_t *);
uint64_t access_server_admin_result_for_test(bool, const char *, const char *);
void access_server_session_sequence_for_test(const access_session_state_t *, size_t);
#endif
#endif
