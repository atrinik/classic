#include <global.h>
#include <server_main.h>
#include <arch.h>
#include <player.h>
#include <player_status.h>
#include <server.h>
#include <object.h>
#include <server_item.h>
#include <toolkit/packet.h>

#define PLAYER_STATUS_KEY_FIELD "player_status_key"
#define PLAYER_STATUS_NAME_FIELD "player_status_name"
#define PLAYER_STATUS_TOOLTIP_FIELD "player_status_tooltip"
#define PLAYER_STATUS_PARALYSIS_KEY "condition:paralysis"

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
           (object_get_value(op, PLAYER_STATUS_KEY_FIELD) != NULL || op->type == DISEASE ||
            op->type == POISONING || player_status_force_candidate(op));
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
    return object_get_value(op, PLAYER_STATUS_KEY_FIELD) != NULL || op->type == DISEASE ||
           op->type == POISONING || QUERY_FLAG(op, FLAG_APPLIED);
}

static bool player_status_key(const object *op, char *key, size_t key_size) {
    int length;
    const char *explicit_key = object_get_value(op, PLAYER_STATUS_KEY_FIELD);
    if (explicit_key != NULL) {
        length = snprintf(key, key_size, "%s", explicit_key);
    } else if (op->type == DISEASE) {
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

static uint64_t player_status_hash_bytes(uint64_t hash, const void *data, size_t length) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t player_status_hash_int(uint64_t hash, int value) {
    uint32_t encoded = (uint32_t)value;
    uint8_t bytes[] = {
        (uint8_t)(encoded >> 24),
        (uint8_t)(encoded >> 16),
        (uint8_t)(encoded >> 8),
        (uint8_t)encoded,
    };
    return player_status_hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t player_status_effect_hash(const object *op, const object *source) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const char *identity[] = {
        source->arch != NULL ? source->arch->name : NULL,
        source->artifact,
        source->name,
        source->title,
    };
    for (size_t i = 0; i < arraysize(identity); i++) {
        if (identity[i] != NULL) {
            hash = player_status_hash_bytes(hash, identity[i], strlen(identity[i]));
        }
        const uint8_t separator = 0;
        hash = player_status_hash_bytes(hash, &separator, sizeof(separator));
    }
    for (int i = 0; i < NUM_STATS; i++) {
        int value = get_attr_value(&op->stats, i);
        hash = player_status_hash_int(hash, value);
    }
    for (int i = 0; i < NROFATTACKS; i++) {
        hash = player_status_hash_int(hash, op->attack[i]);
        hash = player_status_hash_int(hash, op->protection[i]);
    }
    return hash;
}

bool player_status_set(object *op,
                       const char *key,
                       const char *name,
                       const char *tooltip,
                       New_Face *face) {
    if (op == NULL || key == NULL || key[0] == '\0' || name == NULL || name[0] == '\0' ||
        tooltip == NULL || face == NULL || strlen(key) > ATRINIK_PLAYER_STATUS_KEY_SIZE ||
        strlen(name) > ATRINIK_PLAYER_STATUS_NAME_SIZE ||
        strlen(tooltip) > ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE) {
        return false;
    }
    if (!object_set_value(op, PLAYER_STATUS_KEY_FIELD, key, true) ||
        !object_set_value(op, PLAYER_STATUS_NAME_FIELD, name, true) ||
        !object_set_value(op, PLAYER_STATUS_TOOLTIP_FIELD, tooltip, true)) {
        return false;
    }
    op->face = face;
    player_status_update(op);
    return true;
}

bool player_status_set_from_source(object *op, const object *source, const char *key_namespace) {
    if (op == NULL || source == NULL || key_namespace == NULL || source->face == NULL) {
        return false;
    }

    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    int key_length = snprintf(key,
                              sizeof(key),
                              "%s:%016" PRIx64,
                              key_namespace,
                              player_status_effect_hash(op, source));
    if (key_length <= 0 || (size_t)key_length >= sizeof(key)) {
        return false;
    }

    char *base_name = object_get_base_name_s(source, NULL);
    StringBuffer *display_name = stringbuffer_new();
    if (QUERY_FLAG(source, FLAG_DAMNED)) {
        stringbuffer_append_string(display_name, "damned ");
    } else if (QUERY_FLAG(source, FLAG_CURSED)) {
        stringbuffer_append_string(display_name, "cursed ");
    }
    stringbuffer_append_string(display_name, base_name);
    StringBuffer *tooltip = stringbuffer_new();
    bool have_value = false;
    for (int i = 0; i < NUM_STATS; i++) {
        int value = get_attr_value(&op->stats, i);
        if (value == 0) {
            continue;
        }
        stringbuffer_append_printf(tooltip,
                                   "%s%s %+d",
                                   have_value ? ", " : "",
                                   short_stat_name[i],
                                   value);
        have_value = true;
    }
    for (int i = 0; i < NROFATTACKS; i++) {
        if (op->protection[i] == 0) {
            continue;
        }
        stringbuffer_append_printf(tooltip,
                                   "%s%s protection %" PRId8 "%%",
                                   have_value ? ", " : "",
                                   attack_name[i],
                                   op->protection[i]);
        have_value = true;
    }
    if (!have_value) {
        stringbuffer_append_string(tooltip, "Temporary consumable effect");
    }

    char *display_name_string = stringbuffer_finish(display_name);
    char *tooltip_string = stringbuffer_finish(tooltip);
    bool result = player_status_set(op, key, display_name_string, tooltip_string, source->face);
    free(base_name);
    free(display_name_string);
    free(tooltip_string);
    return result;
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
    if (op->type == WORD_OF_RECALL && FABS(op->speed) >= 0.000001) {
        double seconds = ABS(op->speed_left / op->speed / MAX_TICKS);
        return seconds >= INT32_MAX ? INT32_MAX : (int32_t)ceil(seconds);
    }
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

    const char *name = object_get_value(op, PLAYER_STATUS_NAME_FIELD);
    if (name == NULL) {
        name = op->name != NULL ? op->name : "status";
    }
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
        const char *tooltip = object_get_value(op, PLAYER_STATUS_TOOLTIP_FIELD);
        if (tooltip == NULL) {
            tooltip = op->msg != NULL ? op->msg : "";
        }
        packet_writer_write_cstring_n(
            packet,
            tooltip,
            player_status_wire_length(tooltip, ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE));
    }
    packet_writer_write_int32(packet, player_status_seconds(op));
}

static bool player_status_paralysis_active(const object *pl) {
    return pl != NULL && pl->type == PLAYER && QUERY_FLAG(pl, FLAG_PARALYZED) &&
           pl->speed_left < 0.0 && FABS(pl->speed) >= 0.000001;
}

static int32_t player_status_paralysis_seconds(const object *pl) {
    double seconds = -pl->speed_left / FABS(pl->speed) / MAX_TICKS;
    return seconds >= INT32_MAX ? INT32_MAX : MAX(1, (int32_t)ceil(seconds));
}

static void player_status_write_paralysis(packet_struct *packet, const object *pl) {
    packet_writer_write_cstring(packet, PLAYER_STATUS_PARALYSIS_KEY);
    packet_writer_write_uint16(packet, (uint16_t)find_face("force.101", 0));
    packet_writer_write_cstring(packet, "paralysis");
    packet_writer_write_cstring(packet, "You cannot act until the paralysis wears off.");
    packet_writer_write_int32(packet, player_status_paralysis_seconds(pl));
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

static size_t player_status_count(const object *pl, size_t limit) {
    size_t count = player_status_collect(pl, NULL, limit);
    if (count < limit && player_status_paralysis_active(pl)) {
        count++;
    }
    return count;
}

static bool player_status_requires_snapshot(const object *pl) {
    return player_status_count(pl, ATRINIK_PLAYER_STATUS_MAX_STATUSES + 1U) >=
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

void player_status_update_paralysis(object *pl) {
    if (!player_status_socket_ready(pl)) {
        return;
    }
    if (!player_status_paralysis_active(pl)) {
        if (player_status_collect(pl, NULL, ATRINIK_PLAYER_STATUS_MAX_STATUSES) >=
            ATRINIK_PLAYER_STATUS_MAX_STATUSES) {
            player_status_send_snapshot(pl);
            return;
        }
        player_status_send_remove(pl, PLAYER_STATUS_PARALYSIS_KEY);
        return;
    }
    if (player_status_count(pl, ATRINIK_PLAYER_STATUS_MAX_STATUSES + 1U) >
        ATRINIK_PLAYER_STATUS_MAX_STATUSES) {
        player_status_send_snapshot(pl);
        return;
    }
    packet_struct *packet = packet_new(CLIENT_CMD_PLAYER_STATUS, 128, 128);
    packet_enable_ndelay(packet);
    packet_writer_write_uint8(packet, PLAYER_STATUS_UPSERT);
    player_status_write_paralysis(packet, pl);
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
    if (player_status_count(pl, ATRINIK_PLAYER_STATUS_MAX_STATUSES + 1U) >
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
    bool paralysis = player_status_paralysis_active(pl);
    size_t object_limit = ATRINIK_PLAYER_STATUS_MAX_STATUSES - (paralysis ? 1U : 0U);
    uint16_t count = (uint16_t)player_status_collect(pl, statuses, object_limit);

    packet_struct *packet = packet_new(CLIENT_CMD_PLAYER_STATUS, 512, 512);
    packet_enable_ndelay(packet);
    packet_writer_write_uint8(packet, PLAYER_STATUS_SNAPSHOT);
    packet_writer_write_uint16(packet, count + (paralysis ? 1U : 0U));
    if (paralysis) {
        player_status_write_paralysis(packet, pl);
    }
    for (uint16_t i = 0; i < count; i++) {
        player_status_write_entry(packet, statuses[i]);
    }
    socket_send_packet(CONTR(pl)->cs, packet);
}
