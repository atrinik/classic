/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 ************************************************************************/

#include <access_attempt.h>
#include <access_admin_response.h>
#include <access_protocol.h>

#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)  \
    do {                    \
        if (!(condition)) { \
            abort();        \
        }                   \
    } while (0)

static void require_cleared(const client_access_attempt_t *attempt) {
    const unsigned char *bytes = (const unsigned char *)attempt;
    for (size_t i = 0; i < sizeof(*attempt); i++) {
        REQUIRE(bytes[i] == 0);
    }
}

static bool parse_admin(const char *json, client_access_admin_response_t *response) {
    return client_access_admin_response_parse(
        (const uint8_t *)json, strlen(json), response);
}

int main(void) {
    client_access_attempt_t attempt = {0};
    static const char input[] = "0123456789abcdef";
    REQUIRE(client_access_attempt_set(&attempt, input, strlen(input)));
    REQUIRE(attempt.present);
    REQUIRE(strcmp(attempt.code, "0123456789ABCDEF") == 0);

    REQUIRE(!client_access_attempt_set(&attempt, "not-a-code", 10));
    require_cleared(&attempt);
    client_access_attempt_clear(&attempt);
    require_cleared(&attempt);

    const uint8_t open[] = {1, 0};
    const uint8_t protected[] = {1, 1};
    bool value;
    REQUIRE(client_access_policy_parse(open, sizeof(open), &value) && !value);
    REQUIRE(client_access_policy_parse(protected, sizeof(protected), &value) && value);
    REQUIRE(client_access_result_parse(open, sizeof(open), &value) && value);
    REQUIRE(client_access_result_parse(protected, sizeof(protected), &value) && !value);

    const uint8_t bad_version[] = {2, 0};
    const uint8_t bad_value[] = {1, 2};
    const uint8_t trailing[] = {1, 0, 0};
    REQUIRE(!client_access_policy_parse(NULL, 0, &value));
    REQUIRE(!client_access_policy_parse(open, 1, &value));
    REQUIRE(!client_access_policy_parse(bad_version, sizeof(bad_version), &value));
    REQUIRE(!client_access_policy_parse(bad_value, sizeof(bad_value), &value));
    REQUIRE(!client_access_policy_parse(trailing, sizeof(trailing), &value));
    REQUIRE(!client_access_result_parse(open, sizeof(open), NULL));

    client_access_admin_response_t response;
    static const char status[] =
        "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"status\","
        "\"requestId\":\"0123456789abcdef0123456789abcdef\",\"outcome\":\"committed\","
        "\"revision\":\"3\",\"result\":{\"state\":\"initialized\",\"schemaVersion\":1,"
        "\"serverIdentity\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"policy\":\"protected\",\"integrity\":\"ok\",\"durability\":\"ok\","
        "\"revision\":\"3\",\"pendingRouteSync\":0}}";
    REQUIRE(parse_admin(status, &response));
    REQUIRE(response.operation == CLIENT_ACCESS_ADMIN_STATUS);
    REQUIRE(response.committed && response.revision_present);
    REQUIRE(strcmp(response.revision, "3") == 0);

    static const char issue[] =
        "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"issue\","
        "\"requestId\":\"0123456789abcdef0123456789abcdef\",\"outcome\":\"committed\","
        "\"revision\":\"4\",\"result\":{\"tokenId\":\"abcdef0123456789abcdef0123456789\","
        "\"tokenRevision\":\"1\",\"routePending\":false,\"code\":\"0123456789ABCDEF\"}}";
    REQUIRE(parse_admin(issue, &response));
    REQUIRE(response.operation == CLIENT_ACCESS_ADMIN_ISSUE);

    static const char absent[] =
        "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"status\","
        "\"requestId\":\"0123456789abcdef0123456789abcdef\",\"outcome\":\"committed\","
        "\"revision\":null,\"result\":{\"state\":\"absent_open\",\"schemaVersion\":1,"
        "\"serverIdentity\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"policy\":\"open\"}}";
    REQUIRE(parse_admin(absent, &response));
    REQUIRE(!response.revision_present);

    static const char duplicate[] =
        "{\"schema\":\"atrinik-access-admin-v1\",\"schema\":\"atrinik-access-admin-v1\","
        "\"operation\":\"status\",\"requestId\":\"0123456789abcdef0123456789abcdef\","
        "\"outcome\":\"committed\",\"revision\":null,\"result\":{}}";
    REQUIRE(!parse_admin(duplicate, &response));
    REQUIRE(!parse_admin("{\"schema\":\"atrinik-access-admin-v1\"} trailing", &response));
    static const char missing_code[] =
        "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"issue\","
        "\"requestId\":\"0123456789abcdef0123456789abcdef\",\"outcome\":\"committed\","
        "\"revision\":\"4\",\"result\":{\"tokenId\":\"abcdef0123456789abcdef0123456789\","
        "\"tokenRevision\":\"1\",\"routePending\":false}}";
    REQUIRE(!parse_admin(missing_code, &response));
    return 0;
}
