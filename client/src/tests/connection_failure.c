/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#include <connection_failure.h>
#include <join_credentials.h>
#include <metaserver_direct.h>

#include <stdlib.h>
#include <string.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

int main(void) {
    char message[128];
    const struct {
        socket_connect_failure_code_t code;
        const char *text;
    } cases[] = {
        {SOCKET_CONNECT_FAILURE_UNAVAILABLE, "please try again"},
        {SOCKET_CONNECT_FAILURE_AUTHORIZATION, "invite was rejected"},
        {SOCKET_CONNECT_FAILURE_INVITE_EXPIRED, "invite has expired"},
        {SOCKET_CONNECT_FAILURE_SERVER_OFFLINE, "server is offline"},
        {SOCKET_CONNECT_FAILURE_TIMEOUT, "timed out"},
        {SOCKET_CONNECT_FAILURE_PROTOCOL_REVISION, "incompatible"},
        {SOCKET_CONNECT_FAILURE_RENDEZVOUS_UNAVAILABLE, "connection service"},
        {SOCKET_CONNECT_FAILURE_RENDEZVOUS_PROTOCOL, "rendezvous protocols"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        socket_connect_failure_t failure = {.code = cases[i].code};
        TEST_CHECK(client_connection_failure_format(&failure, message, sizeof(message)));
        TEST_CHECK(strstr(message, cases[i].text) != NULL);
    }

    socket_connect_failure_t limited = {
        .code = SOCKET_CONNECT_FAILURE_RATE_LIMITED,
        .retry_after_seconds = 86400,
    };
    TEST_CHECK(client_connection_failure_format(&limited, message, sizeof(message)));
    TEST_CHECK(strstr(message, "86400 seconds") != NULL);
    limited.retry_after_seconds = 0;
    TEST_CHECK(client_connection_failure_format(&limited, message, sizeof(message)));
    TEST_CHECK(strstr(message, "try again later") != NULL);

    char short_message[8];
    TEST_CHECK(!client_connection_failure_format(&limited, short_message, sizeof(short_message)));
    TEST_CHECK(!client_connection_failure_format(NULL, message, sizeof(message)));
    TEST_CHECK(client_join_password_missing("", NULL, NULL));
    TEST_CHECK(!client_join_password_missing("prompt", NULL, NULL));
    TEST_CHECK(!client_join_password_missing("", "configured", NULL));
    TEST_CHECK(!client_join_password_missing("", NULL, "existing"));
    char *selected_password = strdup("selected-secret");
    char *configured_password = strdup("configured-secret");
    TEST_CHECK(selected_password != NULL && configured_password != NULL);
    client_join_credentials_clear(&selected_password, &configured_password);
    TEST_CHECK(selected_password == NULL && configured_password == NULL);
    client_join_credentials_clear(&selected_password, &configured_password);
    rendezvous_invite_t *invite = calloc(1, sizeof(*invite));
    TEST_CHECK(invite != NULL);
    memset(invite->secret, 0xa5, sizeof(invite->secret));
    selected_password = strdup("failed-attempt");
    configured_password = strdup("configured-attempt");
    TEST_CHECK(selected_password != NULL && configured_password != NULL);
    client_attempt_secrets_clear(&selected_password, &configured_password, &invite);
    TEST_CHECK(selected_password == NULL && configured_password == NULL && invite == NULL);
    client_attempt_secrets_clear(&selected_password, &configured_password, &invite);

    static const char identity[] =
        "505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f";
    static const char different_identity[] =
        "605152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f";
    TEST_CHECK(metaserver_direct_identity_valid(identity, identity));
    TEST_CHECK(!metaserver_direct_identity_valid(identity, different_identity));
    TEST_CHECK(!metaserver_direct_identity_valid(identity, NULL));
    return 0;
}
