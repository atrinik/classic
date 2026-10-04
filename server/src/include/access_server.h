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
uint64_t access_server_admin_submit(const char *, size_t, const char *account);
bool access_server_admin_poll(uint64_t, char *, size_t, size_t *);
void access_server_cancel(uint64_t);
uint64_t access_server_auth_submit(const char code[16]);
bool access_server_auth_poll(uint64_t, access_outcome_t *, access_token_ref_t *);
access_session_state_t access_server_session_check(const access_token_ref_t *);
void access_server_tick(void);
#ifdef ATRINIK_TESTING
void access_server_session_sequence_for_test(const access_session_state_t *, size_t);
#endif
#endif
