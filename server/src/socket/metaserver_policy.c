/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <global.h>
#include <metaserver_internal.h>
#include <toolkit/string.h>
#include <openssl/crypto.h>
#include <ctype.h>
#include <string.h>

static void metaserver_json_skip_whitespace(const char **cursor, const char *end) {
    HARD_ASSERT(cursor != NULL);
    HARD_ASSERT(*cursor != NULL);
    HARD_ASSERT(end != NULL);

    while (*cursor < end &&
           (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' || **cursor == '\n')) {
        (*cursor)++;
    }
}

static bool metaserver_json_expect(const char **cursor, const char *end, char expected) {
    HARD_ASSERT(cursor != NULL);
    HARD_ASSERT(*cursor != NULL);
    HARD_ASSERT(end != NULL);

    metaserver_json_skip_whitespace(cursor, end);
    if (*cursor >= end || **cursor != expected) {
        return false;
    }
    (*cursor)++;
    return true;
}

static bool metaserver_json_string(const char **cursor,
                                   const char *end,
                                   char *value,
                                   size_t value_size,
                                   bool reject_escapes) {
    HARD_ASSERT(cursor != NULL);
    HARD_ASSERT(*cursor != NULL);
    HARD_ASSERT(end != NULL);

    metaserver_json_skip_whitespace(cursor, end);
    if (*cursor >= end || **cursor != '"') {
        return false;
    }
    (*cursor)++;

    size_t length = 0;
    while (*cursor < end) {
        unsigned char character = (unsigned char)**cursor;
        (*cursor)++;
        if (character == '"') {
            if (value != NULL) {
                if (length >= value_size) {
                    return false;
                }
                value[length] = '\0';
            }
            return true;
        }
        if (character < 0x20U) {
            return false;
        }
        if (character == '\\') {
            if (reject_escapes || *cursor >= end) {
                return false;
            }
            unsigned char escaped = (unsigned char)**cursor;
            (*cursor)++;
            if (escaped == 'u') {
                if ((size_t)(end - *cursor) < 4U) {
                    return false;
                }
                for (size_t index = 0; index < 4U; index++) {
                    if (!isxdigit((unsigned char)(*cursor)[index])) {
                        return false;
                    }
                }
                *cursor += 4;
            } else if (strchr("\"\\/bfnrt", escaped) == NULL) {
                return false;
            }
            continue;
        }
        if (value != NULL) {
            if (length + 1U >= value_size) {
                return false;
            }
            value[length++] = (char)character;
        }
    }
    return false;
}

static bool metaserver_publish_error_code_valid(const char *value, size_t value_size) {
    if (value == NULL || value_size == 0 || value_size > METASERVER_PUBLISH_ERROR_CODE_MAX) {
        return false;
    }
    for (size_t index = 0; index < value_size; index++) {
        unsigned char character = (unsigned char)value[index];
        bool ascii_alphanumeric = (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9');
        if (!(ascii_alphanumeric || character == '_' || character == '-' || character == '.')) {
            return false;
        }
    }
    return true;
}

static bool metaserver_publish_error_code_parse(const char *body,
                                                size_t body_size,
                                                char *error_code,
                                                size_t error_code_size) {
    if (body == NULL || body_size == 0 || body_size > METASERVER_PUBLISH_RESPONSE_BODY_MAX ||
        error_code == NULL || error_code_size == 0) {
        return false;
    }

    const char *cursor = body;
    const char *end = body + body_size;
    char key[32];
    char sequence[24];
    bool code_seen = false;
    bool message_seen = false;
    bool replay_sequence_seen = false;

    if (!metaserver_json_expect(&cursor, end, '{') ||
        !metaserver_json_string(&cursor, end, key, sizeof(key), true) ||
        strcmp(key, "error") != 0 || !metaserver_json_expect(&cursor, end, ':') ||
        !metaserver_json_expect(&cursor, end, '{')) {
        return false;
    }

    for (;;) {
        if (!metaserver_json_string(&cursor, end, key, sizeof(key), true) ||
            !metaserver_json_expect(&cursor, end, ':')) {
            return false;
        }
        if (strcmp(key, "code") == 0) {
            if (code_seen ||
                !metaserver_json_string(&cursor,
                                        end,
                                        error_code,
                                        error_code_size,
                                        true)) {
                return false;
            }
            code_seen = true;
        } else if (strcmp(key, "message") == 0) {
            if (message_seen || !metaserver_json_string(&cursor, end, NULL, 0, false)) {
                return false;
            }
            message_seen = true;
        } else if (strcmp(key, "minimumNextSequence") == 0) {
            if (replay_sequence_seen ||
                !metaserver_json_string(&cursor, end, sequence, sizeof(sequence), true)) {
                return false;
            }
            replay_sequence_seen = true;
        } else {
            return false;
        }

        metaserver_json_skip_whitespace(&cursor, end);
        if (cursor >= end) {
            return false;
        }
        if (*cursor == ',') {
            cursor++;
            continue;
        }
        if (*cursor != '}') {
            return false;
        }
        cursor++;
        break;
    }

    if (!metaserver_json_expect(&cursor, end, '}')) {
        return false;
    }
    metaserver_json_skip_whitespace(&cursor, end);
    if (cursor != end || !code_seen) {
        return false;
    }

    size_t code_size = strlen(error_code);
    if (!metaserver_publish_error_code_valid(error_code, code_size)) {
        return false;
    }
    if (!replay_sequence_seen) {
        return message_seen;
    }
    if (message_seen || strcmp(error_code, "publish_replay") != 0 || sequence[0] == '0') {
        return false;
    }
    uint64_t minimum_next_sequence;
    if (!string_parse_uint64(sequence, 10, 1, UINT64_MAX, &minimum_next_sequence)) {
        return false;
    }
    char canonical_sequence[24];
    return snprintf(VS(canonical_sequence), "%" PRIu64, minimum_next_sequence) ==
               (int)strlen(sequence) &&
           strcmp(canonical_sequence, sequence) == 0;
}

bool metaserver_publish_error_code(const char *body,
                                   size_t body_size,
                                   char *error_code,
                                   size_t error_code_size) {
    static const char fallback[] = "unavailable";

    if (error_code == NULL || error_code_size == 0) {
        return false;
    }
    if (error_code_size <= sizeof(fallback) - 1U) {
        error_code[0] = '\0';
        return false;
    }
    memcpy(error_code, fallback, sizeof(fallback));

    char parsed_code[METASERVER_PUBLISH_ERROR_CODE_MAX + 1U] = {0};
    if (!metaserver_publish_error_code_parse(
            body, body_size, parsed_code, sizeof(parsed_code))) {
        return false;
    }
    size_t parsed_size = strlen(parsed_code);
    if (parsed_size >= error_code_size) {
        return false;
    }
    memcpy(error_code, parsed_code, parsed_size + 1U);
    return true;
}

static bool
metaserver_retry_after_parse(const char *value, size_t value_size, uint32_t *retry_after_seconds) {
    HARD_ASSERT(value != NULL);
    HARD_ASSERT(retry_after_seconds != NULL);

    const char *end = value + value_size;
    while (value < end && (*value == ' ' || *value == '\t')) {
        value++;
    }
    while (end > value &&
           (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    if (value == end) {
        return false;
    }

    uint32_t parsed = 0;
    for (const char *cursor = value; cursor < end; cursor++) {
        if (!isdigit((unsigned char)*cursor)) {
            return false;
        }
        uint32_t digit = (uint32_t)(*cursor - '0');
        if (parsed > (METASERVER_RENDEZVOUS_RETRY_AFTER_MAX_SECONDS - digit) / 10U) {
            parsed = METASERVER_RENDEZVOUS_RETRY_AFTER_MAX_SECONDS;
            while (++cursor < end) {
                if (!isdigit((unsigned char)*cursor)) {
                    return false;
                }
            }
            break;
        }
        parsed = parsed * 10U + digit;
    }
    *retry_after_seconds = parsed;
    return true;
}

static server_monotonic_t metaserver_deadline_after(server_monotonic_t now,
                                                    server_duration_t duration) {
    server_monotonic_t deadline = {UINT64_MAX - now.microseconds < duration.microseconds
                                       ? UINT64_MAX
                                       : now.microseconds + duration.microseconds};
    return deadline;
}

static server_monotonic_t metaserver_deadline_after_seconds(server_monotonic_t now,
                                                            uint64_t seconds) {
    return metaserver_deadline_after(now, server_duration_from_seconds(seconds));
}

static server_monotonic_t metaserver_deadline_after_milliseconds(server_monotonic_t now,
                                                                 uint64_t milliseconds) {
    return metaserver_deadline_after(now, server_duration_from_milliseconds(milliseconds));
}

static server_monotonic_t metaserver_deadline_later(server_monotonic_t lhs,
                                                    server_monotonic_t rhs) {
    return server_monotonic_before(lhs, rhs) ? rhs : lhs;
}

static void metaserver_attempt_budget_refill(metaserver_attempt_budget_t *budget,
                                             server_monotonic_t now) {
    HARD_ASSERT(budget != NULL);

    if (!server_monotonic_reached(now, budget->refill_deadline)) {
        return;
    }
    if (budget->tokens >= METASERVER_ATTEMPT_RATE_CAPACITY) {
        budget->refill_deadline =
            metaserver_deadline_after_seconds(now, METASERVER_ATTEMPT_RATE_REFILL_SECONDS);
        return;
    }

    uint64_t interval_us =
        server_duration_from_seconds(METASERVER_ATTEMPT_RATE_REFILL_SECONDS).microseconds;
    uint64_t elapsed = now.microseconds - budget->refill_deadline.microseconds;
    uint64_t intervals = elapsed / interval_us + 1U;
    uint64_t available = METASERVER_ATTEMPT_RATE_CAPACITY - budget->tokens;
    budget->tokens += (uint32_t)MIN(intervals, available);
    if (budget->tokens == METASERVER_ATTEMPT_RATE_CAPACITY || intervals > available) {
        budget->refill_deadline =
            metaserver_deadline_after_seconds(now, METASERVER_ATTEMPT_RATE_REFILL_SECONDS);
    } else {
        budget->refill_deadline =
            metaserver_deadline_after_seconds(budget->refill_deadline,
                                              intervals * METASERVER_ATTEMPT_RATE_REFILL_SECONDS);
    }
}

size_t metaserver_rendezvous_header(char *data, size_t size, size_t count, void *user_data) {
    if (size != 0 && count > SIZE_MAX / size) {
        return 0;
    }
    size_t bytes = size * count;
    metaserver_rendezvous_headers_t *headers = user_data;
    if (headers == NULL || (bytes != 0 && data == NULL)) {
        return 0;
    }

    size_t parsed = rendezvous_websocket_protocol_header(data, size, count, &headers->protocol);
    if (parsed != bytes) {
        return parsed;
    }

    static const char status_prefix[] = "HTTP/";
    if (bytes >= sizeof(status_prefix) - 1U &&
        memcmp(data, status_prefix, sizeof(status_prefix) - 1U) == 0) {
        headers->retry_after_seconds = 0;
        headers->has_retry_after = false;
        return bytes;
    }

    static const char name[] = "Retry-After:";
    if (bytes < sizeof(name) - 1U || strncasecmp(data, name, sizeof(name) - 1U) != 0) {
        return bytes;
    }

    uint32_t retry_after_seconds;
    if (metaserver_retry_after_parse(data + sizeof(name) - 1U,
                                     bytes - (sizeof(name) - 1U),
                                     &retry_after_seconds) &&
        (!headers->has_retry_after || retry_after_seconds > headers->retry_after_seconds)) {
        headers->retry_after_seconds = retry_after_seconds;
        headers->has_retry_after = true;
    }
    return bytes;
}

bool metaserver_rendezvous_protocol_allows(const metaserver_rendezvous_headers_t *headers,
                                           bool authorization_required) {
    if (headers == NULL) {
        return false;
    }
    return authorization_required ? rendezvous_websocket_protocol_valid(&headers->protocol)
                                  : headers->protocol.echoes == 0 && !headers->protocol.invalid;
}

uint32_t metaserver_rendezvous_retry_delay_ms(uint32_t failures,
                                              uint32_t retry_after_seconds,
                                              uint32_t random_value) {
    uint64_t base = METASERVER_RENDEZVOUS_RETRY_BASE_MS;
    while (failures-- != 0 && base < METASERVER_RENDEZVOUS_RETRY_MAX_MS) {
        base = MIN(base * 2U, METASERVER_RENDEZVOUS_RETRY_MAX_MS);
    }

    uint32_t jitter = (uint32_t)(base / 4U);
    uint32_t jitter_range = jitter * 2U + 1U;
    uint32_t delay = MIN((uint32_t)base - jitter + random_value % jitter_range,
                         METASERVER_RENDEZVOUS_RETRY_MAX_MS);
    uint32_t bounded_retry_after =
        MIN(retry_after_seconds, METASERVER_RENDEZVOUS_RETRY_AFTER_MAX_SECONDS);
    uint32_t retry_after_ms = bounded_retry_after * 1000U;
    return MAX(delay, retry_after_ms);
}

uint32_t metaserver_rendezvous_retry_failures(uint32_t failures, uint64_t connected_ms) {
    return connected_ms >= METASERVER_RENDEZVOUS_STABLE_MS ? 0 : failures;
}

bool metaserver_rendezvous_generation_allows(uint64_t current_generation,
                                             uint64_t requested_generation,
                                             bool shutdown) {
    return !shutdown && current_generation == requested_generation;
}

bool metaserver_rendezvous_upgrade_retryable(CURLcode result, long http_code, bool protocol_valid) {
    if (http_code == 101) {
        return result == CURLE_OK && protocol_valid;
    }
    if (http_code >= 100 && http_code <= 599) {
        return http_code == 408 || http_code == 429 || (http_code >= 500 && http_code <= 599);
    }
    return result != CURLE_OK;
}

void metaserver_attempt_budget_init(metaserver_attempt_budget_t *budget, server_monotonic_t now) {
    HARD_ASSERT(budget != NULL);

    memset(budget, 0, sizeof(*budget));
    budget->tokens = METASERVER_ATTEMPT_RATE_CAPACITY;
    budget->refill_deadline =
        metaserver_deadline_after_seconds(now, METASERVER_ATTEMPT_RATE_REFILL_SECONDS);
}

bool metaserver_attempt_budget_consume(metaserver_attempt_budget_t *budget,
                                       server_monotonic_t now,
                                       uint32_t *wait_ms) {
    HARD_ASSERT(budget != NULL);
    HARD_ASSERT(wait_ms != NULL);

    metaserver_attempt_budget_refill(budget, now);
    if (budget->tokens != 0) {
        budget->tokens--;
        *wait_ms = 0;
        return true;
    }

    server_duration_t remaining = server_monotonic_difference(budget->refill_deadline, now);
    uint64_t milliseconds = remaining.microseconds / 1000U;
    if (remaining.microseconds % 1000U != 0) {
        milliseconds++;
    }
    *wait_ms = (uint32_t)MIN(milliseconds, UINT32_MAX);
    return false;
}

void metaserver_attempt_budget_reset(metaserver_attempt_budget_t *budget, server_monotonic_t now) {
    HARD_ASSERT(budget != NULL);

    budget->tokens = METASERVER_ATTEMPT_RATE_CAPACITY;
    budget->refill_deadline =
        metaserver_deadline_after_seconds(now, METASERVER_ATTEMPT_RATE_REFILL_SECONDS);
}

uint32_t metaserver_publish_heartbeat_delay_seconds(uint32_t heartbeat_seconds,
                                                    uint32_t random_value) {
    HARD_ASSERT(heartbeat_seconds >= METASERVER_PUBLISH_HEARTBEAT_MIN_SECONDS);
    HARD_ASSERT(heartbeat_seconds <= METASERVER_PUBLISH_HEARTBEAT_MAX_SECONDS);

    uint32_t jitter = MAX(heartbeat_seconds / 10U, 1U);
    uint32_t jitter_range = jitter * 2U + 1U;
    return heartbeat_seconds - jitter + random_value % jitter_range;
}

uint32_t metaserver_publish_retry_delay_ms(uint32_t failures,
                                           uint32_t retry_after_seconds,
                                           uint32_t random_value) {
    uint64_t base = METASERVER_PUBLISH_RETRY_BASE_MS;
    while (failures-- != 0 && base < METASERVER_PUBLISH_RETRY_MAX_MS) {
        base = MIN(base * 2U, METASERVER_PUBLISH_RETRY_MAX_MS);
    }

    uint32_t jitter = (uint32_t)(base / 4U);
    uint32_t jitter_range = jitter * 2U + 1U;
    uint32_t delay =
        MIN((uint32_t)base - jitter + random_value % jitter_range, METASERVER_PUBLISH_RETRY_MAX_MS);
    uint32_t bounded_retry_after =
        MIN(retry_after_seconds, METASERVER_PUBLISH_RETRY_AFTER_MAX_SECONDS);
    uint32_t retry_after_ms = bounded_retry_after * 1000U;
    return MAX(delay, retry_after_ms);
}

bool metaserver_publish_retry_after(const char *headers,
                                    size_t headers_size,
                                    uint32_t *retry_after_seconds) {
    HARD_ASSERT(retry_after_seconds != NULL);

    *retry_after_seconds = 0;
    if (headers == NULL) {
        return false;
    }

    bool found = false;
    const char *cursor = headers;
    const char *end = headers + headers_size;
    while (cursor < end) {
        const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        if (line_end == NULL) {
            line_end = end;
        } else {
            line_end++;
        }
        size_t line_size = (size_t)(line_end - cursor);
        static const char status_prefix[] = "HTTP/";
        if (line_size >= sizeof(status_prefix) - 1U &&
            memcmp(cursor, status_prefix, sizeof(status_prefix) - 1U) == 0) {
            found = false;
            *retry_after_seconds = 0;
        } else {
            static const char name[] = "Retry-After:";
            if (line_size >= sizeof(name) - 1U &&
                strncasecmp(cursor, name, sizeof(name) - 1U) == 0) {
                uint32_t parsed;
                if (metaserver_retry_after_parse(cursor + sizeof(name) - 1U,
                                                 line_size - (sizeof(name) - 1U),
                                                 &parsed) &&
                    (!found || parsed > *retry_after_seconds)) {
                    *retry_after_seconds = parsed;
                    found = true;
                }
            }
        }
        cursor = line_end;
    }
    return found;
}

bool metaserver_publish_response_retryable(curl_state_t state, int http_code) {
    if (http_code == 408 || http_code == 429 || (http_code >= 500 && http_code <= 599)) {
        return true;
    }
    if (http_code == 200 && state != CURL_STATE_OK) {
        return true;
    }
    return state != CURL_STATE_OK && (http_code < 100 || http_code > 599);
}

metaserver_publish_failure_action_t metaserver_publish_failure_action(curl_state_t state,
                                                                      int http_code) {
    if (http_code == 409) {
        return METASERVER_PUBLISH_FAILURE_REPLAY;
    }
    return metaserver_publish_response_retryable(state, http_code)
               ? METASERVER_PUBLISH_FAILURE_RETRY
               : METASERVER_PUBLISH_FAILURE_SUSPEND;
}

void metaserver_publish_cadence_init(metaserver_publish_cadence_t *cadence,
                                     server_monotonic_t now) {
    HARD_ASSERT(cadence != NULL);

    memset(cadence, 0, sizeof(*cadence));
    cadence->dirty = true;
    cadence->dirty_deadline = now;
    metaserver_attempt_budget_init(&cadence->rate_budget, now);
}

void metaserver_publish_cadence_changed(metaserver_publish_cadence_t *cadence,
                                        server_monotonic_t now,
                                        bool actual_change) {
    HARD_ASSERT(cadence != NULL);

    cadence->dirty = true;
    cadence->dirty_deadline =
        metaserver_deadline_after_seconds(now, METASERVER_PUBLISH_DEBOUNCE_SECONDS);
    if (actual_change) {
        cadence->suspended = false;
    }
}

bool metaserver_publish_cadence_needs_snapshot(metaserver_publish_cadence_t *cadence,
                                               server_monotonic_t now) {
    HARD_ASSERT(cadence != NULL);

    if (cadence->suspended) {
        return cadence->dirty;
    }
    metaserver_attempt_budget_refill(&cadence->rate_budget, now);
    if (cadence->rate_budget.tokens == 0 ||
        (server_monotonic_is_set(cadence->retry_deadline) &&
         !server_monotonic_reached(now, cadence->retry_deadline))) {
        return false;
    }
    return (cadence->dirty && server_monotonic_reached(now, cadence->dirty_deadline)) ||
           (cadence->published && server_monotonic_is_set(cadence->heartbeat_deadline) &&
            server_monotonic_reached(now, cadence->heartbeat_deadline));
}

bool metaserver_publish_cadence_due(metaserver_publish_cadence_t *cadence,
                                    server_monotonic_t now,
                                    bool snapshot_changed) {
    HARD_ASSERT(cadence != NULL);

    metaserver_attempt_budget_refill(&cadence->rate_budget, now);
    if (cadence->suspended || cadence->rate_budget.tokens == 0 ||
        (server_monotonic_is_set(cadence->retry_deadline) &&
         !server_monotonic_reached(now, cadence->retry_deadline))) {
        return false;
    }

    bool dirty_due = cadence->dirty && server_monotonic_reached(now, cadence->dirty_deadline);
    if (dirty_due && cadence->published && !snapshot_changed) {
        cadence->dirty = false;
        dirty_due = false;
    }
    bool heartbeat_due = cadence->published &&
                         server_monotonic_is_set(cadence->heartbeat_deadline) &&
                         server_monotonic_reached(now, cadence->heartbeat_deadline);
    return dirty_due || heartbeat_due;
}

bool metaserver_publish_cadence_attempted(metaserver_publish_cadence_t *cadence,
                                          server_monotonic_t now) {
    HARD_ASSERT(cadence != NULL);

    uint32_t wait_ms;
    bool consumed = metaserver_attempt_budget_consume(&cadence->rate_budget, now, &wait_ms);
    HARD_ASSERT(consumed && wait_ms == 0);
    if (!consumed || wait_ms != 0) {
        return false;
    }
    cadence->dirty = false;
    cadence->retry_deadline = (server_monotonic_t){0};
    return true;
}

void metaserver_publish_cadence_succeeded(metaserver_publish_cadence_t *cadence,
                                          server_monotonic_t now,
                                          bool heartbeat_enabled,
                                          uint32_t heartbeat_seconds,
                                          uint32_t random_value) {
    HARD_ASSERT(cadence != NULL);

    cadence->published = true;
    cadence->suspended = false;
    cadence->replay_recovered = false;
    cadence->failures = 0;
    cadence->retry_deadline = (server_monotonic_t){0};
    cadence->heartbeat_deadline =
        heartbeat_enabled
            ? metaserver_deadline_after_seconds(
                  now,
                  metaserver_publish_heartbeat_delay_seconds(heartbeat_seconds, random_value))
            : (server_monotonic_t){0};
}

void metaserver_publish_cadence_failed(metaserver_publish_cadence_t *cadence,
                                       server_monotonic_t now,
                                       uint32_t retry_after_seconds,
                                       uint32_t random_value) {
    HARD_ASSERT(cadence != NULL);

    uint32_t delay =
        metaserver_publish_retry_delay_ms(cadence->failures, retry_after_seconds, random_value);
    if (cadence->failures != UINT32_MAX) {
        cadence->failures++;
    }
    cadence->dirty = true;
    cadence->dirty_deadline = now;
    server_monotonic_t retry = metaserver_deadline_after_milliseconds(now, delay);
    cadence->retry_deadline = metaserver_deadline_later(cadence->retry_deadline, retry);
}

void metaserver_publish_cadence_suspend(metaserver_publish_cadence_t *cadence) {
    HARD_ASSERT(cadence != NULL);

    cadence->suspended = true;
    cadence->retry_deadline = (server_monotonic_t){0};
}

bool metaserver_publish_cadence_recover_replay(metaserver_publish_cadence_t *cadence) {
    HARD_ASSERT(cadence != NULL);

    if (cadence->replay_recovered) {
        return false;
    }
    cadence->replay_recovered = true;
    return true;
}

void metaserver_rendezvous_auth_clear(metaserver_rendezvous_auth_job_t *job) {
    if (job != NULL) {
        OPENSSL_cleanse(job, sizeof(*job));
    }
}

void metaserver_rendezvous_auth_expire(metaserver_rendezvous_auth_job_t *jobs,
                                       size_t capacity,
                                       uint64_t now_ms) {
    HARD_ASSERT(jobs != NULL || capacity == 0);

    for (size_t i = 0; i < capacity; i++) {
        if (jobs[i].active && now_ms >= jobs[i].deadline_ms) {
            metaserver_rendezvous_auth_clear(&jobs[i]);
        }
    }
}

metaserver_rendezvous_auth_claim_t
metaserver_rendezvous_auth_claim(metaserver_rendezvous_auth_job_t *jobs,
                                 size_t capacity,
                                 const char *ticket,
                                 uint64_t deadline_ms,
                                 metaserver_rendezvous_auth_job_t **claimed) {
    HARD_ASSERT(jobs != NULL || capacity == 0);
    HARD_ASSERT(claimed != NULL);

    *claimed = NULL;
    if (ticket == NULL || !string_is_hex_fixed(ticket, RENDEZVOUS_TICKET_HEX_SIZE, true)) {
        return METASERVER_RENDEZVOUS_AUTH_CLAIM_INVALID;
    }

    metaserver_rendezvous_auth_job_t *available = NULL;
    for (size_t i = 0; i < capacity; i++) {
        if (jobs[i].active && strcmp(jobs[i].ticket, ticket) == 0) {
            return METASERVER_RENDEZVOUS_AUTH_CLAIM_DUPLICATE;
        }
        if (!jobs[i].active && available == NULL) {
            available = &jobs[i];
        }
    }
    if (available == NULL) {
        return METASERVER_RENDEZVOUS_AUTH_CLAIM_FULL;
    }

    memset(available, 0, sizeof(*available));
    snprintf(VS(available->ticket), "%s", ticket);
    available->deadline_ms = deadline_ms;
    available->state = RENDEZVOUS_SERVER_AUTH_NEW;
    available->active = true;
    *claimed = available;
    return METASERVER_RENDEZVOUS_AUTH_CLAIM_OK;
}

metaserver_rendezvous_auth_job_t *
metaserver_rendezvous_auth_find(metaserver_rendezvous_auth_job_t *jobs,
                                size_t capacity,
                                const char *ticket,
                                rendezvous_server_auth_state_t state) {
    HARD_ASSERT(jobs != NULL || capacity == 0);

    if (ticket == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < capacity; i++) {
        if (jobs[i].active && jobs[i].state == state && strcmp(jobs[i].ticket, ticket) == 0) {
            return &jobs[i];
        }
    }
    return NULL;
}

bool metaserver_public_endpoint_from_config(const char *configured_host,
                                            uint16_t configured_port,
                                            char *published_host,
                                            size_t published_host_size,
                                            uint16_t *published_port) {
    HARD_ASSERT(configured_host != NULL);
    HARD_ASSERT(published_host != NULL);
    HARD_ASSERT(published_host_size != 0);
    HARD_ASSERT(published_port != NULL);

    *published_host = '\0';
    *published_port = configured_port;
    /* The legacy setting accepts only a raw IP address. Directory publication
     * now requires a separately defined, explicit DNS hostname contract, so a
     * legacy value must fail closed instead of reintroducing raw-IP
     * persistence. */
    (void)configured_host;
    return false;
}
