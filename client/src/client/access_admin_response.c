/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 ************************************************************************/

#include <access_admin_response.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACCESS_ADMIN_RESPONSE_MAX 32768U
#define ACCESS_ADMIN_TOKEN_LIMIT 1024U
#define ACCESS_ADMIN_PAGE_LIMIT 64U
#define ACCESS_ADMIN_HISTORY_LIMIT 16U
#define ACCESS_ADMIN_EXPIRES_MAX UINT64_C(253402300799)

typedef struct json_parser {
    const uint8_t *position;
    const uint8_t *end;
    char *output;
    size_t capacity;
    size_t used;
    bool overflow;
} json_parser_t;

static const char *const operation_names[] = {
    "issue",
    "list",
    "history",
    "revoke",
    "remove",
    "status",
    "result",
};

static const char *const outcome_names[] = {
    "committed",
    "locally_revoked_route_pending",
    "pending",
    "conflict",
    "denied",
    "limit",
    "invalid",
    "not_found",
    "unavailable",
    "save_failed",
    "indeterminate",
    "already_committed_secret_unavailable",
};

static void memory_clear(void *memory, size_t size) {
    volatile uint8_t *bytes = memory;
    while (size-- != 0U) {
        *bytes++ = 0;
    }
}

const char *client_access_admin_operation_name(client_access_admin_operation_t operation) {
    return operation < CLIENT_ACCESS_ADMIN_OPERATION_COUNT ? operation_names[operation] : NULL;
}

/* Output is literal text for the private book, whose renderer disables markup.
 * Never use this formatter's output with a markup-enabled rendering entry point. */
static void text_append(json_parser_t *parser, const char *text) {
    if (parser->output == NULL || parser->overflow) {
        return;
    }
    size_t size = strlen(text);
    if (size >= parser->capacity - parser->used) {
        parser->overflow = true;
        return;
    }
    memcpy(parser->output + parser->used, text, size + 1U);
    parser->used += size;
}

static void text_field(json_parser_t *parser, const char *label, const char *value) {
    text_append(parser, label);
    text_append(parser, ": ");
    text_append(parser, value);
    text_append(parser, "\n");
}

static void text_timestamp(json_parser_t *parser, const char *label, const char *value) {
    if (parser->output == NULL) {
        return;
    }
    /* The strict parser bounds timestamps to 9999-12-31. Convert civil days
     * directly instead of relying on platform time_t range or local timezone. */
    uint64_t seconds = strtoull(value, NULL, 10);
    uint64_t days = seconds / 86400U + 719468U;
    uint64_t era = days / 146097U;
    unsigned int day_of_era = (unsigned int)(days % 146097U);
    unsigned int year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    unsigned int year = year_of_era + (unsigned int)era * 400U;
    unsigned int day_of_year = day_of_era -
        (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    unsigned int month_index = (5U * day_of_year + 2U) / 153U;
    unsigned int day = day_of_year - (153U * month_index + 2U) / 5U + 1U;
    unsigned int month = month_index < 10U ? month_index + 3U : month_index - 9U;
    year += month <= 2U;
    char date[32];
    snprintf(date, sizeof(date), "%04u-%02u-%02u %02u:%02u:%02u UTC",
             year, month, day, (unsigned int)(seconds % 86400U / 3600U),
             (unsigned int)(seconds % 3600U / 60U), (unsigned int)(seconds % 60U));
    text_field(parser, label, date);
}

static void text_named_string(json_parser_t *parser, const char *name, const char *value) {
    static const struct { const char *key; const char *label; } fields[] = {
        {"requestId", "Request ID"}, {"tokenId", "Token ID"},
        {"tokenRevision", "Token revision"},
        {"label", "Label"}, {"serverIdentity", "Server identity"},
        {"policy", "Admission policy"}, {"integrity", "Store integrity"},
        {"durability", "Store durability"}, {"state", "State"}, {"code", "Access code"},
    };
    if (strcmp(name, "createdAt") == 0) {
        text_timestamp(parser, "Created", value);
        return;
    }
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (strcmp(name, fields[i].key) == 0) {
            const char *description = value;
            if (strcmp(name, "state") == 0 && strcmp(value, "absent_open") == 0) {
                description = "Access store not initialized; admission is open";
            } else if (strcmp(name, "policy") == 0) {
                description = strcmp(value, "open") == 0 ? "Open (no access code required)"
                                                        : "Protected (access code required)";
            } else if (strcmp(name, "durability") == 0 && strcmp(value, "indeterminate") == 0) {
                description = "Uncertain; recover outstanding requests before retrying";
            }
            text_field(parser, fields[i].label, *description != '\0' ? description : "None");
            if (strcmp(name, "code") == 0) {
                text_append(parser, "One-time display: copy this code now. It cannot be retrieved again.\n");
            }
            return;
        }
    }
}

