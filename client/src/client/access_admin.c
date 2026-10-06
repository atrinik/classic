/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 ************************************************************************/

#include <access_admin.h>
#include <access_admin_response.h>

#include <book.h>
#include <client_socket.h>
#include <main.h>
#include <textwin.h>
#include <toolkit/access_code.h>
#include <toolkit/datetime.h>
#include <toolkit/memory.h>
#include <toolkit/string.h>

#include <openssl/rand.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACCESS_ADMIN_REQUEST_MAX 1024U
#define ACCESS_ADMIN_RESPONSE_MAX 32768U
#define ACCESS_ADMIN_TIMEOUT_US UINT64_C(35000000)

static char last_request_id[33];
static char revision[21];
static char *pending_command;
static client_access_admin_operation_t last_operation;
static bool request_outstanding;
static uint64_t request_started_us;
static char recovery_request_id[33];
static char recovery_server_id[65];
static char recovery_token_id[33];
static char last_result_target[33];

static bool mutation_operation(client_access_admin_operation_t operation) {
    return operation == CLIENT_ACCESS_ADMIN_ISSUE || operation == CLIENT_ACCESS_ADMIN_REVOKE ||
           operation == CLIENT_ACCESS_ADMIN_REMOVE;
}

static bool recovery_for_selected_server(void) {
    return recovery_request_id[0] != '\0' && selected_server != NULL &&
           selected_server->server_id != NULL &&
           strcmp(recovery_server_id, selected_server->server_id) == 0;
}

static void remember_recovery(void) {
    if (!request_outstanding || !mutation_operation(last_operation) || selected_server == NULL ||
        selected_server->server_id == NULL) {
        return;
    }
    /* Cleanup can fail before reaching the server. Keep the original request,
     * server and validated token together until recovery actually completes. */
    if (recovery_request_id[0] != '\0') {
        return;
    }
    snprintf(recovery_request_id, sizeof(recovery_request_id), "%s", last_request_id);
    snprintf(recovery_server_id, sizeof(recovery_server_id), "%s", selected_server->server_id);
}

