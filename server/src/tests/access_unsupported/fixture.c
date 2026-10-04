/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <access_server.h>
#include <assert.h>

settings_struct settings;

static void denied(void)
{
    char output[8] = "secret";
    size_t length = 6;
    access_outcome_t outcome = ACCESS_COMMITTED;
    access_token_ref_t ref;
    memset(&ref, 0xff, sizeof(ref));
    assert(access_server_auth_submit("0123456789ABCDEF") == 0);
    assert(access_server_root_submit("{}", 2) == 0);
    assert(access_server_admin_submit("{}", 2, "root") == 0);
    assert(!access_server_admin_poll(1, output, sizeof(output), &length));
    assert(length == 0 && output[0] == '\0');
    assert(!access_server_auth_poll(1, &outcome, &ref));
    assert(outcome == ACCESS_UNAVAILABLE && ref.revision == 0);
    for (size_t i = 0; i < sizeof(ref.token_id); i++) assert(ref.token_id[i] == 0);
    assert(access_server_session_check(&ref) == ACCESS_SESSION_DENIED);
    assert(!access_operator_allowed("/unused", "root"));
    assert(!access_operator_allowed(NULL, NULL));
    assert(!access_server_admin_poll(0, NULL, 0, NULL));
    assert(!access_server_auth_poll(0, NULL, NULL));
    access_server_cancel(1);
    access_server_tick();
}

int main(void)
{
    denied();
    /* Every combination of startup settings, for public and private servers. */
    for (unsigned visibility = 0; visibility < 2; visibility++) {
        for (unsigned mask = 0; mask < 16; mask++) {
            memset(&settings, 0, sizeof(settings));
            settings.server_public = visibility != 0;
            settings.access_required = (mask & 1) != 0;
            settings.access_initialize = (mask & 2) != 0;
            if (mask & 4) strcpy(settings.access_store, "/unused/store");
            if (mask & 8) strcpy(settings.access_admin_accounts, "/unused/accounts");
            assert(access_server_init(NULL) == (mask == 0));
            assert(access_server_healthy() == (mask == 0));
            denied();
            assert(access_server_shutdown() == (mask == 0));
            access_server_deinit();
        }
    }
    memset(&settings, 0, sizeof(settings));
    assert(access_server_init(NULL));
    access_server_save_failed();
    assert(!access_server_healthy());
    assert(!access_server_shutdown());
    denied();
    puts("PASS: 32 startup cases, all administration/authentication denied, save failure fenced");
    return 0;
}
