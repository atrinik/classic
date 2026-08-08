/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
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

metaserver_registration_key_action_t
metaserver_registration_key_action(curl_state_t state, int http_code, bool registration) {
    if (registration && state == CURL_STATE_OK && http_code == 401) {
        return METASERVER_REGISTRATION_KEY_DELETE;
    }
    if (registration && (state != CURL_STATE_OK || http_code >= 500)) {
        return METASERVER_REGISTRATION_KEY_RETRY_ESTABLISHED;
    }
    if (!registration && state == CURL_STATE_OK && http_code == 409) {
        return METASERVER_REGISTRATION_KEY_RETRY_REGISTRATION;
    }
    return METASERVER_REGISTRATION_KEY_KEEP;
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
