/*************************************************************************
 * Atrinik server item-socket update regression tests.
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <disease.h>
#include <object.h>
#include <object_methods.h>
#include <player.h>
#include <server.h>
#include <spells.h>
#include <player_status.h>
#include <toolkit/packet.h>

static packet_struct *queued_command_find(socket_struct *cs, uint8_t type) {
    packet_struct *found = NULL;
    for (packet_struct *packet = cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type == type) {
            found = packet;
        }
    }
    return found;
}

static bool queued_drawinfo_has(socket_struct *cs, const char *expected) {
    for (packet_struct *packet = cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type != CLIENT_CMD_DRAWINFO) {
            continue;
        }
        packet_reader_t reader;
        char color[64];
        char message[HUGE_BUF];
        packet_reader_init(&reader, packet->data, packet->len);
        (void)packet_reader_read_uint8(&reader);
        ck_assert(packet_reader_read_string(&reader, VS(color)));
        ck_assert(packet_reader_read_string(&reader, VS(message)));
        ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_NONE);
        if (strcmp(message, expected) == 0) {
            return true;
        }
    }
    return false;
}

static void assert_status_entry(packet_reader_t *reader,
                                const char *expected_key,
                                const char *expected_name,
                                int32_t expected_seconds) {
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    char name[ATRINIK_PLAYER_STATUS_NAME_SIZE + 1U];
    char tooltip[ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE + 1U];
    ck_assert(packet_reader_read_string(reader, VS(key)));
    ck_assert_str_eq(key, expected_key);
    ck_assert_uint_gt(packet_reader_read_uint16(reader), 0);
    ck_assert(packet_reader_read_string(reader, VS(name)));
    ck_assert_str_eq(name, expected_name);
    ck_assert(packet_reader_read_string(reader, VS(tooltip)));
    ck_assert_int_eq(packet_reader_read_int32(reader), expected_seconds);
}

static void assert_status_entry_face(packet_reader_t *reader,
                                     const char *expected_key,
                                     const char *expected_face,
                                     const char *expected_name,
                                     int32_t expected_seconds) {
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    char name[ATRINIK_PLAYER_STATUS_NAME_SIZE + 1U];
    char tooltip[ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE + 1U];
    ck_assert(packet_reader_read_string(reader, VS(key)));
    ck_assert_str_eq(key, expected_key);
    ck_assert_uint_eq(packet_reader_read_uint16(reader), (uint16_t)find_face(expected_face, 0));
    ck_assert(packet_reader_read_string(reader, VS(name)));
    ck_assert_str_eq(name, expected_name);
    ck_assert(packet_reader_read_string(reader, VS(tooltip)));
    ck_assert_int_eq(packet_reader_read_int32(reader), expected_seconds);
}

START_TEST(test_update_map_item_name_and_count_marks_look_stale) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    object *stack = arch_get("torch");
    stack->x = pl->x;
    stack->y = pl->y;
    stack = object_insert_map(stack, map, NULL, INS_NO_MERGE);

    uint8_t old_update = GET_MAP_UPDATE_COUNTER(map, stack->x, stack->y);
    stack->nrof = 2;
    esrv_update_item(UPD_NAME | UPD_NROF, stack);

    ck_assert_uint_eq(GET_MAP_UPDATE_COUNTER(map, stack->x, stack->y), (uint8_t)(old_update + 1));

    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_disease_infection_duplicate_and_cure) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    socket_buffer_clear(CONTR(pl)->cs);

    object *carrier = arch_get("flu");
    ck_assert_ptr_nonnull(carrier);
    ck_assert(disease_infect(carrier, pl, true));

    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    assert_status_entry(&reader, "disease:flu", "flu", -1);
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    ck_assert(!disease_infect(carrier, pl, true));
    ck_assert_ptr_null(queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS));

    object *stronger = arch_get("flu");
    ck_assert_ptr_nonnull(stronger);
    stronger->level = carrier->level + 1;
    socket_buffer_clear(CONTR(pl)->cs);
    ck_assert(disease_infect(stronger, pl, true));
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    assert_status_entry(&reader, "disease:flu", "flu", -1);
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    player_status_send_snapshot(pl);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_SNAPSHOT);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), 1);
    assert_status_entry(&reader, "disease:flu", "flu", -1);
    ck_assert(packet_reader_finish(&reader));

    object *removed = NULL;
    FOR_INV_PREPARE(pl, tmp) {
        if (tmp->type == DISEASE) {
            removed = tmp;
            break;
        }
    }
    FOR_INV_FINISH();
    ck_assert_ptr_nonnull(removed);
    socket_buffer_clear(CONTR(pl)->cs);
    object_remove(removed, 0);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    assert_status_entry(&reader, "disease:flu", "flu", -1);
    ck_assert(packet_reader_finish(&reader));
    object_destroy(removed);

    socket_buffer_clear(CONTR(pl)->cs);
    ck_assert(disease_cure(pl, NULL));
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_REMOVE);
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    ck_assert(packet_reader_read_string(&reader, VS(key)));
    ck_assert_str_eq(key, "disease:flu");
    ck_assert(packet_reader_finish(&reader));

    object_destroy(carrier);
    object_destroy(stronger);
    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_hidden_disease_lifecycle_and_snapshot) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    socket_buffer_clear(CONTR(pl)->cs);

    object *disease = arch_get("flu");
    ck_assert_ptr_nonnull(disease);
    disease = object_insert_into(disease, pl, INS_NO_MERGE);
    ck_assert_ptr_nonnull(disease);
    ck_assert(player_status_should_publish(disease));

    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    assert_status_entry(&reader, "disease:flu", "flu", -1);
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    esrv_send_inventory(pl, pl);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_SNAPSHOT);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), 1);
    assert_status_entry(&reader, "disease:flu", "flu", -1);
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    object_remove(disease, 0);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_REMOVE);
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    ck_assert(packet_reader_read_string(&reader, VS(key)));
    ck_assert_str_eq(key, "disease:flu");
    ck_assert(packet_reader_finish(&reader));
    object_destroy(disease);

    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_explicit_force_allowlist) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    socket_buffer_clear(CONTR(pl)->cs);

    static const char *const published[] = {
        "blindness",
        "confusion",
        "depletion",
        "force_effect",
        "slowness",
        "soul_depletion",
    };
    for (size_t i = 0; i < arraysize(published); i++) {
        socket_buffer_clear(CONTR(pl)->cs);
        object *effect = arch_get(published[i]);
        ck_assert_ptr_nonnull(effect);
        SET_FLAG(effect, FLAG_APPLIED);
        effect = object_insert_into(effect, pl, INS_NO_MERGE);
        ck_assert_ptr_nonnull(effect);
        ck_assert(player_status_should_publish(effect));
        ck_assert_ptr_nonnull(queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS));
    }

    static const char *const excluded[] = {
        "force",
        "immunity",
        "player_force",
        "town_portal",
        "marker",
        "ability_stone_throw",
        "symptom",
    };
    for (size_t i = 0; i < arraysize(excluded); i++) {
        socket_buffer_clear(CONTR(pl)->cs);
        object *effect = arch_get(excluded[i]);
        ck_assert_ptr_nonnull(effect);
        SET_FLAG(effect, FLAG_APPLIED);
        effect = object_insert_into(effect, pl, INS_NO_MERGE);
        ck_assert_ptr_nonnull(effect);
        ck_assert(!player_status_should_publish(effect));
        ck_assert_ptr_null(queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS));
    }

    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_poison_and_timed_force_refresh_and_remove) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    socket_buffer_clear(CONTR(pl)->cs);

    object *poison = arch_get("poisoning");
    ck_assert_ptr_nonnull(poison);
    poison = object_insert_into(poison, pl, INS_NO_MERGE);
    ck_assert_ptr_nonnull(poison);
    ck_assert(player_status_should_publish(poison));
    ck_assert_ptr_nonnull(queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS));

    socket_buffer_clear(CONTR(pl)->cs);
    object *effect = arch_get("force_effect");
    ck_assert_ptr_nonnull(effect);
    SET_FLAG(effect, FLAG_APPLIED);
    SET_FLAG(effect, FLAG_IS_USED_UP);
    effect->speed = 0.05;
    effect->speed_left = -1.0;
    effect->stats.food = 10;
    effect = object_insert_into(effect, pl, INS_NO_MERGE);
    ck_assert_ptr_nonnull(effect);

    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    char name[ATRINIK_PLAYER_STATUS_NAME_SIZE + 1U];
    char tooltip[ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE + 1U];
    ck_assert(packet_reader_read_string(&reader, VS(key)));
    ck_assert_uint_gt(packet_reader_read_uint16(&reader), 0);
    ck_assert(packet_reader_read_string(&reader, VS(name)));
    ck_assert(packet_reader_read_string(&reader, VS(tooltip)));
    ck_assert_int_gt(packet_reader_read_int32(&reader), 0);
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    effect->stats.food = 20;
    esrv_update_item(UPD_EXTRA, effect);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);

    socket_buffer_clear(CONTR(pl)->cs);
    CLEAR_FLAG(effect, FLAG_APPLIED);
    esrv_update_item(UPD_FLAGS, effect);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_REMOVE);
    char removed_key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    ck_assert(packet_reader_read_string(&reader, VS(removed_key)));
    ck_assert_str_eq(removed_key, key);
    ck_assert(packet_reader_finish(&reader));

    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_capacity_resnapshots_membership_changes) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;

    pl->speed = 0.1;
    attack_peform_paralyze(pl, 1.0);
    ck_assert(QUERY_FLAG(pl, FLAG_PARALYZED));

    for (size_t i = 0; i < ATRINIK_PLAYER_STATUS_MAX_STATUSES + 1U; i++) {
        socket_buffer_clear(CONTR(pl)->cs);
        object *effect = arch_get("force_effect");
        ck_assert_ptr_nonnull(effect);
        SET_FLAG(effect, FLAG_APPLIED);
        if (i % 2U == 0U) {
            char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
            snprintf(VS(key), "capacity:%zu", i);
            ck_assert(player_status_set(effect, key, "capacity", "Capacity test.", effect->face));
        }
        effect = object_insert_into(effect, pl, INS_NO_MERGE);
        ck_assert_ptr_nonnull(effect);
    }

    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_SNAPSHOT);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), ATRINIK_PLAYER_STATUS_MAX_STATUSES);
    assert_status_entry_face(&reader,
                             "condition:paralysis",
                             "paralysis.101",
                             "paralysis",
                             (int32_t)ceil(-pl->speed_left / FABS(pl->speed) / MAX_TICKS));
    char keys[ATRINIK_PLAYER_STATUS_MAX_STATUSES - 1U][ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    for (size_t i = 0; i < arraysize(keys); i++) {
        char name[ATRINIK_PLAYER_STATUS_NAME_SIZE + 1U];
        char tooltip[ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE + 1U];
        ck_assert(packet_reader_read_string(&reader, VS(keys[i])));
        ck_assert_uint_gt(packet_reader_read_uint16(&reader), 0);
        ck_assert(packet_reader_read_string(&reader, VS(name)));
        ck_assert(packet_reader_read_string(&reader, VS(tooltip)));
        (void)packet_reader_read_int32(&reader);
        for (size_t j = 0; j < i; j++) {
            ck_assert_str_ne(keys[i], keys[j]);
        }
    }
    ck_assert(packet_reader_finish(&reader));

    object *removed = pl->inv;
    ck_assert_ptr_nonnull(removed);
    socket_buffer_clear(CONTR(pl)->cs);
    object_remove(removed, 0);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_SNAPSHOT);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), ATRINIK_PLAYER_STATUS_MAX_STATUSES);
    object_destroy(removed);

    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_explicit_opt_in_lifecycle) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    socket_buffer_clear(CONTR(pl)->cs);

    object *effect = arch_get("force");
    ck_assert_ptr_nonnull(effect);
    effect = object_insert_into(effect, pl, INS_NO_MERGE);
    ck_assert_ptr_nonnull(effect);
    ck_assert(!player_status_should_publish(effect));
    ck_assert_ptr_null(queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS));

    ck_assert(player_status_set(effect,
                                "condition:test",
                                "test condition",
                                "An explicitly published test condition.",
                                effect->face));
    ck_assert(player_status_should_publish(effect));
    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    assert_status_entry(&reader, "condition:test", "test condition", -1);
    ck_assert(packet_reader_finish(&reader));

    StringBuffer *serialized = stringbuffer_new();
    object_dump_rec(effect, serialized);
    char *serialized_string = stringbuffer_finish(serialized);
    object *roundtrip = object_load_str(serialized_string);
    ck_assert_ptr_nonnull(roundtrip);
    ck_assert_msg(object_get_value(roundtrip, "player_status_key") != NULL,
                  "%s",
                  serialized_string);
    ck_assert_str_eq(object_get_value(roundtrip, "player_status_key"), "condition:test");
    ck_assert_str_eq(object_get_value(roundtrip, "player_status_name"), "test condition");
    ck_assert_str_eq(object_get_value(roundtrip, "player_status_tooltip"),
                     "An explicitly published test condition.");
    ck_assert_uint_eq(roundtrip->face->number, effect->face->number);
    ck_assert_uint_gt(roundtrip->face->number, 0);
    ck_assert_ptr_nonnull(roundtrip->name);
    roundtrip = object_insert_into(roundtrip, pl, INS_NO_MERGE);
    ck_assert_ptr_nonnull(roundtrip);
    ck_assert(player_status_should_publish(roundtrip));
    free(serialized_string);

    socket_buffer_clear(CONTR(pl)->cs);
    player_status_send_snapshot(pl);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_SNAPSHOT);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), 1);
    assert_status_entry(&reader, "condition:test", "test condition", -1);
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    object_remove(effect, 0);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    assert_status_entry(&reader, "condition:test", "test condition", -1);
    ck_assert(packet_reader_finish(&reader));
    object_destroy(effect);

    socket_buffer_clear(CONTR(pl)->cs);
    object_remove(roundtrip, 0);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_REMOVE);
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    ck_assert(packet_reader_read_string(&reader, VS(key)));
    ck_assert_str_eq(key, "condition:test");
    ck_assert(packet_reader_finish(&reader));
    object_destroy(roundtrip);
    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_source_presentation_and_stable_key) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;

    object *source = arch_get("apple");
    object *effect = arch_get("force");
    ck_assert_ptr_nonnull(source);
    ck_assert_ptr_nonnull(effect);
    set_attr_value(&effect->stats, STR, 2);
    effect->protection[ATNR_FIRE] = 15;
    effect->attack[ATNR_COLD] = 20;
    effect->stats.food = 10;
    effect->speed = 0.002;
    effect->speed_left = -1.0;
    SET_FLAG(effect, FLAG_IS_USED_UP);
    ck_assert(player_status_set_from_source(effect, source, "food"));
    effect = object_insert_into(effect, pl, INS_NO_MERGE);
    ck_assert_ptr_nonnull(effect);

    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    char name[ATRINIK_PLAYER_STATUS_NAME_SIZE + 1U];
    char tooltip[ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE + 1U];
    ck_assert(packet_reader_read_string(&reader, VS(key)));
    ck_assert(strncmp(key, "food:", strlen("food:")) == 0);
    ck_assert_uint_gt(packet_reader_read_uint16(&reader), 0);
    ck_assert(packet_reader_read_string(&reader, VS(name)));
    ck_assert_str_eq(name, "red apple");
    ck_assert(packet_reader_read_string(&reader, VS(tooltip)));
    ck_assert_ptr_nonnull(strstr(tooltip, "Str +2"));
    ck_assert_ptr_nonnull(strstr(tooltip, "fire protection 15%"));
    ck_assert_ptr_nonnull(strstr(tooltip, "cold attack 20%"));
    ck_assert_int_eq(packet_reader_read_int32(&reader), 625);
    ck_assert(packet_reader_finish(&reader));

    object *same_effect = arch_get("force");
    set_attr_value(&same_effect->stats, STR, 2);
    same_effect->protection[ATNR_FIRE] = 15;
    same_effect->attack[ATNR_COLD] = 20;
    same_effect->stats.food = 50;
    ck_assert(player_status_set_from_source(same_effect, source, "food"));
    ck_assert_str_eq(object_get_value(effect, "player_status_key"),
                     object_get_value(same_effect, "player_status_key"));

    New_Face same_face = *source->face;
    same_face.number++;
    source->face = &same_face;
    object *renumbered_effect = arch_get("force");
    set_attr_value(&renumbered_effect->stats, STR, 2);
    renumbered_effect->protection[ATNR_FIRE] = 15;
    renumbered_effect->attack[ATNR_COLD] = 20;
    ck_assert(player_status_set_from_source(renumbered_effect, source, "food"));
    ck_assert_str_eq(object_get_value(effect, "player_status_key"),
                     object_get_value(renumbered_effect, "player_status_key"));

    FREE_AND_COPY_HASH(source->title, "hidden title");
    FREE_AND_COPY_HASH(source->artifact, "hidden_artifact");
    object *hidden_identity_effect = arch_get("force");
    set_attr_value(&hidden_identity_effect->stats, STR, 2);
    hidden_identity_effect->protection[ATNR_FIRE] = 15;
    hidden_identity_effect->attack[ATNR_COLD] = 20;
    ck_assert(player_status_set_from_source(hidden_identity_effect, source, "food"));
    ck_assert_str_eq(object_get_value(effect, "player_status_key"),
                     object_get_value(hidden_identity_effect, "player_status_key"));

    object_destroy(hidden_identity_effect);
    object_destroy(renumbered_effect);
    object_destroy(same_effect);
    object_destroy(source);
    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_paralysis_refresh_snapshot_and_cure) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    socket_buffer_clear(CONTR(pl)->cs);

    pl->speed = 0.1;
    pl->speed_left = 0.0;
    attack_peform_paralyze(pl, 1.0);
    ck_assert(QUERY_FLAG(pl, FLAG_PARALYZED));
    ck_assert(pl->speed_left < 0.0);

    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    assert_status_entry_face(&reader,
                             "condition:paralysis",
                             "paralysis.101",
                             "paralysis",
                             (int32_t)ceil(-pl->speed_left / FABS(pl->speed) / MAX_TICKS));
    ck_assert(packet_reader_finish(&reader));

    double first_debt = pl->speed_left;
    socket_buffer_clear(CONTR(pl)->cs);
    attack_peform_paralyze(pl, 1.0);
    ck_assert(pl->speed_left < first_debt);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    assert_status_entry_face(&reader,
                             "condition:paralysis",
                             "paralysis.101",
                             "paralysis",
                             (int32_t)ceil(-pl->speed_left / FABS(pl->speed) / MAX_TICKS));
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    pl->speed = 1.0;
    living_update_player(pl);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    assert_status_entry_face(&reader,
                             "condition:paralysis",
                             "paralysis.101",
                             "paralysis",
                             (int32_t)ceil(-pl->speed_left / FABS(pl->speed) / MAX_TICKS));
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    player_status_send_snapshot(pl);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_SNAPSHOT);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), 1);
    assert_status_entry(&reader,
                        "condition:paralysis",
                        "paralysis",
                        (int32_t)ceil(-pl->speed_left / FABS(pl->speed) / MAX_TICKS));
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    pl->speed_left = 0.0;
    pl->direction = 1;
    const uint8_t push[] = {SERVER_CMD_PLAYER_CMD, '/', 'p', 'u', 's', 'h', '\0'};
    ck_assert(socket_server_command_queue_append(CONTR(pl)->cs, push, sizeof(push)));
    ck_assert_int_eq(handle_newcs_player(CONTR(pl)), 0);
    ck_assert(!QUERY_FLAG(pl, FLAG_PARALYZED));
    ck_assert(queued_drawinfo_has(CONTR(pl)->cs, "You fail to push anything."));
    ck_assert(!queued_drawinfo_has(CONTR(pl)->cs, "You are unable to push anything."));
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_REMOVE);
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    ck_assert(packet_reader_read_string(&reader, VS(key)));
    ck_assert_str_eq(key, "condition:paralysis");
    ck_assert(packet_reader_finish(&reader));

    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_paralysis_pvp_death_clear) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    pl->speed = 0.1;
    attack_peform_paralyze(pl, 1.0);
    ck_assert(QUERY_FLAG(pl, FLAG_PARALYZED));

    map->map_flags |= MAP_FLAG_PVP;
    socket_buffer_clear(CONTR(pl)->cs);
    kill_player(pl, true, false);
    ck_assert(!QUERY_FLAG(pl, FLAG_PARALYZED));
    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_REMOVE);
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    ck_assert(packet_reader_read_string(&reader, VS(key)));
    ck_assert_str_eq(key, "condition:paralysis");
    ck_assert(packet_reader_finish(&reader));

    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_native_consumable_producers) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;

    object *food = arch_get("apple");
    ck_assert_ptr_nonnull(food);
    FREE_AND_COPY_HASH(food->title, "of might");
    set_attr_value(&food->stats, STR, 2);
    food->speed_left = 0.1;
    SET_FLAG(food, FLAG_CURSED);
    food = object_insert_into(food, pl, INS_NO_MERGE);
    socket_buffer_clear(CONTR(pl)->cs);
    manual_apply(pl, food, 0);
    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    char name[ATRINIK_PLAYER_STATUS_NAME_SIZE + 1U];
    char tooltip[ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE + 1U];
    ck_assert(packet_reader_read_string(&reader, VS(key)));
    ck_assert(strncmp(key, "food:", strlen("food:")) == 0);
    (void)packet_reader_read_uint16(&reader);
    ck_assert(packet_reader_read_string(&reader, VS(name)));
    ck_assert_str_eq(name, "cursed red apple");
    ck_assert(packet_reader_read_string(&reader, VS(tooltip)));
    ck_assert_ptr_nonnull(strstr(tooltip, "Str -4"));
    ck_assert_int_gt(packet_reader_read_int32(&reader), 0);
    ck_assert(packet_reader_finish(&reader));

    object *potion = arch_get("potion_generic");
    ck_assert_ptr_nonnull(potion);
    potion->stats.food = 1;
    set_attr_value(&potion->stats, DEX, 1);
    potion->protection[ATNR_COLD] = 10;
    SET_FLAG(potion, FLAG_DAMNED);
    potion = object_insert_into(potion, pl, INS_NO_MERGE);
    socket_buffer_clear(CONTR(pl)->cs);
    manual_apply(pl, potion, 0);
    object *potion_effect = NULL;
    FOR_INV_PREPARE(pl, tmp) {
        const char *status_key = object_get_value(tmp, "player_status_key");
        if (status_key != NULL && strncmp(status_key, "potion:", strlen("potion:")) == 0) {
            potion_effect = tmp;
            break;
        }
    }
    FOR_INV_FINISH();
    ck_assert_ptr_nonnull(potion_effect);
    ck_assert(QUERY_FLAG(potion_effect, FLAG_IS_USED_UP));
    ck_assert(FABS(potion_effect->speed) >= 0.000001);
    ck_assert_int_gt(potion_effect->stats.food, 0);
    ck_assert_int_eq(get_attr_value(&potion_effect->stats, DEX), -2);
    ck_assert_int_eq(potion_effect->protection[ATNR_COLD], -20);
    ck_assert_ptr_nonnull(strstr(object_get_value(potion_effect, "player_status_name"), "damned"));
    ck_assert_ptr_nonnull(
        strstr(object_get_value(potion_effect, "player_status_tooltip"), "Dex -2"));
    ck_assert_ptr_nonnull(
        strstr(object_get_value(potion_effect, "player_status_tooltip"), "cold protection -20%"));
    socket_buffer_clear(CONTR(pl)->cs);
    player_status_update(potion_effect);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);

    object_destroy(pl);
}
END_TEST

START_TEST(test_player_status_word_of_recall_cancel) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    socket_buffer_clear(CONTR(pl)->cs);

    ck_assert_ptr_nonnull(spells[SP_WOR].at);
    snprintf(VS(CONTR(pl)->savebed_map), "%s", "/emergency");
    ck_assert_int_eq(cast_wor(pl, pl), 1);
    object *recall = NULL;
    FOR_INV_PREPARE(pl, tmp) {
        if (tmp->type == WORD_OF_RECALL) {
            recall = tmp;
            break;
        }
    }
    FOR_INV_FINISH();
    ck_assert_ptr_nonnull(recall);
    ck_assert(player_status_should_publish(recall));
    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_UPSERT);
    assert_status_entry(&reader,
                        "spell:word_of_recall",
                        "word of recall",
                        (int32_t)ceil(ABS(recall->speed_left / recall->speed / MAX_TICKS)));
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    object_remove(recall, 0);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_REMOVE);
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    ck_assert(packet_reader_read_string(&reader, VS(key)));
    ck_assert_str_eq(key, "spell:word_of_recall");
    ck_assert(packet_reader_finish(&reader));
    object_destroy(recall);

    socket_buffer_clear(CONTR(pl)->cs);
    ck_assert_int_eq(cast_wor(pl, pl), 1);
    FOR_INV_PREPARE(pl, tmp) {
        if (tmp->type == WORD_OF_RECALL) {
            recall = tmp;
            break;
        }
    }
    FOR_INV_FINISH();
    ck_assert_ptr_nonnull(recall);
    SET_MAP_FLAGS(map, pl->x, pl->y, GET_MAP_FLAGS(map, pl->x, pl->y) | P_NO_MAGIC);
    socket_buffer_clear(CONTR(pl)->cs);
    object_process(recall);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), PLAYER_STATUS_REMOVE);
    ck_assert(packet_reader_read_string(&reader, VS(key)));
    ck_assert_str_eq(key, "spell:word_of_recall");
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(CONTR(pl)->cs);
    ck_assert_int_eq(cast_wor(pl, pl), 0);
    ck_assert_ptr_null(queued_command_find(CONTR(pl)->cs, CLIENT_CMD_PLAYER_STATUS));
    object_destroy(pl);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("item");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_update_map_item_name_and_count_marks_look_stale);
    tcase_add_test(tc_core, test_player_status_hidden_disease_lifecycle_and_snapshot);
    tcase_add_test(tc_core, test_player_status_disease_infection_duplicate_and_cure);
    tcase_add_test(tc_core, test_player_status_explicit_force_allowlist);
    tcase_add_test(tc_core, test_player_status_poison_and_timed_force_refresh_and_remove);
    tcase_add_test(tc_core, test_player_status_capacity_resnapshots_membership_changes);
    tcase_add_test(tc_core, test_player_status_explicit_opt_in_lifecycle);
    tcase_add_test(tc_core, test_player_status_source_presentation_and_stable_key);
    tcase_add_test(tc_core, test_player_status_paralysis_refresh_snapshot_and_cure);
    tcase_add_test(tc_core, test_player_status_paralysis_pvp_death_clear);
    tcase_add_test(tc_core, test_player_status_native_consumable_producers);
    tcase_add_test(tc_core, test_player_status_word_of_recall_cancel);

    return s;
}

void check_socket_item(void) {
    check_run_suite(suite(), __FILE__);
}
