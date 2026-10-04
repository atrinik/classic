/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <toolkit/rendezvous.h>
#include <toolkit/socket.h>
#include <toolkit/datetime.h>
#define REQUIRE(x)                                  \
    do {                                            \
        if (!(x)) {                                 \
            fprintf(stderr, "line %d\n", __LINE__); \
            return 1;                               \
        }                                           \
    } while (0)
int main(void) {
    toolkit_import(datetime);
    rendezvous_access_grant_t grant = {0};
    memset(grant.server_id, 'a', 64);
    memset(grant.generation, 'b', 64);
    memset(grant.client_nonce, 'c', 64);
    memset(grant.grant, 'd', 64);
    grant.expiry = (uint64_t)time(NULL) + 10;
    REQUIRE(rendezvous_access_grant_valid(&grant, grant.server_id, grant.expiry - 10));
    REQUIRE(!rendezvous_access_grant_valid(&grant, grant.server_id, grant.expiry));
    REQUIRE(!rendezvous_access_grant_valid(&grant, grant.client_nonce, grant.expiry - 10));
    REQUIRE(!rendezvous_access_grant_valid(&grant, grant.server_id, grant.expiry - 16));
    char frame[513];
    socket_rendezvous_attempt_t *attempt =
        socket_rendezvous_attempt_create(grant.server_id,
                                         grant.grant,
                                         &grant,
                                         datetime_monotonic_ms() + 10000);
    REQUIRE(socket_rendezvous_attempt_create(grant.server_id,
                                             grant.client_nonce,
                                             &grant,
                                             datetime_monotonic_ms() + 10000) == NULL);
    REQUIRE(attempt != NULL);
    REQUIRE(!socket_rendezvous_attempt_directory_probe_allowed(attempt));
    REQUIRE(!socket_rendezvous_attempt_peer_traffic_allowed(attempt));
    REQUIRE(socket_rendezvous_attempt_auth_init(attempt, frame, sizeof(frame)));
    REQUIRE(strstr(frame, "access_init") != NULL && strstr(frame, grant.grant) != NULL);
    const char ready[] = "{\"type\":\"access_ready\",\"version\":1}";
    REQUIRE(socket_rendezvous_attempt_auth_result(attempt, ready, sizeof(ready) - 1) ==
            SOCKET_RENDEZVOUS_FRAME_AUTHORIZED);
    REQUIRE(socket_rendezvous_attempt_client_candidate(attempt,
                                                       "127.0.0.1",
                                                       13327,
                                                       frame,
                                                       sizeof(frame)));
    REQUIRE(!socket_rendezvous_attempt_client_candidate(attempt,
                                                        "127.0.0.1",
                                                        13327,
                                                        frame,
                                                        sizeof(frame)));
    socket_rendezvous_attempt_destroy(attempt);
    attempt = socket_rendezvous_attempt_create(grant.server_id,
                                               grant.grant,
                                               &grant,
                                               datetime_monotonic_ms() + 10000);
    REQUIRE(attempt != NULL);
    REQUIRE(socket_rendezvous_attempt_auth_result(attempt, ready, sizeof(ready) - 1) ==
            SOCKET_RENDEZVOUS_FRAME_INVALID);
    REQUIRE(!socket_rendezvous_attempt_auth_init(attempt, frame, sizeof(frame)));
    socket_rendezvous_attempt_destroy(attempt);
    const char *bad[] = {"",
                         "{\"type\":\"access_ready\",\"version\":2}",
                         "{\"type\":\"access_ready\",\"version\":1} ",
                         "{\"type\":\"auth_result\",\"version\":1}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        REQUIRE(!rendezvous_access_ready_parse(bad[i]));
    rendezvous_access_grant_clear(&grant);
    REQUIRE(grant.expiry == 0 && grant.grant[0] == 0);
    toolkit_deinit();
    return 0;
}