static bool json_character(json_parser_t *parser, uint8_t character) {
    if (parser->position == parser->end || *parser->position != character) {
        return false;
    }
    parser->position++;
    return true;
}

static bool json_literal(json_parser_t *parser, const char *literal) {
    size_t size = strlen(literal);
    if ((size_t)(parser->end - parser->position) < size ||
        memcmp(parser->position, literal, size) != 0) {
        return false;
    }
    parser->position += size;
    return true;
}

static bool json_hex4(json_parser_t *parser, uint32_t *value) {
    if ((size_t)(parser->end - parser->position) < 4U) {
        return false;
    }
    uint32_t result = 0;
    for (unsigned int i = 0; i < 4U; i++) {
        uint8_t character = *parser->position++;
        unsigned int digit;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10U;
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10U;
        } else {
            return false;
        }
        result = (result << 4U) | digit;
    }
    *value = result;
    return true;
}

static bool utf8_valid(const uint8_t *text) {
    while (*text != '\0') {
        uint32_t codepoint = *text++;
        unsigned int remaining = 0;
        uint32_t minimum = 0;
        if (codepoint >= 0xc2U && codepoint <= 0xdfU) {
            codepoint &= 0x1fU;
            remaining = 1;
            minimum = 0x80U;
        } else if (codepoint >= 0xe0U && codepoint <= 0xefU) {
            codepoint &= 0x0fU;
            remaining = 2;
            minimum = 0x800U;
        } else if (codepoint >= 0xf0U && codepoint <= 0xf4U) {
            codepoint &= 0x07U;
            remaining = 3;
            minimum = 0x10000U;
        } else if (codepoint >= 0x80U) {
            return false;
        }
        while (remaining-- != 0U) {
            if ((*text & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (*text++ & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint < 0x20U ||
            (codepoint >= 0x7fU && codepoint <= 0x9fU)) {
            return false;
        }
    }
    return true;
}

static bool json_string(json_parser_t *parser, char *output, size_t capacity) {
    if (capacity == 0 || !json_character(parser, '"')) {
        return false;
    }
    size_t used = 0;
    while (parser->position < parser->end) {
        uint32_t codepoint = *parser->position++;
        uint8_t encoded[4];
        size_t count = 1;
        encoded[0] = (uint8_t)codepoint;
        if (codepoint == '"') {
            output[used] = '\0';
            return utf8_valid((const uint8_t *)output);
        }
        if (codepoint < 0x20U) {
            return false;
        }
        if (codepoint == '\\') {
            if (parser->position == parser->end) {
                return false;
            }
            codepoint = *parser->position++;
            if (codepoint == '"' || codepoint == '\\' || codepoint == '/') {
                encoded[0] = (uint8_t)codepoint;
            } else if (codepoint == 'u') {
                if (!json_hex4(parser, &codepoint)) {
                    return false;
                }
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    uint32_t low;
                    if ((size_t)(parser->end - parser->position) < 2U ||
                        parser->position[0] != '\\' || parser->position[1] != 'u') {
                        return false;
                    }
                    parser->position += 2;
                    if (!json_hex4(parser, &low) || low < 0xdc00U || low > 0xdfffU) {
                        return false;
                    }
                    codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + low - 0xdc00U;
                }
                if (codepoint == 0 || (codepoint >= 0xdc00U && codepoint <= 0xdfffU)) {
                    return false;
                }
                if (codepoint < 0x80U) {
                    encoded[0] = (uint8_t)codepoint;
                } else if (codepoint < 0x800U) {
                    count = 2;
                    encoded[0] = 0xc0U | (uint8_t)(codepoint >> 6U);
                    encoded[1] = 0x80U | (uint8_t)(codepoint & 0x3fU);
                } else if (codepoint < 0x10000U) {
                    count = 3;
                    encoded[0] = 0xe0U | (uint8_t)(codepoint >> 12U);
                    encoded[1] = 0x80U | (uint8_t)((codepoint >> 6U) & 0x3fU);
                    encoded[2] = 0x80U | (uint8_t)(codepoint & 0x3fU);
                } else {
                    count = 4;
                    encoded[0] = 0xf0U | (uint8_t)(codepoint >> 18U);
                    encoded[1] = 0x80U | (uint8_t)((codepoint >> 12U) & 0x3fU);
                    encoded[2] = 0x80U | (uint8_t)((codepoint >> 6U) & 0x3fU);
                    encoded[3] = 0x80U | (uint8_t)(codepoint & 0x3fU);
                }
            } else {
                return false;
            }
        }
        if (count >= capacity - used) {
            return false;
        }
        memcpy(output + used, encoded, count);
        used += count;
    }
    return false;
}

static bool
json_named_string(json_parser_t *parser, const char *name, char *output, size_t capacity) {
    char key[32];
    bool valid = json_string(parser, key, sizeof(key)) && strcmp(key, name) == 0 &&
                 json_character(parser, ':') && json_string(parser, output, capacity);
    if (valid) {
        text_named_string(parser, name, output);
    }
    return valid;
}

static bool fixed_hex(const char *text, size_t size) {
    if (strlen(text) != size) {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        if (!isdigit((unsigned char)text[i]) && !(text[i] >= 'a' && text[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool decimal_value(const char *text, uint64_t maximum, bool positive) {
    if (*text == '\0' || (text[0] == '0' && text[1] != '\0')) {
        return false;
    }
    uint64_t value = 0;
    for (; *text != '\0'; text++) {
        if (!isdigit((unsigned char)*text)) {
            return false;
        }
        uint64_t digit = (uint64_t)(*text - '0');
        if (value > (maximum - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    return !positive || value != 0;
}

static bool json_decimal_string(json_parser_t *parser,
                                char *output,
                                size_t capacity,
                                uint64_t maximum,
                                bool positive) {
    return json_string(parser, output, capacity) && decimal_value(output, maximum, positive);
}

static bool json_boolean(json_parser_t *parser) {
    bool pending = json_literal(parser, "true");
    if (!pending && !json_literal(parser, "false")) {
        return false;
    }
    text_field(parser, "Route synchronization", pending ? "Pending" : "Complete");
    return true;
}

static bool json_empty_object(json_parser_t *parser) {
    return json_literal(parser, "{}");
}

static bool json_nullable_timestamp(json_parser_t *parser, const char *label, const char *absent) {
    if (json_literal(parser, "null")) {
        text_field(parser, label, absent);
        return true;
    }
    char value[13];
    if (!json_decimal_string(parser, value, sizeof(value), ACCESS_ADMIN_EXPIRES_MAX, true)) {
        return false;
    }
    text_timestamp(parser, label, value);
    return true;
}

static bool json_token(json_parser_t *parser, bool history) {
    text_append(parser, "\n");
    char text[129];
    if (!json_character(parser, '{') || !json_named_string(parser, "tokenId", text, sizeof(text)) ||
        !fixed_hex(text, 32) || !json_character(parser, ',') ||
        !json_named_string(parser, "revision", text, sizeof(text)) ||
        !decimal_value(text, UINT64_MAX, true)) {
        return false;
    }
    text_field(parser, "Token revision", text);
    if (!json_character(parser, ',') ||
        !json_named_string(parser, "label", text, sizeof(text)) || *text == '\0' ||
        !json_character(parser, ',') ||
        !json_named_string(parser, "createdAt", text, sizeof(text)) ||
        !decimal_value(text, ACCESS_ADMIN_EXPIRES_MAX, true) || !json_character(parser, ',')) {
        return false;
    }
    char key[32];
    if (!json_string(parser, key, sizeof(key)) || strcmp(key, "expiresAt") != 0 ||
        !json_character(parser, ':') || !json_nullable_timestamp(parser, "Expires", "Never") ||
        !json_character(parser, ',') || !json_named_string(parser, "state", text, sizeof(text)) ||
        (strcmp(text, "pending") != 0 && strcmp(text, "active") != 0 &&
         strcmp(text, "revoked") != 0 && strcmp(text, "expired") != 0) ||
        !json_character(parser, ',') || !json_string(parser, key, sizeof(key)) ||
        strcmp(key, "routePending") != 0 || !json_character(parser, ':') || !json_boolean(parser) ||
        !json_character(parser, ',') || !json_string(parser, key, sizeof(key)) ||
        strcmp(key, "lastAdmittedAt") != 0 || !json_character(parser, ':') ||
        !json_nullable_timestamp(parser, "Last admitted", "Never")) {
        return false;
    }
    if (history) {
        if (!json_character(parser, ',') || !json_string(parser, key, sizeof(key)) ||
            strcmp(key, "history") != 0 || !json_character(parser, ':') ||
            !json_character(parser, '[')) {
            return false;
        }
        text_append(parser, "Admission history (oldest first):\n");
        size_t count = 0;
        if (!json_character(parser, ']')) {
            do {
                if (count++ >= ACCESS_ADMIN_HISTORY_LIMIT ||
                    !json_decimal_string(parser,
                                         text,
                                         sizeof(text),
                                         ACCESS_ADMIN_EXPIRES_MAX,
                                         true)) {
                    return false;
                }
                text_timestamp(parser, "  Admitted", text);
                if (json_character(parser, ']')) {
                    break;
                }
            } while (json_character(parser, ','));
            if (parser->position[-1] != ']') {
                return false;
            }
        }
        if (count == 0U) {
            text_append(parser, "  No admissions recorded.\n");
        }
    }
    return json_character(parser, '}');
}

static bool json_list_result(json_parser_t *parser) {
    char key[32];
    if (!json_character(parser, '{') || !json_string(parser, key, sizeof(key)) ||
        strcmp(key, "tokens") != 0 || !json_character(parser, ':') ||
        !json_character(parser, '[')) {
        return false;
    }
    size_t count = 0;
    if (!json_character(parser, ']')) {
        do {
            if (count++ >= ACCESS_ADMIN_PAGE_LIMIT || !json_token(parser, false)) {
                return false;
            }
            if (json_character(parser, ']')) {
                break;
            }
        } while (json_character(parser, ','));
        if (parser->position[-1] != ']') {
            return false;
        }
    }
    if (!json_character(parser, ',') || !json_string(parser, key, sizeof(key)) ||
        strcmp(key, "cursor") != 0 || !json_character(parser, ':')) {
        return false;
    }
    if (count == 0U) {
        text_append(parser, "No access tokens on this page.\n");
    }
    if (!json_literal(parser, "null")) {
        char cursor[21];
        if (!json_decimal_string(parser, cursor, sizeof(cursor), ACCESS_ADMIN_TOKEN_LIMIT, true)) {
            return false;
        }
        text_append(parser, "\nNext page: /access list --cursor ");
        text_append(parser, cursor);
        text_append(parser, "\n");
    } else {
        text_append(parser, "\nEnd of token list.\n");
    }
    return json_character(parser, '}');
}

static bool json_mutation_result(json_parser_t *parser,
                                 client_access_admin_operation_t operation,
                                 bool committed,
                                 char token_id[33]) {
    char value[65];
    char key[32];
    if (!json_character(parser, '{') ||
        !json_named_string(parser, "tokenId", token_id, 33) ||
        (*token_id != '\0' && !fixed_hex(token_id, 32)) || !json_character(parser, ',') ||
        !json_named_string(parser, "tokenRevision", value, sizeof(value)) ||
        !decimal_value(value, UINT64_MAX, false) || !json_character(parser, ',') ||
        !json_string(parser, key, sizeof(key)) || strcmp(key, "routePending") != 0 ||
        !json_character(parser, ':') || !json_boolean(parser)) {
        return false;
    }
    if (operation == CLIENT_ACCESS_ADMIN_ISSUE && committed) {
        if (!json_character(parser, ',') ||
            !json_named_string(parser, "code", value, sizeof(value)) || strlen(value) != 16U) {
            memory_clear(value, sizeof(value));
            return false;
        }
        static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        if (strspn(value, alphabet) != 16U) {
            memory_clear(value, sizeof(value));
            return false;
        }
    }
    bool valid = json_character(parser, '}');
    memory_clear(value, sizeof(value));
    return valid;
}

static bool
json_status_result(json_parser_t *parser, bool revision_present, const char *outer_revision) {
    char value[65];
    if (!json_character(parser, '{') || !json_named_string(parser, "state", value, sizeof(value))) {
        return false;
    }
    bool absent = strcmp(value, "absent_open") == 0;
    if ((!absent && strcmp(value, "initialized") != 0) || absent == revision_present ||
        !json_character(parser, ',')) {
        return false;
    }
    char key[32];
    if (!json_string(parser, key, sizeof(key)) || strcmp(key, "schemaVersion") != 0 ||
        !json_character(parser, ':') || !json_literal(parser, "1") ||
        !json_character(parser, ',') ||
        !json_named_string(parser, "serverIdentity", value, sizeof(value)) ||
        !fixed_hex(value, 64) || !json_character(parser, ',') ||
        !json_named_string(parser, "policy", value, sizeof(value)) ||
        (strcmp(value, "open") != 0 && strcmp(value, "protected") != 0)) {
        return false;
    }
    if (absent) {
        return strcmp(value, "open") == 0 && json_character(parser, '}');
    }
    if (!json_character(parser, ',') ||
        !json_named_string(parser, "integrity", value, sizeof(value)) ||
        (strcmp(value, "ok") != 0 && strcmp(value, "failed") != 0) ||
        !json_character(parser, ',') ||
        !json_named_string(parser, "durability", value, sizeof(value)) ||
        (strcmp(value, "ok") != 0 && strcmp(value, "indeterminate") != 0) ||
        !json_character(parser, ',') ||
        !json_named_string(parser, "revision", value, sizeof(value)) ||
        !decimal_value(value, UINT64_MAX, false) || strcmp(value, outer_revision) != 0 ||
        !json_character(parser, ',') || !json_string(parser, key, sizeof(key)) ||
        strcmp(key, "pendingRouteSync") != 0 || !json_character(parser, ':')) {
        return false;
    }
    uint64_t pending = 0;
    size_t digits = 0;
    const uint8_t *digits_start = parser->position;
    while (parser->position < parser->end && isdigit(*parser->position)) {
        uint64_t digit = (uint64_t)(*parser->position - '0');
        if (pending > (UINT64_C(1024) - digit) / 10U) {
            return false;
        }
        pending = pending * 10U + digit;
        parser->position++;
        digits++;
    }
    char pending_text[32];
    snprintf(pending_text, sizeof(pending_text), "%llu", (unsigned long long)pending);
    text_field(parser, "Routes awaiting synchronization", pending_text);
    return digits != 0U && (digits == 1U || *digits_start != '0') && pending <= 1024U &&
           json_character(parser, '}');
}

static bool json_result(json_parser_t *parser,
                        client_access_admin_operation_t operation,
                        bool committed,
                        bool revision_present,
                        const char *revision,
                        char token_id[33]) {
    if (!revision_present) {
        if (operation == CLIENT_ACCESS_ADMIN_STATUS && committed) {
            return json_status_result(parser, false, revision);
        }
        return !committed && json_empty_object(parser);
    }
    switch (operation) {
        case CLIENT_ACCESS_ADMIN_STATUS:
            return committed && json_status_result(parser, true, revision);
        case CLIENT_ACCESS_ADMIN_LIST:
            return json_list_result(parser);
        case CLIENT_ACCESS_ADMIN_HISTORY:
            return committed ? json_token(parser, true) : json_empty_object(parser);
        case CLIENT_ACCESS_ADMIN_ISSUE:
        case CLIENT_ACCESS_ADMIN_REVOKE:
        case CLIENT_ACCESS_ADMIN_REMOVE:
        case CLIENT_ACCESS_ADMIN_RESULT:
            return json_mutation_result(parser, operation, committed, token_id);
        default:
            return false;
    }
}

static bool response_parse(const uint8_t *data,
                           size_t size,
                           client_access_admin_response_t *response,
                           char *output,
                           size_t capacity) {
    if (data == NULL || response == NULL || size == 0 || size > ACCESS_ADMIN_RESPONSE_MAX ||
        memchr(data, '\0', size) != NULL) {
        return false;
    }
    json_parser_t parser = {.position = data, .end = data + size,
                            .output = output, .capacity = capacity};
    char value[96];
    memset(response, 0, sizeof(*response));
    if (!json_character(&parser, '{') ||
        !json_named_string(&parser, "schema", value, sizeof(value)) ||
        strcmp(value, "atrinik-access-admin-v1") != 0 || !json_character(&parser, ',') ||
        !json_named_string(&parser, "operation", value, sizeof(value))) {
        return false;
    }
    for (response->operation = 0; response->operation < CLIENT_ACCESS_ADMIN_OPERATION_COUNT;
         response->operation++) {
        if (strcmp(value, operation_names[response->operation]) == 0) {
            break;
        }
    }
    if (response->operation == CLIENT_ACCESS_ADMIN_OPERATION_COUNT ||
        !json_character(&parser, ',') ||
        !json_named_string(&parser,
                           "requestId",
                           response->request_id,
                           sizeof(response->request_id)) ||
        !fixed_hex(response->request_id, 32) || !json_character(&parser, ',') ||
        !json_named_string(&parser, "outcome", value, sizeof(value))) {
        return false;
    }
    static const char *const operation_titles[] = {
        "Issue access code", "List access tokens", "Access token history", "Revoke access token",
        "Remove access token", "Access management status", "Recover request result",
    };
    text_field(&parser, "Operation", operation_titles[response->operation]);
    static const char *const outcome_descriptions[] = {
        "Completed successfully.",
        "Locally revoked; route synchronization is still pending.",
        "Still pending. Recover this request before another mutation.",
        "Revision conflict. Refresh with /access status or /access list before retrying.",
        "Permission denied.", "Access token limit reached.", "Invalid request.",
        "Token or request not found.", "Access management is unavailable.",
        "Could not save the access store.",
        "Result is uncertain. Recover this request before retrying.",
        "Already completed; the one-time access code cannot be retrieved again.",
    };
    bool known_outcome = false;
    for (size_t i = 0; i < sizeof(outcome_names) / sizeof(outcome_names[0]); i++) {
        if (strcmp(value, outcome_names[i]) == 0) {
            known_outcome = true;
            text_field(&parser, "Outcome", outcome_descriptions[i]);
            break;
        }
    }
    response->committed = strcmp(value, "committed") == 0;
    response->terminal = strcmp(value, "pending") != 0 && strcmp(value, "indeterminate") != 0;
    char key[32];
    if (!known_outcome || !json_character(&parser, ',') ||
        !json_string(&parser, key, sizeof(key)) || strcmp(key, "revision") != 0 ||
        !json_character(&parser, ':')) {
        return false;
    }
    if (json_literal(&parser, "null")) {
        response->revision_present = false;
    } else if (!json_decimal_string(&parser,
                                    response->revision,
                                    sizeof(response->revision),
                                    UINT64_MAX,
                                    false)) {
        return false;
    } else {
        response->revision_present = true;
    }
    if (response->revision_present) {
        text_field(&parser, "Store revision", response->revision);
    }
    if (!response->terminal && response->operation != CLIENT_ACCESS_ADMIN_RESULT) {
        text_append(&parser, "Recovery command: /access result ");
        text_append(&parser, response->request_id);
        text_append(&parser, "\n");
    }
    return json_character(&parser, ',') && json_string(&parser, key, sizeof(key)) &&
           strcmp(key, "result") == 0 && json_character(&parser, ':') &&
           json_result(&parser,
                       response->operation,
                       response->committed,
                       response->revision_present,
                       response->revision,
                       response->token_id) &&
           json_character(&parser, '}') && parser.position == parser.end && !parser.overflow;
}

bool client_access_admin_response_parse(const uint8_t *data,
                                        size_t size,
                                        client_access_admin_response_t *response) {
    return response_parse(data, size, response, NULL, 0);
}

bool client_access_admin_response_format(const uint8_t *data,
                                         size_t size,
                                         char *output,
                                         size_t capacity) {
    if (output == NULL || capacity == 0) {
        return false;
    }
    output[0] = '\0';
    client_access_admin_response_t response;
    bool valid = response_parse(data, size, &response, output, capacity);
    memory_clear(&response, sizeof(response));
    if (!valid) {
        memory_clear(output, capacity);
    }
    return valid;
}
