#include <global.h>
#include <server_main.h>
#include <arch.h>
#include <player.h>
#include <player_status.h>
#include <server.h>
#include <object.h>
#include <toolkit/packet.h>

static bool player_status_force_candidate(const object *op) {
    if (op->type != FORCE || op->arch == NULL || op->arch->name == NULL) {
        return false;
    }

    static const char *const archetypes[] = {
        "blindness",
        "confusion",
        "depletion",
        "force_effect",
        "soul_depletion",
    };
    for (size_t i = 0; i < arraysize(archetypes); i++) {
        if (strcmp(op->arch->name, archetypes[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool player_status_candidate(const object *op) {
    return op != NULL &&
           (op->type == DISEASE || op->type == POISONING || player_status_force_candidate(op));
}

static bool player_status_key(const object *op, char *key, size_t key_size);

bool player_status_should_publish(const object *op) {
    if (!player_status_candidate(op) || op->env == NULL || op->env->type != PLAYER) {
        return false;
    }
    if (op->face == NULL || op->name == NULL) {
        return false;
    }
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    if (!player_status_key(op, key, sizeof(key))) {
        return false;
    }
    return op->type == DISEASE || op->type == POISONING || QUERY_FLAG(op, FLAG_APPLIED);
}

static bool player_status_key(const object *op, char *key, size_t key_size) {
    int length;
    if (op->type == DISEASE) {
        if (op->arch == NULL || op->arch->name == NULL) {
            return false;
        }
        length = snprintf(key, key_size, "disease:%s", op->arch->name);
    } else if (op->type == POISONING) {
        length = snprintf(key, key_size, "poison:%" PRIu32, op->count);
    } else {
        length = snprintf(key, key_size, "effect:%" PRIu32, op->count);
    }
    return length > 0 && (size_t)length < key_size;
}

static size_t player_status_wire_length_n(const char *value, size_t length, size_t maximum) {
    if (length <= maximum) {
        return length;
    }
    while (maximum > 0 && ((unsigned char)value[maximum] & 0xc0U) == 0x80U) {
        maximum--;
    }
    return maximum;
}

static size_t player_status_wire_length(const char *value, size_t maximum) {
    return player_status_wire_length_n(value, strlen(value), maximum);
}

static int32_t player_status_seconds(const object *op) {
    if (!QUERY_FLAG(op, FLAG_IS_USED_UP) || FABS(op->speed) < 0.000001) {
        return -1;
    }

    double current = ABS(op->speed_left / op->speed / MAX_TICKS);
    double remaining = ABS((1.0 / op->speed / MAX_TICKS) * (op->stats.food - 1));
    double seconds = current + remaining;
    return seconds >= INT32_MAX ? INT32_MAX : (int32_t)seconds;
}

static void player_status_write_entry(packet_struct *packet, const object *op) {
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    bool have_key = player_status_key(op, key, sizeof(key));
    if (!have_key) {
        HARD_ASSERT(false);
        return;
    }
    packet_writer_write_cstring(packet, key);
    packet_writer_write_uint16(packet, op->face->number);

    const char *name = op->name != NULL ? op->name : "status";
    packet_writer_write_cstring_n(packet,
                                  name,
                                  player_status_wire_length(name, ATRINIK_PLAYER_STATUS_NAME_SIZE));

    if (op->arch != NULL && op->arch->name != NULL && strcmp(op->arch->name, "depletion") == 0) {
        StringBuffer *tooltip = depletion_get_tooltip(op, NULL);
        packet_writer_write_cstring_n(
            packet,
            stringbuffer_data(tooltip),
            player_status_wire_length_n(stringbuffer_data(tooltip),
                                        stringbuffer_length(tooltip),
                                        ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE));
        stringbuffer_free(tooltip);
    } else {
        const char *tooltip = op->msg != NULL ? op->msg : "";
        packet_writer_write_cstring_n(
            packet,
            tooltip,
            player_status_wire_length(tooltip, ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE));
    }
    packet_writer_write_int32(packet, player_status_seconds(op));
}

static bool player_status_socket_ready(const object *pl) {
    return pl != NULL && pl->type == PLAYER && CONTR(pl) != NULL && CONTR(pl)->cs != NULL &&
           CONTR(pl)->cs->state == ST_PLAYING;
}

static void player_status_send_upsert(object *pl, const object *op) {
    if (!player_status_socket_ready(pl)) {
        return;
    }

    packet_struct *packet = packet_new(CLIENT_CMD_PLAYER_STATUS, 256, 256);
    packet_enable_ndelay(packet);
    packet_writer_write_uint8(packet, PLAYER_STATUS_UPSERT);
    player_status_write_entry(packet, op);
    socket_send_packet(CONTR(pl)->cs, packet);
}

static const object *player_status_find_by_key(const object *pl, const char *key) {
    for (const object *candidate = pl->inv; candidate != NULL; candidate = candidate->below) {
        char candidate_key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
        if (player_status_should_publish(candidate) &&
            player_status_key(candidate, candidate_key, sizeof(candidate_key)) &&
            strcmp(candidate_key, key) == 0) {
            return candidate;
        }
    }
    return NULL;
}

static size_t player_status_collect(const object *pl, const object **statuses, size_t limit) {
    char keys[ATRINIK_PLAYER_STATUS_MAX_STATUSES + 1U][ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    HARD_ASSERT(limit <= arraysize(keys));

    size_t count = 0;
    for (const object *op = pl->inv; op != NULL && count < limit; op = op->below) {
        if (!player_status_should_publish(op) ||
            !player_status_key(op, keys[count], sizeof(keys[count]))) {
            continue;
        }
        bool duplicate = false;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(keys[i], keys[count]) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            if (statuses != NULL) {
                statuses[count] = op;
            }
            count++;
        }
    }
    return count;
}

static bool player_status_requires_snapshot(const object *pl) {
    return player_status_collect(pl, NULL, ATRINIK_PLAYER_STATUS_MAX_STATUSES + 1U) >=
           ATRINIK_PLAYER_STATUS_MAX_STATUSES;
}

static void player_status_send_remove(object *pl, const char *key) {
    if (!player_status_socket_ready(pl)) {
        return;
    }

    packet_struct *packet = packet_new(CLIENT_CMD_PLAYER_STATUS, 32, 32);
    packet_enable_ndelay(packet);
    packet_writer_write_uint8(packet, PLAYER_STATUS_REMOVE);
    packet_writer_write_cstring(packet, key);
    socket_send_packet(CONTR(pl)->cs, packet);
}

void player_status_update(object *op) {
    if (!player_status_candidate(op) || op->env == NULL || op->env->type != PLAYER) {
        return;
    }

    object *pl = op->env;
    if (!player_status_should_publish(op)) {
        if (player_status_requires_snapshot(pl)) {
            player_status_send_snapshot(pl);
            return;
        }
        char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
        if (player_status_key(op, key, sizeof(key))) {
            player_status_send_remove(pl, key);
        }
        return;
    }
    if (player_status_collect(pl, NULL, ATRINIK_PLAYER_STATUS_MAX_STATUSES + 1U) >
        ATRINIK_PLAYER_STATUS_MAX_STATUSES) {
        player_status_send_snapshot(pl);
        return;
    }
    player_status_send_upsert(pl, op);
}

void player_status_remove(object *op) {
    if (!player_status_candidate(op) || op->env == NULL || op->env->type != PLAYER) {
        return;
    }
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    if (!player_status_key(op, key, sizeof(key))) {
        return;
    }
    if (player_status_requires_snapshot(op->env)) {
        player_status_send_snapshot(op->env);
        return;
    }
    const object *survivor = player_status_find_by_key(op->env, key);
    if (survivor != NULL) {
        player_status_send_upsert(op->env, survivor);
    } else {
        player_status_send_remove(op->env, key);
    }
}

void player_status_send_snapshot(object *pl) {
    if (!player_status_socket_ready(pl)) {
        return;
    }

    const object *statuses[ATRINIK_PLAYER_STATUS_MAX_STATUSES];
    uint16_t count =
        (uint16_t)player_status_collect(pl, statuses, ATRINIK_PLAYER_STATUS_MAX_STATUSES);

    packet_struct *packet = packet_new(CLIENT_CMD_PLAYER_STATUS, 512, 512);
    packet_enable_ndelay(packet);
    packet_writer_write_uint8(packet, PLAYER_STATUS_SNAPSHOT);
    packet_writer_write_uint16(packet, count);
    for (uint16_t i = 0; i < count; i++) {
        player_status_write_entry(packet, statuses[i]);
    }
    socket_send_packet(CONTR(pl)->cs, packet);
}