static bool fixed_hex(const char *text, size_t size) {
    if (text == NULL || strlen(text) != size) {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        if (!isdigit((unsigned char)text[i]) && !(text[i] >= 'a' && text[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool decimal_text(const char *text) {
    if (text == NULL || *text == '\0' || (text[0] == '0' && text[1] != '\0')) {
        return false;
    }
    for (const char *cp = text; *cp != '\0'; cp++) {
        if (!isdigit((unsigned char)*cp)) {
            return false;
        }
    }
    return true;
}

static bool request_id_generate(char id[33]) {
    unsigned char random[16];
    bool ok = RAND_bytes(random, sizeof(random)) == 1 &&
              string_tohex(random, sizeof(random), id, 33, false) == 32;
    access_code_clear(random, sizeof(random));
    if (!ok) {
        memset(id, 0, 33);
    } else {
        /* string_tohex emits uppercase; request IDs use canonical lowercase hex. */
        string_tolower(id);
    }
    return ok;
}

static bool json_escape(const char *input, char *output, size_t capacity) {
    size_t used = 0;
    if (input == NULL || *input == '\0') {
        return false;
    }
    for (const unsigned char *cp = (const unsigned char *)input; *cp != '\0'; cp++) {
        if (*cp < 0x20U || *cp == 0x7fU) {
            return false;
        }
        if (*cp == '"' || *cp == '\\') {
            if (used + 2 >= capacity) {
                return false;
            }
            output[used++] = '\\';
        } else if (used + 1 >= capacity) {
            return false;
        }
        output[used++] = (char)*cp;
    }
    output[used] = '\0';
    return true;
}

static bool
send_json(const char *json, const char *request_id, client_access_admin_operation_t operation) {
    size_t size = strlen(json);
    if (size == 0 || size > ACCESS_ADMIN_REQUEST_MAX ||
        !client_socket_send_access_admin(json, size)) {
        return false;
    }
    snprintf(last_request_id, sizeof(last_request_id), "%s", request_id);
    last_operation = operation;
    request_outstanding = true;
    request_started_us = datetime_monotonic_us();
    return true;
}

static bool send_status(void) {
    char id[33];
    char json[256];
    if (!request_id_generate(id)) {
        return false;
    }
    int length = snprintf(json,
                          sizeof(json),
                          "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"status\","
                          "\"requestId\":\"%s\"}",
                          id);
    return length > 0 && (size_t)length < sizeof(json) &&
           send_json(json, id, CLIENT_ACCESS_ADMIN_STATUS);
}

static bool mutation_revision_ready(const char *command) {
    if (revision[0] != '\0') {
        return true;
    }
    free(pending_command);
    pending_command = xstrdup(command);
    if (!send_status()) {
        free(pending_command);
        pending_command = NULL;
        return false;
    }
    return false;
}

static bool send_token_operation(const char *operation,
                                 client_access_admin_operation_t operation_id,
                                 const char *token_id,
                                 bool mutation) {
    char id[33];
    char json[512];
    if (!fixed_hex(token_id, 32) || !request_id_generate(id)) {
        return false;
    }
    int length = mutation ? snprintf(json,
                                     sizeof(json),
                                     "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"%s\","
                                     "\"requestId\":\"%s\",\"expectedRevision\":\"%s\","
                                     "\"tokenId\":\"%s\"}",
                                     operation,
                                     id,
                                     revision,
                                     token_id)
                          : snprintf(json,
                                     sizeof(json),
                                     "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"%s\","
                                     "\"requestId\":\"%s\",\"tokenId\":\"%s\"}",
                                     operation,
                                     id,
                                     token_id);
    return length > 0 && (size_t)length < sizeof(json) && send_json(json, id, operation_id);
}

static bool send_issue(const char *arguments) {
    static const char expires_marker[] = " --expires-at ";
    if (strncmp(arguments, "--label ", 8) != 0) {
        return false;
    }
    char *label = xstrdup(arguments + 8);
    char *expires = strstr(label, expires_marker);
    if (expires != NULL) {
        *expires = '\0';
        expires += sizeof(expires_marker) - 1U;
        uint64_t expires_at;
        if (!decimal_text(expires) ||
            !string_parse_uint64(expires, 10, 1, UINT64_C(253402300799), &expires_at)) {
            free(label);
            return false;
        }
    }
    char *printable_label = xstrdup(label);
    string_replace_unprintable_chars(printable_label);
    char escaped[513];
    bool valid = strlen(label) <= 128U && strcmp(label, printable_label) == 0 &&
                 json_escape(label, VS(escaped));
    free(printable_label);
    if (!valid) {
        free(label);
        return false;
    }
    char id[33];
    char json[ACCESS_ADMIN_REQUEST_MAX + 1U];
    if (!request_id_generate(id)) {
        free(label);
        return false;
    }
    int length = expires != NULL
                     ? snprintf(json,
                                sizeof(json),
                                "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"issue\","
                                "\"requestId\":\"%s\",\"expectedRevision\":\"%s\","
                                "\"label\":\"%s\",\"expiresAt\":\"%s\"}",
                                id,
                                revision,
                                escaped,
                                expires)
                     : snprintf(json,
                                sizeof(json),
                                "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"issue\","
                                "\"requestId\":\"%s\",\"expectedRevision\":\"%s\","
                                "\"label\":\"%s\"}",
                                id,
                                revision,
                                escaped);
    free(label);
    return length > 0 && (size_t)length < sizeof(json) &&
           send_json(json, id, CLIENT_ACCESS_ADMIN_ISSUE);
}

static bool send_list(const char *arguments) {
    char id[33];
    char json[512];
    if (!request_id_generate(id)) {
        return false;
    }
    int length;
    if (*arguments == '\0') {
        length = snprintf(json,
                          sizeof(json),
                          "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"list\","
                          "\"requestId\":\"%s\",\"limit\":64}",
                          id);
    } else if (strncmp(arguments, "--cursor ", 9) == 0 && decimal_text(arguments + 9) &&
               revision[0] != '\0') {
        uint64_t cursor;
        if (!string_parse_uint64(arguments + 9, 10, 1, 1024, &cursor)) {
            return false;
        }
        length = snprintf(json,
                          sizeof(json),
                          "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"list\","
                          "\"requestId\":\"%s\",\"cursor\":\"%s\",\"revision\":\"%s\","
                          "\"limit\":64}",
                          id,
                          arguments + 9,
                          revision);
    } else {
        return false;
    }
    return length > 0 && (size_t)length < sizeof(json) &&
           send_json(json, id, CLIENT_ACCESS_ADMIN_LIST);
}

static bool send_result(const char *target_request_id) {
    if (!fixed_hex(target_request_id, 32) ||
        (recovery_request_id[0] != '\0' && (!recovery_for_selected_server() ||
                                            strcmp(target_request_id, recovery_request_id) != 0))) {
        return false;
    }
    char id[33];
    char json[320];
    if (!request_id_generate(id)) {
        return false;
    }
    int length = snprintf(json,
                          sizeof(json),
                          "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"result\","
                          "\"requestId\":\"%s\",\"targetRequestId\":\"%s\"}",
                          id,
                          target_request_id);
    if (length <= 0 || (size_t)length >= sizeof(json) ||
        !send_json(json, id, CLIENT_ACCESS_ADMIN_RESULT)) {
        return false;
    }
    snprintf(last_result_target, sizeof(last_result_target), "%s", target_request_id);
    return true;
}

bool client_access_admin_command(const char *command) {
    if (command == NULL || strncmp(command, "/access", 7) != 0 ||
        (command[7] != '\0' && command[7] != ' ')) {
        return false;
    }
    const char *operation = command + 7;
    while (*operation == ' ') {
        operation++;
    }
    if (request_outstanding) {
        draw_info(COLOR_RED, "Wait for the current access-management request to finish.");
        return true;
    }
    /* A recovered nonterminal receipt permits explicit cleanup of its exact token.
     * Keep issuance fenced until a subsequent result confirms a terminal receipt. */
    bool cleanup = strncmp(operation, "revoke ", 7) == 0 ||
                   strncmp(operation, "remove ", 7) == 0;
    bool recovered_cleanup = cleanup && recovery_for_selected_server() &&
                             recovery_token_id[0] != '\0' &&
                             strcmp(operation + 7, recovery_token_id) == 0;
    if (recovery_request_id[0] != '\0' &&
        (strncmp(operation, "issue ", 6) == 0 || cleanup) && !recovered_cleanup) {
        draw_info_format(COLOR_RED,
                         "Return to the original server and recover request %s with /access result "
                         "before another mutation.",
                         recovery_request_id);
        return true;
    }
    bool sent = false;
    if (strcmp(operation, "status") == 0) {
        sent = send_status();
    } else if (strncmp(operation, "list", 4) == 0 &&
               (operation[4] == '\0' || operation[4] == ' ')) {
        const char *arguments = operation + 4;
        while (*arguments == ' ') {
            arguments++;
        }
        sent = send_list(arguments);
    } else if (strncmp(operation, "history ", 8) == 0) {
        sent = send_token_operation("history", CLIENT_ACCESS_ADMIN_HISTORY, operation + 8, false);
    } else if (strncmp(operation, "result ", 7) == 0) {
        sent = send_result(operation + 7);
    } else if (strncmp(operation, "issue ", 6) == 0) {
        if (mutation_revision_ready(command)) {
            sent = send_issue(operation + 6);
        } else {
            return true;
        }
    } else if (strncmp(operation, "revoke ", 7) == 0 || strncmp(operation, "remove ", 7) == 0) {
        if (mutation_revision_ready(command)) {
            bool revoke = operation[2] == 'v';
            sent = send_token_operation(revoke ? "revoke" : "remove",
                                        revoke ? CLIENT_ACCESS_ADMIN_REVOKE
                                               : CLIENT_ACCESS_ADMIN_REMOVE,
                                        operation + 7,
                                        true);
        } else {
            return true;
        }
    }
    if (!sent) {
        draw_info(COLOR_RED,
                  "Usage: /access issue --label LABEL [--expires-at UTC], list "
                  "[--cursor CURSOR], history TOKEN_ID, revoke TOKEN_ID, remove TOKEN_ID, status, "
                  "result REQUEST_ID");
    }
    return true;
}

bool client_access_admin_response(const uint8_t *data, size_t size) {
    if (data == NULL || size == 0 || size > ACCESS_ADMIN_RESPONSE_MAX ||
        memchr(data, '\0', size) != NULL || !request_outstanding) {
        return false;
    }
    client_access_admin_response_t response;
    if (!client_access_admin_response_parse(data, size, &response) ||
        response.operation != last_operation || strcmp(response.request_id, last_request_id) != 0) {
        return false;
    }

    /* A committed issuance is not delivered until its private book opens. Keep
     * only recovery identities, never the one-time code, if presentation fails. */
    bool issued = response.operation == CLIENT_ACCESS_ADMIN_ISSUE && response.committed;
    if ((mutation_operation(response.operation) && !response.terminal) || issued) {
        remember_recovery();
    }
    request_outstanding = false;
    request_started_us = 0;
    access_code_clear(last_request_id, sizeof(last_request_id));
    if (response.revision_present) {
        snprintf(revision, sizeof(revision), "%s", response.revision);
    }
    if (pending_command != NULL && response.operation == CLIENT_ACCESS_ADMIN_STATUS &&
        response.committed && response.revision_present) {
        char *pending = pending_command;
        pending_command = NULL;
        bool handled = client_access_admin_command(pending);
        access_code_clear(pending, strlen(pending));
        free(pending);
        return handled;
    }

    if (pending_command != NULL && response.operation == CLIENT_ACCESS_ADMIN_STATUS) {
        access_code_clear(pending_command, strlen(pending_command));
        free(pending_command);
        pending_command = NULL;
    }
    char *json = xmalloc(size + 1U);
    memcpy(json, data, size);
    json[size] = '\0';
    bool displayed = book_load_sensitive(json, (int)size, "Access management");
    access_code_clear(json, size);
    free(json);
    bool recovered_result = response.operation == CLIENT_ACCESS_ADMIN_RESULT &&
                            strcmp(last_result_target, recovery_request_id) == 0;
    if (recovery_for_selected_server() && (issued || recovered_result)) {
        if (displayed && response.terminal) {
            access_code_clear(recovery_request_id, sizeof(recovery_request_id));
            access_code_clear(recovery_server_id, sizeof(recovery_server_id));
            access_code_clear(recovery_token_id, sizeof(recovery_token_id));
        } else if (response.token_id[0] != '\0' || displayed) {
            snprintf(recovery_token_id, sizeof(recovery_token_id), "%s", response.token_id);
        }
    }
    access_code_clear(last_result_target, sizeof(last_result_target));
    return displayed;
}

void client_access_admin_update(void) {
    if (!request_outstanding || request_started_us == 0) {
        return;
    }
    uint64_t now = datetime_monotonic_us();
    if (now < request_started_us || now - request_started_us < ACCESS_ADMIN_TIMEOUT_US) {
        return;
    }
    remember_recovery();
    if (recovery_for_selected_server()) {
        draw_info_format(COLOR_RED,
                         "Access request timed out. Recover %s with /access result.",
                         recovery_request_id);
    } else {
        draw_info(COLOR_RED, "Access-management request timed out.");
    }
    request_outstanding = false;
    request_started_us = 0;
    access_code_clear(last_request_id, sizeof(last_request_id));
    access_code_clear(last_result_target, sizeof(last_result_target));
    if (pending_command != NULL) {
        access_code_clear(pending_command, strlen(pending_command));
        free(pending_command);
        pending_command = NULL;
    }
}

void client_access_admin_reset(void) {
    remember_recovery();
    access_code_clear(last_request_id, sizeof(last_request_id));
    access_code_clear(revision, sizeof(revision));
    last_operation = CLIENT_ACCESS_ADMIN_OPERATION_COUNT;
    request_outstanding = false;
    request_started_us = 0;
    access_code_clear(last_result_target, sizeof(last_result_target));
    if (pending_command != NULL) {
        access_code_clear(pending_command, strlen(pending_command));
        free(pending_command);
        pending_command = NULL;
    }
    book_sensitive_clear();
}
