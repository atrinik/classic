#include <player_status.h>
#include <toolkit/packet.h>

player_status_model_t player_status_model;

void player_status_model_clear(player_status_model_t *model) {
    HARD_ASSERT(model != NULL);

    player_status_t *status = model->head;
    while (status != NULL) {
        player_status_t *next = status->next;
        free(status);
        status = next;
    }
    *model = (player_status_model_t){0};
}

const player_status_t *player_status_model_find(const player_status_model_t *model,
                                                const char *key) {
    HARD_ASSERT(model != NULL);
    HARD_ASSERT(key != NULL);

    for (player_status_t *status = model->head; status != NULL; status = status->next) {
        if (strcmp(status->key, key) == 0) {
            return status;
        }
    }
    return NULL;
}

static bool player_status_model_upsert(player_status_model_t *model,
                                       const player_status_t *replacement) {
    player_status_t **link = &model->head;
    while (*link != NULL && strcmp((*link)->key, replacement->key) != 0) {
        link = &(*link)->next;
    }

    if (*link != NULL) {
        player_status_t *next = (*link)->next;
        **link = *replacement;
        (*link)->next = next;
        return true;
    }
    if (model->count >= ATRINIK_PLAYER_STATUS_MAX_STATUSES) {
        return false;
    }

    player_status_t *status = malloc(sizeof(*status));
    if (status == NULL) {
        return false;
    }
    *status = *replacement;
    status->next = NULL;
    *link = status;
    model->count++;
    return true;
}

static void player_status_model_remove(player_status_model_t *model, const char *key) {
    player_status_t **link = &model->head;
    while (*link != NULL) {
        if (strcmp((*link)->key, key) == 0) {
            player_status_t *removed = *link;
            *link = removed->next;
            free(removed);
            model->count--;
            return;
        }
        link = &(*link)->next;
    }
}

bool player_status_model_tick(player_status_model_t *model, uint32_t seconds) {
    HARD_ASSERT(model != NULL);

    bool changed = false;
    for (player_status_t *status = model->head; status != NULL; status = status->next) {
        if (status->seconds > 0) {
            status->seconds =
                seconds >= (uint32_t)status->seconds ? 0 : status->seconds - (int32_t)seconds;
            changed = true;
        }
    }
    return changed;
}

static bool player_status_parse_entry(packet_reader_t *reader, player_status_t *status) {
    *status = (player_status_t){0};
    packet_reader_read_string_bounded(reader,
                                      status->key,
                                      sizeof(status->key),
                                      ATRINIK_PLAYER_STATUS_KEY_SIZE);
    status->face = packet_reader_read_uint16(reader);
    packet_reader_read_string_bounded(reader,
                                      status->name,
                                      sizeof(status->name),
                                      ATRINIK_PLAYER_STATUS_NAME_SIZE);
    packet_reader_read_string_bounded(reader,
                                      status->tooltip,
                                      sizeof(status->tooltip),
                                      ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE);
    status->seconds = packet_reader_read_int32(reader);
    if (packet_reader_error(reader) == PACKET_ERROR_NONE &&
        (status->key[0] == '\0' || status->name[0] == '\0' || status->seconds < -1)) {
        packet_reader_set_error(reader, PACKET_ERROR_INVALID_ENCODING);
    }
    return packet_reader_error(reader) == PACKET_ERROR_NONE;
}

bool player_status_parse_command(player_status_model_t *model,
                                 const uint8_t *data,
                                 size_t len,
                                 size_t pos) {
    HARD_ASSERT(model != NULL);

    packet_reader_t reader;
    packet_reader_init_at(&reader, data, len, pos);
    uint8_t operation = packet_reader_read_uint8(&reader);

    if (operation == PLAYER_STATUS_SNAPSHOT) {
        size_t count;
        if (!packet_reader_read_count16(&reader, ATRINIK_PLAYER_STATUS_MAX_STATUSES, &count)) {
            return false;
        }

        player_status_model_t replacement = {0};
        for (size_t i = 0; i < count; i++) {
            player_status_t status;
            if (!player_status_parse_entry(&reader, &status)) {
                player_status_model_clear(&replacement);
                return false;
            }
            if (player_status_model_find(&replacement, status.key) != NULL) {
                packet_reader_set_error(&reader, PACKET_ERROR_INVALID_ENCODING);
                player_status_model_clear(&replacement);
                return false;
            }
            if (!player_status_model_upsert(&replacement, &status)) {
                packet_reader_set_error(&reader, PACKET_ERROR_ALLOCATION);
                player_status_model_clear(&replacement);
                return false;
            }
        }
        if (!packet_reader_finish(&reader)) {
            player_status_model_clear(&replacement);
            return false;
        }

        player_status_model_clear(model);
        *model = replacement;
        return true;
    }

    if (operation == PLAYER_STATUS_UPSERT) {
        player_status_t status;
        if (!player_status_parse_entry(&reader, &status) || !packet_reader_finish(&reader)) {
            return false;
        }
        if (player_status_model_find(model, status.key) == NULL &&
            model->count >= ATRINIK_PLAYER_STATUS_MAX_STATUSES) {
            packet_reader_set_error(&reader, PACKET_ERROR_LIMIT_EXCEEDED);
            return false;
        }
        if (!player_status_model_upsert(model, &status)) {
            packet_reader_set_error(&reader, PACKET_ERROR_ALLOCATION);
            return false;
        }
        return true;
    }

    if (operation == PLAYER_STATUS_REMOVE) {
        char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
        packet_reader_read_string_bounded(&reader,
                                          key,
                                          sizeof(key),
                                          ATRINIK_PLAYER_STATUS_KEY_SIZE);
        if (key[0] == '\0') {
            packet_reader_set_error(&reader, PACKET_ERROR_INVALID_ENCODING);
        }
        if (!packet_reader_finish(&reader)) {
            return false;
        }
        player_status_model_remove(model, key);
        return true;
    }

    packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
    return false;
}
