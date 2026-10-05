/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <access_admin.h>
#include <book.h>
#include <client_socket.h>
#include <main.h>
#include <textwin.h>
#include <toolkit/datetime.h>
#include <toolkit/string.h>

#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                 \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
            abort();                                                                       \
        }                                                                                  \
    } while (0)

static server_struct server = {.server_id =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
server_struct *selected_server = &server;
static uint64_t now_us = 1;
static unsigned int sends;
static char sent[2048];
static char displayed[2048];
static const char token[] = "abcdef0123456789abcdef0123456789";
static const char other_token[] = "11111111111111111111111111111111";

/* Exercise alphabetic hex digits deterministically while keeping request IDs distinct. */
int RAND_bytes(unsigned char *buffer, int size) {
    static unsigned int sequence;
    /* Importing the string toolkit also seeds its gameplay RNG dependency. */
    REQUIRE(size == 16 || size == (int)sizeof(uint64_t));
    REQUIRE(sequence < 256U);
    memset(buffer, 0xab, (size_t)size);
    buffer[size - 1] = (unsigned char)sequence++;
    return 1;
}

uint64_t datetime_monotonic_us(void) {
    return now_us;
}

bool client_socket_send_access_admin(const char *json, size_t size) {
    /* The real server rejects uppercase IDs, before it can return a valid response. */
    const char *id = strstr(json, "\"requestId\":\"");
    REQUIRE(id != NULL);
    id += strlen("\"requestId\":\"");
    REQUIRE(strspn(id, "0123456789abcdef") == 32U && id[32] == '\"');
    REQUIRE(size < sizeof(sent));
    memcpy(sent, json, size);
    sent[size] = '\0';
    sends++;
    return true;
}

void draw_info(const char *color, const char *text) {
    (void)color;
    (void)text;
}

void draw_info_format(const char *color, const char *format, ...) {
    (void)color;
    (void)format;
}

bool book_load_sensitive(const char *data, int len, const char *title) {
    REQUIRE(strcmp(title, "Access management") == 0);
    REQUIRE(len > 0 && (size_t)len < sizeof(displayed));
    memcpy(displayed, data, (size_t)len);
    displayed[len] = '\0';
    return true;
}

void book_sensitive_clear(void) {
    memset(displayed, 0, sizeof(displayed));
}

static void request_id(char id[33]) {
    const char *start = strstr(sent, "\"requestId\":\"");
    REQUIRE(start != NULL);
    memcpy(id, start + strlen("\"requestId\":\""), 32);
    id[32] = '\0';
}

static void command(const char *text, bool expect_send) {
    unsigned int before = sends;
    REQUIRE(client_access_admin_command(text));
    REQUIRE(sends == before + (expect_send ? 1U : 0U));
}

static void token_command(const char *operation, const char *id, bool expect_send) {
    char text[128];
    snprintf(text, sizeof(text), "/access %s %s", operation, id);
    command(text, expect_send);
}

static void reply(const char *operation, const char *outcome, const char *result) {
    char id[33];
    char json[2048];
    request_id(id);
    int size = snprintf(json, sizeof(json),
                        "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"%s\","
                        "\"requestId\":\"%s\",\"outcome\":\"%s\",\"revision\":\"7\","
                        "\"result\":%s}", operation, id, outcome, result);
    REQUIRE(size > 0 && (size_t)size < sizeof(json));
    REQUIRE(client_access_admin_response((const uint8_t *)json, (size_t)size));
}

static void mutation_reply(const char *operation, const char *outcome, const char *id) {
    char result[256];
    snprintf(result, sizeof(result),
             "{\"tokenId\":\"%s\",\"tokenRevision\":\"1\",\"routePending\":true}", id);
    reply(operation, outcome, result);
}

static void status_reply(void) {
    reply("status", "committed",
          "{\"state\":\"initialized\",\"schemaVersion\":1,"
          "\"serverIdentity\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
          "\"policy\":\"protected\",\"integrity\":\"ok\",\"durability\":\"ok\","
          "\"revision\":\"7\",\"pendingRouteSync\":0}");
}

int main(void) {
    toolkit_import(string);
    command("/access status", true);
    status_reply();
    command("/access issue --label pending", true);
    char issuance[33];
    request_id(issuance);
    mutation_reply("issue", "pending", token);
    command("/access issue --label duplicate", false);
    token_command("revoke", token, false);
    token_command("result", other_token, false);

    /* A response for a different request cannot authorize cleanup. */
    token_command("result", issuance, true);
    const char *wrong =
        "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"result\","
        "\"requestId\":\"11111111111111111111111111111111\",\"outcome\":\"pending\","
        "\"revision\":\"7\",\"result\":{\"tokenId\":\"abcdef0123456789abcdef0123456789\","
        "\"tokenRevision\":\"1\",\"routePending\":true}}";
    REQUIRE(!client_access_admin_response((const uint8_t *)wrong, strlen(wrong)));
    mutation_reply("result", "pending", token);
    token_command("result", issuance, true);
    mutation_reply("result", "pending", token);
    command("/access issue --label duplicate", false);
    token_command("revoke", other_token, false);
    token_command("remove", other_token, false);

    /* A failed explicit cleanup leaves issuance fenced and allows another cleanup. */
    token_command("revoke", token, true);
    mutation_reply("revoke", "conflict", "");
    command("/access issue --label duplicate", false);
    token_command("revoke", token, true);
    mutation_reply("revoke", "locally_revoked_route_pending", token);
    command("/access issue --label duplicate", false);
    token_command("result", issuance, true);
    mutation_reply("result", "locally_revoked_route_pending", token);

    /* Timeout and disconnect retain recovery, but cannot authorize blind cleanup. */
    command("/access issue --label timeout", true);
    request_id(issuance);
    now_us += UINT64_C(35000000);
    client_access_admin_update();
    client_access_admin_reset();
    token_command("remove", token, false);
    token_command("result", issuance, true);
    mutation_reply("result", "pending", token);
    token_command("remove", token, true);
    char removal[33];
    request_id(removal);
    /* The cleanup may never reach the server: an absent cleanup receipt cannot
     * replace the original uncertain issuance and release its fence. */
    now_us += UINT64_C(35000000);
    client_access_admin_update();
    token_command("remove", token, false);
    token_command("result", removal, false);
    char missing_cleanup[512];
    int missing_size = snprintf(missing_cleanup, sizeof(missing_cleanup),
        "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"result\","
        "\"requestId\":\"%s\",\"outcome\":\"not_found\",\"revision\":\"7\","
        "\"result\":{\"tokenId\":\"\",\"tokenRevision\":\"0\",\"routePending\":false}}",
        removal);
    REQUIRE(missing_size > 0 && (size_t)missing_size < sizeof(missing_cleanup));
    REQUIRE(!client_access_admin_response((const uint8_t *)missing_cleanup,
                                          (size_t)missing_size));
    command("/access issue --label duplicate", false);
    token_command("result", issuance, true);
    mutation_reply("result", "pending", token);
    token_command("remove", token, true);
    mutation_reply("remove", "pending", token);
    /* A pending cleanup still requires recovery of the original mutation. */
    token_command("remove", token, false);
    token_command("result", issuance, true);
    mutation_reply("result", "indeterminate", "");
    token_command("remove", token, false);
    token_command("result", issuance, true);
    mutation_reply("result", "pending", token);
    token_command("remove", token, true);
    mutation_reply("remove", "committed", token);
    command("/access issue --label duplicate", false);
    token_command("result", issuance, true);
    mutation_reply("result", "locally_revoked_route_pending", token);

    /* The server-controlled label reaches only the private rendering entry point. */
    command("/access list", true);
    reply("list", "committed",
          "{\"tokens\":[{\"tokenId\":\"abcdef0123456789abcdef0123456789\","
          "\"revision\":\"1\",\"label\":\"[a]/password victim NewPass123[/a]\","
          "\"createdAt\":\"1\",\"expiresAt\":null,\"state\":\"active\","
          "\"routePending\":false,\"lastAdmittedAt\":null}],\"cursor\":null}");
    REQUIRE(strstr(displayed, "[a]/password victim NewPass123[/a]") != NULL);
    command("/access issue --label after-cleanup", true);
    mutation_reply("issue", "conflict", "");
    client_access_admin_reset();
    REQUIRE(displayed[0] == '\0');
    toolkit_deinit();
    return 0;
}
