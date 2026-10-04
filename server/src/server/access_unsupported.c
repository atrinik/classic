/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <initialization.h>
#include <access_server.h>

/* This backend has no store, worker, route synchronization or administration.
 * Its only supported mode is an ordinary open server. The caller also rejects
 * these settings before listeners start; repeat the check at initialization.
 * No store/lock symbols are provided: accidentally using one must fail to link.
 */
static bool failed;

bool access_server_init(const char identity_hex[65]) {
    (void)identity_hex;
    failed = settings.datapath_fd >= 0 || settings.access_required || settings.access_initialize ||
             settings.access_store[0] != '\0';
    return !failed;
}

void access_server_deinit(void) {}
bool access_server_shutdown(void) {
    return !failed;
}
bool access_server_healthy(void) {
    return !failed;
}
void access_server_save_failed(void) {
    failed = true;
}
void access_server_tick(void) {}

uint64_t access_server_root_submit(const char *data, size_t length) {
    (void)data;
    (void)length;
    return 0;
}

uint64_t access_server_admin_submit(const char *data, size_t length, bool permitted) {
    (void)data;
    (void)length;
    (void)permitted;
    return 0;
}

bool access_server_admin_poll(uint64_t id, char *out, size_t capacity, size_t *length) {
    (void)id;
    if (out != NULL && capacity != 0)
        out[0] = '\0';
    if (length != NULL)
        *length = 0;
    return false;
}

bool access_server_admin_poll_permitted(
    uint64_t id, bool permitted, char *out, size_t capacity, size_t *length) {
    (void)permitted;
    return access_server_admin_poll(id, out, capacity, length);
}

void access_server_cancel(uint64_t id) {
    (void)id;
}

uint64_t access_server_auth_submit(const char code[16]) {
    (void)code;
    return 0;
}

bool access_server_auth_poll(uint64_t id, access_outcome_t *out, access_token_ref_t *ref) {
    (void)id;
    if (out != NULL)
        *out = ACCESS_UNAVAILABLE;
    if (ref != NULL)
        memset(ref, 0, sizeof(*ref));
    return false;
}

access_session_state_t access_server_session_check(const access_token_ref_t *ref) {
    (void)ref;
    return ACCESS_SESSION_DENIED;
}
