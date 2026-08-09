/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <toolkit/metaserver_publisher.h>
#include <toolkit/path.h>
#include <toolkit/string.h>

#define METASERVER_PUBLISH_SEQUENCE_FILE "metaserver-publish-sequence"

typedef struct metaserver_publish_sequence_slot {
    char path[HUGE_BUF];
    uint64_t value;
    bool exists;
} metaserver_publish_sequence_slot_t;

static bool metaserver_publish_sequence_read(const char *data_path,
                                             const char *server_id,
                                             metaserver_publish_sequence_slot_t slots[2],
                                             uint64_t *highwater) {
    if (!string_is_hex_fixed(server_id, 64, true)) {
        return false;
    }
    *highwater = 0;
    for (size_t i = 0; i < 2; i++) {
        if (snprintf(VS(slots[i].path),
                     "%s/%s-%s.%u",
                     data_path,
                     METASERVER_PUBLISH_SEQUENCE_FILE,
                     server_id,
                     (unsigned int)i) >= (int)sizeof(slots[i].path)) {
            return false;
        }
        char value[22];
        bool permissive = false;
        path_secret_error_t error = path_read_secret(slots[i].path, VS(value), &permissive);
        if (error == PATH_SECRET_NOT_FOUND) {
            continue;
        }
        char canonical[21];
        if (error != PATH_SECRET_OK || permissive ||
            !string_parse_uint64(value, 10, 1, UINT64_MAX, &slots[i].value) ||
            snprintf(VS(canonical), "%" PRIu64, slots[i].value) >= (int)sizeof(canonical) ||
            strcmp(value, canonical) != 0) {
            return false;
        }
        slots[i].exists = true;
        *highwater = MAX(*highwater, slots[i].value);
    }
    if (slots[0].exists && slots[1].exists && slots[0].value == slots[1].value) {
        return false;
    }
    return true;
}

static metaserver_publish_sequence_result_t
metaserver_publish_sequence_persist(const char *data_path,
                                    const char *server_id,
                                    uint64_t desired) {
    metaserver_publish_sequence_slot_t slots[2] = {0};
    uint64_t highwater;
    if (!metaserver_publish_sequence_read(data_path, server_id, slots, &highwater)) {
        return METASERVER_PUBLISH_SEQUENCE_ERROR;
    }
    if (desired <= highwater) {
        return METASERVER_PUBLISH_SEQUENCE_OK;
    }

    size_t target;
    if (!slots[0].exists) {
        target = 0;
    } else if (!slots[1].exists) {
        target = 1;
    } else {
        target = slots[0].value < slots[1].value ? 0 : 1;
        if (unlink(slots[target].path) != 0) {
            return METASERVER_PUBLISH_SEQUENCE_ERROR;
        }
        slots[target].exists = false;
    }

    char value[22];
    int length = snprintf(VS(value), "%" PRIu64 "\n", desired);
    if (length <= 0 || (size_t)length >= sizeof(value)) {
        return METASERVER_PUBLISH_SEQUENCE_ERROR;
    }
    path_secret_create_result_t created =
        path_secret_create_atomic(slots[target].path, value, (size_t)length);
    if (created != PATH_SECRET_CREATE_OK) {
        metaserver_publish_sequence_slot_t after[2] = {0};
        uint64_t after_highwater;
        if (!metaserver_publish_sequence_read(data_path, server_id, after, &after_highwater) ||
            after_highwater != desired) {
            return METASERVER_PUBLISH_SEQUENCE_ERROR;
        }
    }

    size_t previous = target == 0 ? 1 : 0;
    if (slots[previous].exists && slots[previous].value < desired) {
        (void)unlink(slots[previous].path);
    }
    return METASERVER_PUBLISH_SEQUENCE_OK;
}

metaserver_publish_sequence_result_t metaserver_publish_sequence_reserve(const char *data_path,
                                                                         const char *server_id,
                                                                         uint64_t minimum,
                                                                         uint64_t *sequence) {
    HARD_ASSERT(data_path != NULL);
    HARD_ASSERT(server_id != NULL);
    HARD_ASSERT(sequence != NULL);
    *sequence = 0;
    if (minimum == 0) {
        return METASERVER_PUBLISH_SEQUENCE_ERROR;
    }
    metaserver_publish_sequence_slot_t slots[2] = {0};
    uint64_t highwater;
    if (!metaserver_publish_sequence_read(data_path, server_id, slots, &highwater)) {
        return METASERVER_PUBLISH_SEQUENCE_ERROR;
    }
    if (highwater == UINT64_MAX) {
        return METASERVER_PUBLISH_SEQUENCE_EXHAUSTED;
    }
    uint64_t desired = MAX(highwater + 1U, minimum);
    metaserver_publish_sequence_result_t result =
        metaserver_publish_sequence_persist(data_path, server_id, desired);
    if (result == METASERVER_PUBLISH_SEQUENCE_OK) {
        *sequence = desired;
    }
    return result;
}

metaserver_publish_sequence_result_t
metaserver_publish_sequence_recover(const char *data_path,
                                    const char *server_id,
                                    uint64_t minimum_next_sequence) {
    HARD_ASSERT(data_path != NULL);
    HARD_ASSERT(server_id != NULL);
    if (minimum_next_sequence == 0) {
        return METASERVER_PUBLISH_SEQUENCE_ERROR;
    }
    metaserver_publish_sequence_slot_t slots[2] = {0};
    uint64_t highwater;
    if (!metaserver_publish_sequence_read(data_path, server_id, slots, &highwater)) {
        return METASERVER_PUBLISH_SEQUENCE_ERROR;
    }
    if (highwater == UINT64_MAX) {
        return METASERVER_PUBLISH_SEQUENCE_EXHAUSTED;
    }
    uint64_t required_highwater = minimum_next_sequence - 1U;
    return metaserver_publish_sequence_persist(data_path,
                                               server_id,
                                               MAX(highwater, required_highwater));
}

bool metaserver_publish_replay_parse(const char *body,
                                     size_t body_size,
                                     uint64_t *minimum_next_sequence) {
    HARD_ASSERT(minimum_next_sequence != NULL);
    *minimum_next_sequence = 0;
    static const char prefix[] =
        "{\"error\":{\"code\":\"publish_replay\",\"minimumNextSequence\":\"";
    static const char suffix[] = "\"}}";
    if (body == NULL || body_size <= sizeof(prefix) - 1U + sizeof(suffix) - 1U ||
        body_size >= 1024 || memcmp(body, prefix, sizeof(prefix) - 1U) != 0 ||
        memcmp(body + body_size - (sizeof(suffix) - 1U), suffix, sizeof(suffix) - 1U) != 0) {
        return false;
    }
    const char *digits = body + sizeof(prefix) - 1U;
    size_t digits_size = body_size - (sizeof(prefix) - 1U) - (sizeof(suffix) - 1U);
    if (digits_size == 0 || digits_size > 20 || digits[0] == '0') {
        return false;
    }
    char value[21];
    memcpy(value, digits, digits_size);
    value[digits_size] = '\0';
    uint64_t parsed;
    if (!string_parse_uint64(value, 10, 1, UINT64_MAX, &parsed)) {
        return false;
    }
    char canonical[21];
    if (snprintf(VS(canonical), "%" PRIu64, parsed) != (int)digits_size ||
        memcmp(canonical, digits, digits_size) != 0) {
        return false;
    }
    *minimum_next_sequence = parsed;
    return true;
}
