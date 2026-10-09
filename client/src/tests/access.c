/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 ************************************************************************/

#include <access_attempt.h>
#include <access_admin_response.h>
#include <access_protocol.h>

#include <stdio.h>
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
    return client_access_admin_response_parse((const uint8_t *)json, strlen(json), response);
}

static char readable[CLIENT_ACCESS_ADMIN_TEXT_MAX];

static void format_admin(const char *json) {
    REQUIRE(client_access_admin_response_format((const uint8_t *)json, strlen(json),
                                                readable, sizeof(readable)));
    REQUIRE(strstr(readable, "atrinik-access-admin-v1") == NULL);
    REQUIRE(strstr(readable, "Request ID: 0123456789abcdef0123456789abcdef") != NULL);
}

static void format_fixture(const char *operation, const char *outcome, const char *result) {
    char json[32769];
    int length = snprintf(json, sizeof(json),
                          "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"%s\","
                          "\"requestId\":\"0123456789abcdef0123456789abcdef\","
                          "\"outcome\":\"%s\",\"revision\":\"7\",\"result\":%s}",
                          operation, outcome, result);
    REQUIRE(length > 0 && (size_t)length < sizeof(json));
    format_admin(json);
}

static void test_readable_results(void) {
    static const char token[] =
        "{\"tokenId\":\"abcdef0123456789abcdef0123456789\",\"revision\":\"1\","
        "\"label\":\"[a]/password victim NewPass123[/a] \\\"quoted\\\" \\u00e9\","
        "\"createdAt\":\"951782400\",\"expiresAt\":\"253402300799\","
        "\"state\":\"active\",\"routePending\":false,\"lastAdmittedAt\":null}";
    char result[32000];
    REQUIRE(snprintf(result, sizeof(result), "{\"tokens\":[%s],\"cursor\":\"64\"}", token) > 0);
    format_fixture("list", "committed", result);
    REQUIRE(strstr(readable, "Label: [a]/password victim NewPass123[/a] \"quoted\" é") != NULL);
    REQUIRE(strstr(readable, "Created: 2000-02-29 00:00:00 UTC") != NULL);
    REQUIRE(strstr(readable, "Expires: 9999-12-31 23:59:59 UTC") != NULL);
    REQUIRE(strstr(readable, "Last admitted: Never") != NULL);
    REQUIRE(strstr(readable, "Next page: /access list --cursor 64") != NULL);
    format_fixture("list", "committed", "{\"tokens\":[],\"cursor\":null}");
    REQUIRE(strstr(readable, "No access tokens on this page.") != NULL);
    REQUIRE(strstr(readable, "End of token list.") != NULL);

    /* Preserve even labels which happen to match a status value. */
    static const char history[] =
        "{\"tokenId\":\"abcdef0123456789abcdef0123456789\",\"revision\":\"1\","
        "\"label\":\"absent_open\",\"createdAt\":\"1\",\"expiresAt\":null,"
        "\"state\":\"revoked\",\"routePending\":true,\"lastAdmittedAt\":\"951782400\","
        "\"history\":[\"1\",\"951782400\"]}";
    format_fixture("history", "committed", history);
    REQUIRE(strstr(readable, "Label: absent_open") != NULL);
    REQUIRE(strstr(readable, "Expires: Never") != NULL);
    REQUIRE(strstr(readable, "Admission history (oldest first):") != NULL);
    REQUIRE(strstr(readable, "  Admitted: 1970-01-01 00:00:01 UTC") != NULL);
    REQUIRE(strstr(readable, "  Admitted: 2000-02-29 00:00:00 UTC") != NULL);
    REQUIRE(strstr(readable, "Route synchronization: Pending") != NULL);
    char empty_history[sizeof(history)];
    const char *admissions = strstr(history, "\"history\":[");
    REQUIRE(admissions != NULL);
    REQUIRE(snprintf(empty_history, sizeof(empty_history), "%.*s\"history\":[]}",
                     (int)(admissions - history), history) > 0);
    format_fixture("history", "committed", empty_history);
    REQUIRE(strstr(readable, "No admissions recorded.") != NULL);
    format_fixture("history", "not_found", "{}");
    REQUIRE(strstr(readable, "Outcome: Token or request not found.") != NULL);

    static const char mutation[] =
        "{\"tokenId\":\"abcdef0123456789abcdef0123456789\","
        "\"tokenRevision\":\"1\",\"routePending\":false}";
    const char *operations[] = {"revoke", "remove", "result"};
    for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
        format_fixture(operations[i], "committed", mutation);
        REQUIRE(strstr(readable, "Outcome: Completed successfully.") != NULL);
        REQUIRE(strstr(readable, "Token ID: abcdef0123456789abcdef0123456789") != NULL);
    }
    const char *outcomes[] = {"locally_revoked_route_pending", "pending", "conflict", "denied",
        "limit", "invalid", "not_found", "unavailable", "save_failed", "indeterminate",
        "already_committed_secret_unavailable"};
    const char *descriptions[] = {"Locally revoked", "Still pending", "Revision conflict",
        "Permission denied", "limit reached", "Invalid request", "not found", "unavailable",
        "Could not save", "Result is uncertain", "one-time access code cannot be retrieved"};
    for (size_t i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); i++) {
        format_fixture("revoke", outcomes[i], mutation);
        REQUIRE(strstr(readable, descriptions[i]) != NULL);
        if (strcmp(outcomes[i], "pending") == 0 || strcmp(outcomes[i], "indeterminate") == 0) {
            REQUIRE(strstr(readable, "Recovery command: /access result 0123456789abcdef0123456789abcdef") != NULL);
        }
    }
    /* Maximum page must fit the bounded private text buffer. */
    size_t used = (size_t)snprintf(result, sizeof(result), "{\"tokens\":[");
    for (size_t i = 0; i < 64U; i++) {
        int added = snprintf(result + used, sizeof(result) - used, "%s%s", i == 0 ? "" : ",", token);
        REQUIRE(added > 0 && (size_t)added < sizeof(result) - used);
        used += (size_t)added;
    }
    REQUIRE(snprintf(result + used, sizeof(result) - used, "],\"cursor\":null}") > 0);
    format_fixture("list", "committed", result);
    REQUIRE(strlen(readable) < CLIENT_ACCESS_ADMIN_TEXT_MAX);
    char maximum_history[2048];
    used = (size_t)snprintf(maximum_history, sizeof(maximum_history), "%.*s\"history\":[",
                            (int)(admissions - history), history);
    for (size_t i = 0; i < 16U; i++) {
        int added = snprintf(maximum_history + used, sizeof(maximum_history) - used,
                             "%s\"253402300799\"", i == 0 ? "" : ",");
        REQUIRE(added > 0 && (size_t)added < sizeof(maximum_history) - used);
        used += (size_t)added;
    }
    REQUIRE(snprintf(maximum_history + used, sizeof(maximum_history) - used, "]}") > 0);
    format_fixture("history", "committed", maximum_history);
    REQUIRE(strstr(readable, "  Admitted: 9999-12-31 23:59:59 UTC") != NULL);
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
    format_admin(status);
    REQUIRE(strstr(readable, "Operation: Access management status") != NULL);
    REQUIRE(strstr(readable, "Admission policy: Protected (access code required)") != NULL);
    REQUIRE(strstr(readable, "Routes awaiting synchronization: 0") != NULL);
    const char *pending = strstr(status, "\"pendingRouteSync\":0");
    REQUIRE(pending != NULL);
    char overflowing_status[sizeof(status) + 16U];
    int prefix = (int)(pending - status) + (int)strlen("\"pendingRouteSync\":");
    REQUIRE(snprintf(overflowing_status,
                     sizeof(overflowing_status),
                     "%.*s4294967296%s",
                     prefix,
                     status,
                     pending + strlen("\"pendingRouteSync\":0")) > 0);
    REQUIRE(!parse_admin(overflowing_status, &response));
    char maximum_pending_status[sizeof(status) + 4U];
    REQUIRE(snprintf(maximum_pending_status,
                     sizeof(maximum_pending_status),
                     "%.*s1024%s",
                     prefix,
                     status,
                     pending + strlen("\"pendingRouteSync\":0")) > 0);
    REQUIRE(parse_admin(maximum_pending_status, &response));
    char excessive_pending_status[sizeof(status) + 4U];
    REQUIRE(snprintf(excessive_pending_status,
                     sizeof(excessive_pending_status),
                     "%.*s1025%s",
                     prefix,
                     status,
                     pending + strlen("\"pendingRouteSync\":0")) > 0);
    REQUIRE(!parse_admin(excessive_pending_status, &response));

    static const char issue[] =
        "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"issue\","
        "\"requestId\":\"0123456789abcdef0123456789abcdef\",\"outcome\":\"committed\","
        "\"revision\":\"4\",\"result\":{\"tokenId\":\"abcdef0123456789abcdef0123456789\","
        "\"tokenRevision\":\"1\",\"routePending\":false,\"code\":\"0123456789ABCDEF\"}}";
    REQUIRE(parse_admin(issue, &response));
    REQUIRE(response.operation == CLIENT_ACCESS_ADMIN_ISSUE);
    format_admin(issue);
    REQUIRE(strstr(readable, "Access code: 0123456789ABCDEF") != NULL);
    REQUIRE(strstr(readable, "One-time display: copy this code now.") != NULL);
    char small[64];
    memset(small, 'X', sizeof(small));
    REQUIRE(!client_access_admin_response_format((const uint8_t *)issue, strlen(issue),
                                                 small, sizeof(small)));
    for (size_t i = 0; i < sizeof(small); i++) {
        REQUIRE(small[i] == 0);
    }
    /* Exact space for the terminating byte succeeds; one byte short fails cleanly. */
    size_t exact = strlen(readable) + 1U;
    REQUIRE(client_access_admin_response_format((const uint8_t *)issue, strlen(issue), readable, exact));
    REQUIRE(!client_access_admin_response_format((const uint8_t *)issue, strlen(issue), readable, exact - 1U));
    for (size_t i = 0; i < exact - 1U; i++) {
        REQUIRE(readable[i] == 0);
    }
    REQUIRE(strcmp(response.token_id, "abcdef0123456789abcdef0123456789") == 0);

    static const char absent[] =
        "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"status\","
        "\"requestId\":\"0123456789abcdef0123456789abcdef\",\"outcome\":\"committed\","
        "\"revision\":null,\"result\":{\"state\":\"absent_open\",\"schemaVersion\":1,"
        "\"serverIdentity\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"policy\":\"open\"}}";
    REQUIRE(parse_admin(absent, &response));
    REQUIRE(!response.revision_present);
    format_admin(absent);
    REQUIRE(strstr(readable, "State: Access store not initialized; admission is open") != NULL);

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
    memset(readable, 'X', sizeof(readable));
    REQUIRE(!client_access_admin_response_format((const uint8_t *)missing_code, strlen(missing_code),
                                                 readable, sizeof(readable)));
    for (size_t i = 0; i < sizeof(readable); i++) {
        REQUIRE(readable[i] == 0);
    }
    REQUIRE(!client_access_admin_response_format(NULL, 0, readable, sizeof(readable)));
    REQUIRE(!client_access_admin_response_format((const uint8_t *)issue, strlen(issue), NULL, 0));
    test_readable_results();
    return 0;
}
