/*************************************************************************
 * Atrinik server presentation synchronization regression tests.
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <toolkit/clioptions.h>
#include <toolkit/map_protocol.h>
#include <toolkit/packet.h>
#include <arch.h>
#include <initialization.h>
#include <light.h>
#include <los.h>
#include <object.h>
#include <player.h>

static size_t queued_command_count(socket_struct *cs, uint8_t type) {
    size_t count = 0;

    for (packet_struct *packet = cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type == type) {
            count++;
        }
    }

    return count;
}

static packet_struct *queued_command_find(socket_struct *cs, uint8_t type) {
    for (packet_struct *packet = cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type == type) {
            return packet;
        }
    }

    return NULL;
}

static packet_struct *queued_command_payload_find(socket_struct *cs, uint8_t type) {
    for (packet_struct *packet = cs->packets; packet != NULL && packet->next != NULL;
         packet = packet->next) {
        if (packet->type == 0 && packet->len >= 3 && packet->data[packet->len - 1] == type) {
            return packet->next;
        }
    }

    return NULL;
}

static size_t validate_queued_map_payloads(socket_struct *cs) {
    size_t count = 0;
    for (packet_struct *packet = cs->packets; packet != NULL && packet->next != NULL;
         packet = packet->next) {
        if (packet->type != 0 || packet->len < 3 ||
            packet->data[packet->len - 1] != CLIENT_CMD_MAP) {
            continue;
        }
        packet_struct *payload = packet->next;
        ck_assert_uint_le(payload->len, PACKET_PAYLOAD_MAX);
        ck_assert_msg(map_protocol_validate(payload->data, payload->len, 0, cs->mapx, cs->mapy),
                      "invalid queued MAP payload %zu: command=%" PRIu8 " length=%zu",
                      count,
                      payload->data[0],
                      payload->len);
        count++;
        packet = payload;
    }
    return count;
}

static bool map_packet_level_size(const packet_struct *packet, int expected_depth, uint32_t *size) {
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);

    if (packet_reader_read_uint8(&reader) != MAP_UPDATE_CMD_SAME) {
        return false;
    }

    (void)packet_reader_read_uint8(&reader);
    (void)packet_reader_read_uint8(&reader);
    (void)packet_reader_read_uint8(&reader);
    (void)packet_reader_read_uint16(&reader);
    uint8_t level_count = packet_reader_read_uint8(&reader);

    for (uint8_t level = 0; level < level_count; level++) {
        int depth = packet_reader_read_int8(&reader);
        uint32_t level_size = packet_reader_read_uint32(&reader);
        if (level_size > packet_reader_remaining(&reader)) {
            return false;
        }

        if (depth == expected_depth) {
            *size = level_size;
            return true;
        }

        if (!packet_reader_skip(&reader, level_size)) {
            return false;
        }
    }

    return false;
}

static bool map_cache_cell_has_roof(socket_struct *cs, int depth, int x, int y) {
    MapCell *cell = map_client_cache_cell(&cs->lastmap, depth, x, y, false);
    if (cell == NULL || cell->cleared) {
        return false;
    }

    for (size_t layer = 0; layer < NUM_REAL_LAYERS; layer++) {
        if (cell->roof[layer]) {
            return true;
        }
    }

    return false;
}

static bool map_cache_has_roof(socket_struct *cs, int depth) {
    for (int x = 0; x < cs->mapx; x++) {
        for (int y = 0; y < cs->mapy; y++) {
            if (map_cache_cell_has_roof(cs, depth, x, y)) {
                return true;
            }
        }
    }

    return false;
}

static size_t map_cache_semantic_count(socket_struct *cs, bool exit_semantic) {
    size_t count = 0;

    for (int x = 0; x < cs->mapx; x++) {
        for (int y = 0; y < cs->mapy; y++) {
            MapCell *cell = map_client_cache_cell(&cs->lastmap, 0, x, y, false);
            if (cell == NULL || cell->cleared) {
                continue;
            }

            for (size_t layer = 0; layer < NUM_REAL_LAYERS; layer++) {
                count += (exit_semantic ? cell->exit[layer] : cell->door[layer]) != 0;
            }
        }
    }

    return count;
}

static void request_move_player(object **pl, mapstruct *map, int x, int y) {
    object_remove(*pl, 0);
    (*pl)->x = x;
    (*pl)->y = y;
    *pl = object_insert_map(*pl, map, NULL, 0);
    ck_assert_ptr_nonnull(*pl);
    CONTR(*pl)->update_los = 1;
}

static void request_move_path(player *pl, int x, int y) {
    uint8_t request[] = {
        (uint8_t)(pl->cs->mapx_2 + x - pl->ob->x),
        (uint8_t)(pl->cs->mapy_2 + y - pl->ob->y),
    };
    socket_command_move_path(pl->cs, pl, request, sizeof(request), 0);
}

static player_path *request_path_last(player *pl) {
    player_path *path = pl->move_path;
    while (path != NULL && path->next != NULL) {
        path = path->next;
    }
    return path;
}

static void request_run_path(player *pl) {
    pl->ob->speed_left = 100.0f;
    player_path_handle(pl);
}

static void request_set_path_node_budget(size_t budget) {
    char option[64];
    snprintf(option, sizeof(option), "pathfinder_max_nodes = %zu", budget);
    char *errmsg = NULL;
    ck_assert_msg(clioptions_load_str(option, &errmsg), "%s", errmsg != NULL ? errmsg : "");
    free(errmsg);
}

START_TEST(test_move_path_walkable_target_reaches_exact_coordinate) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 4, 4);

    request_move_path(CONTR(pl), 8, 4);

    player_path *last = request_path_last(CONTR(pl));
    ck_assert_ptr_nonnull(last);
    ck_assert_ptr_eq(last->map, map);
    ck_assert_int_eq(last->x, 8);
    ck_assert_int_eq(last->y, 4);
    request_run_path(CONTR(pl));
    ck_assert_ptr_null(CONTR(pl)->move_path);
    ck_assert_ptr_null(CONTR(pl)->move_path_end);
    ck_assert_int_eq(pl->x, 8);
    ck_assert_int_eq(pl->y, 4);
}
END_TEST

START_TEST(test_move_path_blocked_target_stops_at_deterministic_adjacent_tile) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 4, 4);
    SET_MAP_FLAGS(map, 8, 4, P_NO_PASS);

    request_move_path(CONTR(pl), 8, 4);

    player_path *last = request_path_last(CONTR(pl));
    ck_assert_ptr_nonnull(last);
    ck_assert(!(last->map == map && last->x == 8 && last->y == 4));
    int16_t expected_x = last->x;
    int16_t expected_y = last->y;
    request_run_path(CONTR(pl));
    ck_assert_ptr_null(CONTR(pl)->move_path);
    ck_assert_ptr_null(CONTR(pl)->move_path_end);
    ck_assert_int_eq(pl->x, expected_x);
    ck_assert_int_eq(pl->y, expected_y);

    request_move_player(&pl, map, 4, 4);
    request_move_path(CONTR(pl), 8, 4);
    last = request_path_last(CONTR(pl));
    ck_assert_ptr_nonnull(last);
    ck_assert_int_eq(last->x, expected_x);
    ck_assert_int_eq(last->y, expected_y);
}
END_TEST

START_TEST(test_move_path_blocks_opaque_passable_target) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 4, 4);
    SET_MAP_FLAGS(map, 8, 4, P_BLOCKSVIEW);

    request_move_path(CONTR(pl), 8, 4);

    player_path *last = request_path_last(CONTR(pl));
    ck_assert_ptr_nonnull(last);
    ck_assert(!(last->map == map && last->x == 8 && last->y == 4));
    request_run_path(CONTR(pl));
    ck_assert_ptr_null(CONTR(pl)->move_path);
    ck_assert_ptr_null(CONTR(pl)->move_path_end);
    ck_assert(!(pl->x == 8 && pl->y == 4));
}
END_TEST

START_TEST(test_move_path_occupied_target_stops_at_adjacent_tile) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 4, 4);
    object *npc = arch_get("kobold");
    npc->x = 8;
    npc->y = 4;
    npc = object_insert_map(npc, map, NULL, 0);
    ck_assert_ptr_nonnull(npc);

    request_move_path(CONTR(pl), 8, 4);

    player_path *last = request_path_last(CONTR(pl));
    ck_assert_ptr_nonnull(last);
    ck_assert(!(last->map == map && last->x == 8 && last->y == 4));
    request_run_path(CONTR(pl));
    ck_assert_ptr_null(CONTR(pl)->move_path);
    ck_assert(!(pl->x == 8 && pl->y == 4));
}
END_TEST

START_TEST(test_move_path_unreachable_request_clears_existing_queue) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 12, 12);
    player_path_add(CONTR(pl), map, 13, 12);
    for (int direction = 1; direction <= SIZEOFFREE1; direction++) {
        SET_MAP_FLAGS(map, pl->x + freearr_x[direction], pl->y + freearr_y[direction], P_NO_PASS);
    }

    request_move_path(CONTR(pl), 18, 18);

    ck_assert_ptr_null(CONTR(pl)->move_path);
    ck_assert_ptr_null(CONTR(pl)->move_path_end);
}
END_TEST

START_TEST(test_move_path_center_request_clears_existing_queue) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 12, 12);
    player_path_add(CONTR(pl), map, 13, 12);

    request_move_path(CONTR(pl), pl->x, pl->y);

    ck_assert_ptr_null(CONTR(pl)->move_path);
    ck_assert_ptr_null(CONTR(pl)->move_path_end);
}
END_TEST

START_TEST(test_move_path_unmapped_request_clears_existing_queue) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 0, 0);
    player_path_add(CONTR(pl), map, 1, 0);
    uint8_t request[] = {0, CONTR(pl)->cs->mapy_2};

    socket_command_move_path(CONTR(pl)->cs, CONTR(pl), request, sizeof(request), 0);

    ck_assert_ptr_null(CONTR(pl)->move_path);
    ck_assert_ptr_null(CONTR(pl)->move_path_end);
}
END_TEST

START_TEST(test_move_path_does_not_queue_partial_search_result) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 4, 4);
    request_set_path_node_budget(2);

    request_move_path(CONTR(pl), 18, 18);

    ck_assert_ptr_null(CONTR(pl)->move_path);
    ck_assert_ptr_null(CONTR(pl)->move_path_end);
    request_set_path_node_budget(10000);
}
END_TEST

START_TEST(test_move_path_opening_door_retains_queue_for_retry) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 4, 4);
    object *door = arch_get("door_wood1");
    ck_assert_ptr_nonnull(door);
    door->x = 5;
    door->y = 4;
    door = object_insert_map(door, map, NULL, 0);
    ck_assert_ptr_nonnull(door);

    request_move_path(CONTR(pl), 5, 4);
    player_path *queued = CONTR(pl)->move_path;
    ck_assert_ptr_nonnull(queued);

    pl->speed_left = 0.0f;
    player_path_handle(CONTR(pl));

    ck_assert_ptr_eq(CONTR(pl)->move_path, queued);
    ck_assert_ptr_eq(CONTR(pl)->move_path_end, queued);
    ck_assert_int_eq(pl->x, 4);
    ck_assert_int_eq(pl->y, 4);

    request_run_path(CONTR(pl));
    ck_assert_ptr_null(CONTR(pl)->move_path);
    ck_assert_ptr_null(CONTR(pl)->move_path_end);
    ck_assert_int_eq(pl->x, 5);
    ck_assert_int_eq(pl->y, 4);
}
END_TEST

START_TEST(test_move_path_invalid_request_preserves_existing_queue) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 12, 12);
    player_path_add(CONTR(pl), map, 13, 12);
    player_path *queued = CONTR(pl)->move_path;
    uint8_t truncated[] = {CONTR(pl)->cs->mapx_2};

    socket_command_move_path(CONTR(pl)->cs, CONTR(pl), truncated, sizeof(truncated), 0);
    ck_assert_ptr_eq(CONTR(pl)->move_path, queued);

    uint8_t out_of_range[] = {CONTR(pl)->cs->mapx, CONTR(pl)->cs->mapy};
    socket_command_move_path(CONTR(pl)->cs, CONTR(pl), out_of_range, sizeof(out_of_range), 0);
    ck_assert_ptr_eq(CONTR(pl)->move_path, queued);
}
END_TEST

START_TEST(test_move_path_new_blockage_stops_without_displacement) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 4, 4);
    request_move_path(CONTR(pl), 8, 4);
    ck_assert_ptr_nonnull(CONTR(pl)->move_path);
    SET_MAP_FLAGS(map, 5, 4, P_NO_PASS);

    request_run_path(CONTR(pl));

    ck_assert_ptr_null(CONTR(pl)->move_path);
    ck_assert_ptr_null(CONTR(pl)->move_path_end);
    ck_assert_int_eq(pl->x, 4);
    ck_assert_int_eq(pl->y, 4);
}
END_TEST

static void request_version(socket_struct *cs, player *pl, uint32_t version) {
    packet_struct *request = packet_new(0, 4, 0);
    packet_writer_write_uint32(request, version);
    socket_command_version(cs, pl, request->data, request->len, 0);
    packet_free(request);
}

enum command_phase_mask {
    COMMAND_PHASE_CONNECTED = 1U << 0,
    COMMAND_PHASE_VERSIONED = 1U << 1,
    COMMAND_PHASE_SETUP_UNAUTHENTICATED = 1U << 2,
    COMMAND_PHASE_ADMITTED_LOGIN = 1U << 3,
    COMMAND_PHASE_PLAYING = 1U << 4,
    COMMAND_PHASE_PLAYING_UNAUTHENTICATED = 1U << 5,
};

#define COMMAND_PHASE_LOGIN_MASK                                                               \
    (COMMAND_PHASE_CONNECTED | COMMAND_PHASE_VERSIONED | COMMAND_PHASE_SETUP_UNAUTHENTICATED | \
     COMMAND_PHASE_ADMITTED_LOGIN)
#define COMMAND_PHASE_PLAYING_MASK (COMMAND_PHASE_PLAYING | COMMAND_PHASE_PLAYING_UNAUTHENTICATED)
#define COMMAND_PHASE_ALL_MASK (COMMAND_PHASE_LOGIN_MASK | COMMAND_PHASE_PLAYING_MASK)

typedef struct command_phase {
    const char *name;
    unsigned int mask;
    int state;
    uint32_t socket_version;
    bool setup_completed;
    bool join_authenticated;
} command_phase_t;

static const command_phase_t command_phases[] = {
    {"connected", COMMAND_PHASE_CONNECTED, ST_LOGIN, 0, false, false},
    {"versioned", COMMAND_PHASE_VERSIONED, ST_LOGIN, SOCKET_VERSION, false, false},
    {"setup-unauthenticated",
     COMMAND_PHASE_SETUP_UNAUTHENTICATED,
     ST_LOGIN,
     SOCKET_VERSION,
     true,
     false},
    {"admitted-login", COMMAND_PHASE_ADMITTED_LOGIN, ST_LOGIN, SOCKET_VERSION, true, true},
    {"playing", COMMAND_PHASE_PLAYING, ST_PLAYING, SOCKET_VERSION, true, true},
    {"playing-unauthenticated",
     COMMAND_PHASE_PLAYING_UNAUTHENTICATED,
     ST_PLAYING,
     SOCKET_VERSION,
     true,
     false},
};

typedef struct command_policy_expectation {
    const char *name;
    unsigned int without_join_password;
    unsigned int with_join_password;
} command_policy_expectation_t;

#define COMMAND_POLICY(_symbol, _without, _with) \
    [SERVER_CMD_##_symbol] = {SERVER_CMD_NAME_##_symbol, (_without), (_with)}

static const command_policy_expectation_t command_policy_expectations[] = {
    COMMAND_POLICY(CONTROL, COMMAND_PHASE_LOGIN_MASK, COMMAND_PHASE_LOGIN_MASK),
    COMMAND_POLICY(ASK_FACE,
                   COMMAND_PHASE_SETUP_UNAUTHENTICATED | COMMAND_PHASE_ADMITTED_LOGIN |
                       COMMAND_PHASE_PLAYING_MASK,
                   COMMAND_PHASE_ADMITTED_LOGIN | COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(SETUP,
                   COMMAND_PHASE_VERSIONED | COMMAND_PHASE_PLAYING_MASK,
                   COMMAND_PHASE_VERSIONED | COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(VERSION, COMMAND_PHASE_CONNECTED, COMMAND_PHASE_CONNECTED),
    COMMAND_POLICY(CLEAR, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(REQUEST_UPDATE,
                   COMMAND_PHASE_SETUP_UNAUTHENTICATED | COMMAND_PHASE_ADMITTED_LOGIN,
                   COMMAND_PHASE_ADMITTED_LOGIN),
    COMMAND_POLICY(KEEPALIVE, COMMAND_PHASE_ALL_MASK, COMMAND_PHASE_ALL_MASK),
    COMMAND_POLICY(ACCOUNT,
                   COMMAND_PHASE_SETUP_UNAUTHENTICATED | COMMAND_PHASE_ADMITTED_LOGIN,
                   COMMAND_PHASE_ADMITTED_LOGIN),
    COMMAND_POLICY(ITEM_EXAMINE, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(ITEM_APPLY, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(ITEM_MOVE, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(ASK_RESOURCE, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(PLAYER_CMD, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(ITEM_LOCK, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(ITEM_MARK, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(FIRE, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(QUICKSLOT, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(QUESTLIST, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(MOVE_PATH, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(COMBAT, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(TALK, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(MOVE, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
    COMMAND_POLICY(TARGET, COMMAND_PHASE_PLAYING_MASK, COMMAND_PHASE_PLAYING),
};

#undef COMMAND_POLICY
CASSERT_ARRAY(command_policy_expectations, SERVER_CMD_NROF);

START_TEST(test_target_packet_includes_current_level_and_plain_name) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    pl->level = 42;
    CONTR(pl)->tgm = 1;
    socket_buffer_clear(CONTR(pl)->cs);

    send_target_command(CONTR(pl));

    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_TARGET);
    ck_assert_ptr_nonnull(packet);

    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    char color[MAX_BUF];
    char name[MAX_BUF];
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), CMD_TARGET_SELF);
    ck_assert(packet_reader_read_string(&reader, VS(color)));
    ck_assert(packet_reader_read_string(&reader, VS(name)));
    ck_assert_str_eq(name, pl->name);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 42);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), CONTR(pl)->combat);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), CONTR(pl)->combat_force);
    ck_assert(packet_reader_finish(&reader));
}
END_TEST

START_TEST(test_wizardry_level_change_refreshes_spell_cost_once) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    object *spell = object_insert_into(arch_get("spell_frostbolt"), pl, 0);
    ck_assert_ptr_nonnull(spell);
    ck_assert_int_eq(spell->type, SPELL);

    CONTR(pl)->last_spell_cost_level = CONTR(pl)->skill_ptr[SK_WIZARDRY_SPELLS]->level;
    socket_buffer_clear(CONTR(pl)->cs);
    CONTR(pl)->skill_ptr[SK_WIZARDRY_SPELLS]->level++;

    esrv_update_stats(CONTR(pl));
    ck_assert_uint_eq(queued_command_count(CONTR(pl)->cs, CLIENT_CMD_ITEM_UPDATE), 1);

    socket_buffer_clear(CONTR(pl)->cs);
    esrv_update_stats(CONTR(pl));
    ck_assert_uint_eq(queued_command_count(CONTR(pl)->cs, CLIENT_CMD_ITEM_UPDATE), 0);
}
END_TEST

START_TEST(test_setup_round_trip_uses_current_option_ids) {
    mapstruct *map;
    object *pl;

    ck_assert_uint_eq(CMD_SETUP_SOUND, 0);
    ck_assert_uint_eq(CMD_SETUP_MAPSIZE, 1);
    ck_assert_uint_eq(CMD_SETUP_DATA_URL, 2);
    ck_assert_uint_eq(CMD_SETUP_JOIN_PASSWORD, 3);
    ck_assert_uint_eq(CMD_SETUP_ASSET_TRANSPORT, 4);
    ck_assert_uint_eq(CMD_SETUP_CONNECTION_MODE, 5);
    ck_assert_uint_ge(SOCKET_VERSION, ASSET_TRANSPORT_FACE_BATCH_VERSION);
    ck_assert_uint_eq(ASSET_TRANSPORT_CAP_ALL,
                      ASSET_TRANSPORT_CAP_GENERIC | ASSET_TRANSPORT_CAP_FACE_BATCH);

    check_setup_env_pl(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    cs->asset_transport_capabilities = ASSET_TRANSPORT_CAP_ALL;
    uint8_t request[] = {CMD_SETUP_SOUND, 1, CMD_SETUP_MAPSIZE, 13, 15, CMD_SETUP_ASSET_TRANSPORT};
    socket_buffer_clear(cs);

    socket_command_setup(cs, CONTR(pl), request, sizeof(request), 0);

    packet_struct *response = queued_command_find(cs, CLIENT_CMD_SETUP);
    ck_assert_ptr_nonnull(response);

    packet_reader_t reader;
    packet_reader_init(&reader, response->data, response->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), CMD_SETUP_SOUND);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 1);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), CMD_SETUP_MAPSIZE);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 13);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 15);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), CMD_SETUP_ASSET_TRANSPORT);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), ASSET_TRANSPORT_CAP_ALL);
    ck_assert(packet_reader_finish(&reader));
    ck_assert_uint_eq(cs->sound, 1);
    ck_assert_int_eq(cs->mapx, 13);
    ck_assert_int_eq(cs->mapy, 15);
}
END_TEST

START_TEST(test_setup_rejects_unknown_option) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    uint8_t request[] = {UINT8_MAX};
    socket_buffer_clear(cs);

    socket_command_setup(cs, CONTR(pl), request, sizeof(request), 0);

    ck_assert_int_eq(cs->state, ST_ZOMBIE);
    ck_assert_ptr_null(queued_command_find(cs, CLIENT_CMD_SETUP));
}
END_TEST

START_TEST(test_initial_setup_completion_is_transactional) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    cs->state = ST_LOGIN;
    cs->socket_version = SOCKET_VERSION;
    cs->setup_completed = false;
    settings.join_password[0] = '\0';
    socket_buffer_clear(cs);

    uint8_t truncated[] = {CMD_SETUP_MAPSIZE, 13};
    socket_command_setup(cs, CONTR(pl), truncated, sizeof(truncated), 0);
    ck_assert(!cs->setup_completed);
    ck_assert_ptr_null(queued_command_find(cs, CLIENT_CMD_SETUP));

    uint8_t valid[] = {CMD_SETUP_MAPSIZE, 13, 15};
    socket_command_setup(cs, CONTR(pl), valid, sizeof(valid), 0);
    ck_assert(cs->setup_completed);
    ck_assert_ptr_nonnull(queued_command_find(cs, CLIENT_CMD_SETUP));
}
END_TEST

START_TEST(test_initial_setup_requires_valid_join_password) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    cs->state = ST_LOGIN;
    cs->socket_version = SOCKET_VERSION;
    cs->join_authenticated = false;
    cs->setup_completed = false;
    memset(settings.join_password, 0, sizeof(settings.join_password));
    snprintf(VS(settings.join_password), "%s", "secret");
    socket_buffer_clear(cs);

    packet_struct *request = packet_new(0, 32, 0);
    packet_writer_write_uint8(request, CMD_SETUP_JOIN_PASSWORD);
    packet_writer_write_cstring(request, "secret");
    socket_command_setup(cs, CONTR(pl), request->data, request->len, 0);
    packet_free(request);
    ck_assert(cs->join_authenticated);
    ck_assert(cs->setup_completed);
    ck_assert_int_eq(cs->state, ST_LOGIN);

    packet_struct *response = queued_command_find(cs, CLIENT_CMD_SETUP);
    ck_assert_ptr_nonnull(response);
    packet_reader_t reader;
    packet_reader_init(&reader, response->data, response->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), CMD_SETUP_JOIN_PASSWORD);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 1);
    ck_assert(packet_reader_finish(&reader));

    socket_buffer_clear(cs);
    cs->state = ST_LOGIN;
    cs->join_authenticated = false;
    cs->setup_completed = false;
    request = packet_new(0, 32, 0);
    packet_writer_write_uint8(request, CMD_SETUP_JOIN_PASSWORD);
    packet_writer_write_cstring(request, "wrong");
    socket_command_setup(cs, CONTR(pl), request->data, request->len, 0);
    packet_free(request);
    ck_assert(!cs->join_authenticated);
    ck_assert(!cs->setup_completed);
    ck_assert_int_eq(cs->state, ST_ZOMBIE);
}
END_TEST

START_TEST(test_command_policy_covers_every_connection_phase) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;

    for (size_t join_required = 0; join_required < 2; join_required++) {
        memset(settings.join_password, 0, sizeof(settings.join_password));
        snprintf(VS(settings.join_password), "%s", join_required ? "secret" : "");

        for (size_t phase_index = 0; phase_index < arraysize(command_phases); phase_index++) {
            const command_phase_t *phase = &command_phases[phase_index];
            cs->state = phase->state;
            cs->socket_version = phase->socket_version;
            cs->setup_completed = phase->setup_completed;
            cs->join_authenticated = phase->join_authenticated;

            for (size_t command = 0; command < arraysize(command_policy_expectations); command++) {
                const command_policy_expectation_t *expectation =
                    &command_policy_expectations[command];
                unsigned int allowed = join_required ? expectation->with_join_password
                                                     : expectation->without_join_password;
                bool expected = (allowed & phase->mask) != 0;
                bool actual = socket_server_command_phase_allowed(cs, (uint8_t)command);
                ck_assert_msg(actual == expected,
                              "%s was unexpectedly %s in phase %s with join password %s",
                              expectation->name,
                              actual ? "allowed" : "rejected",
                              phase->name,
                              join_required ? "enabled" : "disabled");
            }
        }
    }
}
END_TEST

START_TEST(test_out_of_order_player_command_is_not_queued) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    settings.join_password[0] = '\0';
    uint8_t request[] = {SERVER_CMD_MOVE};

    cs->state = ST_LOGIN;
    cs->socket_version = 0;
    cs->setup_completed = false;
    ck_assert(socket_server_handle_command(cs, NULL, request, sizeof(request)));
    ck_assert_uint_eq(cs->packet_recv_cmd->len, 0);

    cs->state = ST_PLAYING;
    cs->socket_version = SOCKET_VERSION;
    cs->setup_completed = true;
    ck_assert(!socket_server_handle_command(cs, NULL, request, sizeof(request)));
}
END_TEST

START_TEST(test_only_valid_post_setup_activity_refreshes_login_deadline) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    settings.join_password[0] = '\0';
    uint8_t keepalive[] = {SERVER_CMD_KEEPALIVE};

    cs->state = ST_LOGIN;
    cs->socket_version = SOCKET_VERSION;
    cs->setup_completed = true;
    cs->login_deadline = (server_monotonic_t){1};
    ck_assert(socket_server_handle_command(cs, NULL, keepalive, sizeof(keepalive)));
    ck_assert_uint_gt(cs->login_deadline.microseconds, 1);

    cs->setup_completed = false;
    cs->login_deadline = (server_monotonic_t){1};
    ck_assert(socket_server_handle_command(cs, NULL, keepalive, sizeof(keepalive)));
    ck_assert_uint_eq(cs->login_deadline.microseconds, 1);

    uint8_t player_command[] = {SERVER_CMD_MOVE};
    ck_assert(socket_server_handle_command(cs, NULL, player_command, sizeof(player_command)));
    ck_assert_uint_eq(cs->login_deadline.microseconds, 1);
}
END_TEST

START_TEST(test_version_requires_exact_match) {
    mapstruct *map;
    object *pl;

    check_setup_env_pl(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;

    socket_buffer_clear(cs);
    cs->state = ST_LOGIN;
    cs->socket_version = 0;
    request_version(cs, CONTR(pl), SOCKET_VERSION - 1);
    ck_assert_int_eq(cs->state, ST_ZOMBIE);
    ck_assert_uint_eq(cs->socket_version, 0);
    ck_assert_ptr_null(queued_command_find(cs, CLIENT_CMD_VERSION));

    socket_buffer_clear(cs);
    cs->state = ST_LOGIN;
    cs->socket_version = 0;
    request_version(cs, CONTR(pl), SOCKET_VERSION + 1);
    ck_assert_int_eq(cs->state, ST_ZOMBIE);
    ck_assert_uint_eq(cs->socket_version, 0);
    ck_assert_ptr_null(queued_command_find(cs, CLIENT_CMD_VERSION));

    socket_buffer_clear(cs);
    cs->state = ST_LOGIN;
    cs->socket_version = 0;
    request_version(cs, CONTR(pl), SOCKET_VERSION);
    ck_assert_int_eq(cs->state, ST_LOGIN);
    ck_assert_uint_eq(cs->socket_version, SOCKET_VERSION);

    packet_struct *response = queued_command_find(cs, CLIENT_CMD_VERSION);
    ck_assert_ptr_nonnull(response);
    packet_reader_t reader;
    packet_reader_init(&reader, response->data, response->len);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), SOCKET_VERSION);
    ck_assert(packet_reader_finish(&reader));
}
END_TEST

START_TEST(test_incuna_unchanged_roof_level_remains_present) {
    mapstruct *map = ready_map_name("/shattered_islands/world_4_85", NULL, 0);
    ck_assert_ptr_nonnull(map);

    mapstruct *roof_map = get_map_from_tiled(map, TILED_UP);
    ck_assert_ptr_nonnull(roof_map);
    ck_assert_str_eq(roof_map->path, "/shattered_islands/world_4_85_1");

    object *pl = player_get_dummy(NULL, NULL);
    ck_assert_ptr_nonnull(pl);
    object_remove(pl, 0);
    pl->x = 7;
    pl->y = 6;
    pl = object_insert_map(pl, map, NULL, 0);
    ck_assert_ptr_nonnull(pl);

    socket_struct *cs = CONTR(pl)->cs;
    CONTR(pl)->map_update_cmd = MAP_UPDATE_CMD_SAME;
    update_los(pl);
    socket_buffer_clear(cs);

    draw_client_map2(pl);

    packet_struct *packet = queued_command_payload_find(cs, CLIENT_CMD_MAP);
    ck_assert_ptr_nonnull(packet);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, cs->mapx, cs->mapy));
    uint32_t roof_delta_size = 0;
    ck_assert(map_packet_level_size(packet, 1, &roof_delta_size));
    ck_assert_uint_gt(roof_delta_size, 0);

    bool stable_roof_level_found = false;
    for (size_t update = 0; update < 8; update++) {
        socket_buffer_clear(cs);
        draw_client_map2(pl);

        packet = queued_command_payload_find(cs, CLIENT_CMD_MAP);
        ck_assert_ptr_nonnull(packet);
        ck_assert(map_protocol_validate(packet->data, packet->len, 0, cs->mapx, cs->mapy));
        roof_delta_size = UINT32_MAX;
        ck_assert(map_packet_level_size(packet, 1, &roof_delta_size));
        if (roof_delta_size == 0) {
            stable_roof_level_found = true;
            break;
        }
    }

    ck_assert_msg(stable_roof_level_found, "Incuna roof level did not reach a stable empty delta");
    ck_assert(map_cache_cell_has_roof(cs, 1, cs->mapx_2, cs->mapy_2 - 3));

    CONTR(pl)->last_update = map;
    CONTR(pl)->map_tile_x = pl->x;
    CONTR(pl)->map_tile_y = pl->y;
    request_move_player(&pl, map, 8, 6);
    socket_buffer_clear(cs);
    draw_client_map(pl);
    ck_assert(map_cache_cell_has_roof(cs, 1, cs->mapx_2 - 1, cs->mapy_2 - 3));

    request_move_player(&pl, map, 7, 3);
    socket_buffer_clear(cs);
    draw_client_map(pl);

    MapCell *interior_roof = map_client_cache_cell(&cs->lastmap, 1, cs->mapx_2, cs->mapy_2, false);
    ck_assert_ptr_nonnull(interior_roof);
    ck_assert_uint_eq(interior_roof->cleared, 1);
    ck_assert(map_cache_has_roof(cs, 1));

    request_move_player(&pl, map, 7, 6);
    socket_buffer_clear(cs);
    draw_client_map(pl);
    ck_assert(map_cache_cell_has_roof(cs, 1, cs->mapx_2, cs->mapy_2 - 3));

    map_client_cache_clear(&cs->lastmap);
    cs->lastmap_player_level_known = false;
    socket_buffer_clear(cs);
    draw_client_map2(pl);
    ck_assert(map_cache_cell_has_roof(cs, 1, cs->mapx_2, cs->mapy_2 - 3));

    mapstruct *dock_map = ready_map_name("/shattered_islands/world_5_84", NULL, 0);
    ck_assert_ptr_nonnull(dock_map);
    object_remove(pl, 0);
    pl->x = 9;
    pl->y = 15;
    pl->sub_layer = 1;
    pl = object_insert_map(pl, dock_map, NULL, 0);
    ck_assert_ptr_nonnull(pl);
    map_client_cache_clear(&cs->lastmap);
    cs->lastmap_player_level_known = false;
    CONTR(pl)->last_update = NULL;
    CONTR(pl)->update_los = 1;
    socket_buffer_clear(cs);
    draw_client_map(pl);
    ck_assert_msg(map_cache_has_roof(cs, 1),
                  "Incuna dock roofs are absent at the reported position");
    packet = queued_command_payload_find(cs, CLIENT_CMD_MAP);
    ck_assert_ptr_nonnull(packet);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, cs->mapx, cs->mapy));

    bool dock_stable_roof_level_found = false;
    for (size_t update = 0; update < 8; update++) {
        socket_buffer_clear(cs);
        draw_client_map(pl);

        packet = queued_command_payload_find(cs, CLIENT_CMD_MAP);
        ck_assert_ptr_nonnull(packet);
        ck_assert(map_protocol_validate(packet->data, packet->len, 0, cs->mapx, cs->mapy));
        roof_delta_size = UINT32_MAX;
        ck_assert_msg(map_packet_level_size(packet, 1, &roof_delta_size),
                      "Incuna dock roof level was omitted after login");
        dock_stable_roof_level_found |= roof_delta_size == 0;
        ck_assert_msg(map_cache_has_roof(cs, 1),
                      "Incuna dock roofs disappeared after an unchanged update");
    }
    ck_assert_msg(dock_stable_roof_level_found,
                  "Incuna dock roof level did not reach a stable empty delta");
}
END_TEST

START_TEST(test_map_exit_semantic_not_disclosed_by_boundary_geometry) {
    mapstruct *base;
    object *pl;
    check_setup_env_pl(&base, &pl);
    request_move_player(&pl, base, 12, 12);

    mapstruct *upper = get_empty_map(24, 24);
    base->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = base;
    base->coords[2] = 0;
    base->level_min = upper->level_min = 0;
    base->level_max = upper->level_max = 1;
    upper->coords[2] = 1;

    int target_x = pl->x + 1;
    int target_y = pl->y;

    object *exit = arch_get("stairs_down");
    ck_assert_ptr_nonnull(exit);
    exit->x = target_x;
    exit->y = target_y;
    exit = object_insert_map(exit, base, NULL, 0);
    ck_assert_ptr_nonnull(exit);

    object *roof = arch_get("roof_thatch");
    ck_assert_ptr_nonnull(roof);
    roof->x = target_x;
    roof->y = target_y;
    roof = object_insert_map(roof, upper, NULL, 0);
    ck_assert_ptr_nonnull(roof);

    socket_struct *cs = CONTR(pl)->cs;
    int ax = cs->mapx_2 + 1;
    int ay = cs->mapy_2;

    update_los(pl);
    CONTR(pl)->blocked_los[ax][ay] |= BLOCKED_LOS_BLOCKED;
    map_client_cache_clear(&cs->lastmap);
    socket_buffer_clear(cs);
    CONTR(pl)->map_update_cmd = MAP_UPDATE_CMD_SAME;
    draw_client_map2(pl);

    ck_assert_uint_eq(validate_queued_map_payloads(cs), 1);
    ck_assert(map_cache_cell_has_roof(cs, 1, ax, ay));

    MapCell *cell = map_client_cache_cell(&cs->lastmap, 0, ax, ay, false);
    ck_assert_ptr_nonnull(cell);
    ck_assert_uint_eq(cell->cleared, 0);
    ck_assert_uint_eq(cell->fow, 1);

    size_t socket_layer = NUM_LAYERS * exit->sub_layer + LAYER_WALL - 1;
    ck_assert_uint_eq(cell->exit[socket_layer], 0);
}
END_TEST

START_TEST(test_map_rgb_cache_tracks_hue_changes_and_neutral_reset) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 4, 4);
    object *source = arch_get("letter");
    source->x = pl->x;
    source->y = pl->y;
    source->glow_radius = 1;
    source->light_color = UINT32_C(0xff0000);
    source = object_insert_map(source, map, NULL, 0);
    ck_assert_ptr_nonnull(source);

    update_los(pl);
    map_client_cache_clear(&CONTR(pl)->cs->lastmap);
    socket_buffer_clear(CONTR(pl)->cs);
    CONTR(pl)->map_update_cmd = MAP_UPDATE_CMD_SAME;
    draw_client_map2(pl);
    ck_assert_uint_eq(validate_queued_map_payloads(CONTR(pl)->cs), 1);
    MapCell *cell = map_client_cache_cell(&CONTR(pl)->cs->lastmap,
                                          0,
                                          CONTR(pl)->cs->mapx_2,
                                          CONTR(pl)->cs->mapy_2,
                                          false);
    ck_assert_ptr_nonnull(cell);
    ck_assert_uint_gt(cell->light_rgb[pl->sub_layer][0], cell->light_rgb[pl->sub_layer][2]);

    adjust_light_source_color(map, source->x, source->y, 1, source->light_color, -1);
    source->light_color = UINT32_C(0x0000ff);
    adjust_light_source_color(map, source->x, source->y, 1, source->light_color, 1);
    socket_buffer_clear(CONTR(pl)->cs);
    draw_client_map2(pl);
    ck_assert_uint_eq(validate_queued_map_payloads(CONTR(pl)->cs), 1);
    ck_assert_uint_gt(cell->light_rgb[pl->sub_layer][2], cell->light_rgb[pl->sub_layer][0]);

    adjust_light_source_color(map, source->x, source->y, 1, source->light_color, -1);
    source->light_color = LIGHT_COLOR_WHITE;
    adjust_light_source_color(map, source->x, source->y, 1, source->light_color, 1);
    socket_buffer_clear(CONTR(pl)->cs);
    draw_client_map2(pl);
    ck_assert_uint_eq(validate_queued_map_payloads(CONTR(pl)->cs), 1);
    ck_assert_uint_eq(cell->light_rgb[pl->sub_layer][0], cell->light_level[pl->sub_layer]);
    ck_assert_uint_eq(cell->light_rgb[pl->sub_layer][1], cell->light_level[pl->sub_layer]);
    ck_assert_uint_eq(cell->light_rgb[pl->sub_layer][2], cell->light_level[pl->sub_layer]);
}
END_TEST

START_TEST(test_map_exit_semantic_tracks_visible_layer_and_cache_changes) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    request_move_player(&pl, map, 12, 12);
    SET_FLAG(pl, FLAG_XRAYS);

    object *exit = arch_get("stairs_down");
    ck_assert_ptr_nonnull(exit);
    exit->x = pl->x + 1;
    exit->y = pl->y;
    exit = object_insert_map(exit, map, NULL, 0);
    ck_assert_ptr_nonnull(exit);

    object *door = arch_get("door_wood1");
    ck_assert_ptr_nonnull(door);
    door->x = pl->x - 1;
    door->y = pl->y;
    door = object_insert_map(door, map, NULL, 0);
    ck_assert_ptr_nonnull(door);

    object *invisible_exit = arch_get("invis_exit");
    ck_assert_ptr_nonnull(invisible_exit);
    invisible_exit->x = pl->x;
    invisible_exit->y = pl->y + 1;
    invisible_exit = object_insert_map(invisible_exit, map, NULL, 0);
    ck_assert_ptr_nonnull(invisible_exit);

    socket_struct *cs = CONTR(pl)->cs;
    update_los(pl);
    map_client_cache_clear(&cs->lastmap);
    socket_buffer_clear(cs);
    CONTR(pl)->map_update_cmd = MAP_UPDATE_CMD_SAME;
    draw_client_map2(pl);
    ck_assert_uint_eq(validate_queued_map_payloads(cs), 1);

    ck_assert_uint_eq(map_cache_semantic_count(cs, true), 1);
    ck_assert_uint_eq(map_cache_semantic_count(cs, false), 1);

    socket_buffer_clear(cs);
    draw_client_map2(pl);
    ck_assert_uint_eq(validate_queued_map_payloads(cs), 1);
    ck_assert_uint_eq(map_cache_semantic_count(cs, true), 1);
    ck_assert_uint_eq(map_cache_semantic_count(cs, false), 1);
    packet_struct *packet;
    uint32_t level_size;

    exit->type = DOOR;
    socket_buffer_clear(cs);
    draw_client_map2(pl);
    ck_assert_uint_eq(validate_queued_map_payloads(cs), 1);
    ck_assert_uint_eq(map_cache_semantic_count(cs, true), 0);
    ck_assert_uint_eq(map_cache_semantic_count(cs, false), 2);
    packet = queued_command_payload_find(cs, CLIENT_CMD_MAP);
    ck_assert_ptr_nonnull(packet);
    ck_assert(map_packet_level_size(packet, 0, &level_size));
    ck_assert_uint_gt(level_size, 0);

    object_remove(exit, 0);
    socket_buffer_clear(cs);
    draw_client_map2(pl);
    ck_assert_uint_eq(validate_queued_map_payloads(cs), 1);
    ck_assert_uint_eq(map_cache_semantic_count(cs, true), 0);
    ck_assert_uint_eq(map_cache_semantic_count(cs, false), 1);
    packet = queued_command_payload_find(cs, CLIENT_CMD_MAP);
    ck_assert_ptr_nonnull(packet);
    ck_assert(map_packet_level_size(packet, 0, &level_size));
    ck_assert_uint_gt(level_size, 0);
    object_destroy(exit);
}
END_TEST

START_TEST(test_dense_colored_level_splits_at_tile_boundaries) {
    mapstruct *old_map;
    object *pl;
    check_setup_env_pl(&old_map, &pl);
    mapstruct *map = get_empty_map(MAP_CLIENT_X, MAP_CLIENT_Y);
    request_move_player(&pl, map, MAP_CLIENT_X / 2, MAP_CLIENT_Y / 2);

    char glow[241];
    memset(glow, 'a', sizeof(glow) - 1);
    glow[sizeof(glow) - 1] = '\0';
    for (int y = 0; y < MAP_CLIENT_Y; y++) {
        for (int x = 0; x < MAP_CLIENT_X; x++) {
            object *marker = arch_get("letter");
            marker->x = x;
            marker->y = y;
            FREE_AND_COPY_HASH(marker->glow, glow);
            marker->glow_speed = 1;
            ck_assert_ptr_nonnull(object_insert_map(marker, map, NULL, 0));
            MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
            space->light_source_value = 40;
            space->light_source_color[0] = INT64_C(40) * UINT8_MAX;
            space->light_source_color_weight = INT64_C(40) * UINT8_MAX;
        }
    }

    update_los(pl);
    map_client_cache_clear(&CONTR(pl)->cs->lastmap);
    socket_buffer_clear(CONTR(pl)->cs);
    CONTR(pl)->map_update_cmd = MAP_UPDATE_CMD_SAME;
    draw_client_map2(pl);
    ck_assert_uint_gt(validate_queued_map_payloads(CONTR(pl)->cs), 1);

    packet_struct *first = queued_command_payload_find(CONTR(pl)->cs, CLIENT_CMD_MAP);
    ck_assert_ptr_nonnull(first);
    ck_assert_uint_eq(first->data[0], MAP_UPDATE_CMD_SAME);
    ck_assert_uint_ge(first->len, 7);
    uint16_t expected_continuations = ((uint16_t)first->data[4] << 8) | first->data[5];
    ck_assert_uint_gt(expected_continuations, 0);
    bool continuation_found = false;
    uint16_t continuation_sequence = 0;
    for (packet_struct *packet = CONTR(pl)->cs->packets; packet != NULL && packet->next != NULL;
         packet = packet->next) {
        if (packet->type == 0 && packet->len >= 3 &&
            packet->data[packet->len - 1] == CLIENT_CMD_MAP &&
            packet->next->data[0] == MAP_UPDATE_CMD_PARTIAL) {
            continuation_found = true;
            continuation_sequence++;
            ck_assert_uint_ge(packet->next->len, 7);
            ck_assert_uint_eq(((uint16_t)packet->next->data[4] << 8) | packet->next->data[5],
                              continuation_sequence);
            packet = packet->next;
        }
    }
    ck_assert(continuation_found);
    ck_assert_uint_eq(continuation_sequence, expected_continuations);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("request");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_target_packet_includes_current_level_and_plain_name);
    tcase_add_test(tc_core, test_wizardry_level_change_refreshes_spell_cost_once);
    tcase_add_test(tc_core, test_setup_round_trip_uses_current_option_ids);
    tcase_add_test(tc_core, test_setup_rejects_unknown_option);
    tcase_add_test(tc_core, test_initial_setup_completion_is_transactional);
    tcase_add_test(tc_core, test_initial_setup_requires_valid_join_password);
    tcase_add_test(tc_core, test_command_policy_covers_every_connection_phase);
    tcase_add_test(tc_core, test_out_of_order_player_command_is_not_queued);
    tcase_add_test(tc_core, test_only_valid_post_setup_activity_refreshes_login_deadline);
    tcase_add_test(tc_core, test_version_requires_exact_match);
    tcase_add_test(tc_core, test_move_path_walkable_target_reaches_exact_coordinate);
    tcase_add_test(tc_core, test_move_path_blocked_target_stops_at_deterministic_adjacent_tile);
    tcase_add_test(tc_core, test_move_path_occupied_target_stops_at_adjacent_tile);
    tcase_add_test(tc_core, test_move_path_blocks_opaque_passable_target);
    tcase_add_test(tc_core, test_move_path_unreachable_request_clears_existing_queue);
    tcase_add_test(tc_core, test_move_path_center_request_clears_existing_queue);
    tcase_add_test(tc_core, test_move_path_unmapped_request_clears_existing_queue);
    tcase_add_test(tc_core, test_move_path_does_not_queue_partial_search_result);
    tcase_add_test(tc_core, test_move_path_opening_door_retains_queue_for_retry);
    tcase_add_test(tc_core, test_move_path_invalid_request_preserves_existing_queue);
    tcase_add_test(tc_core, test_move_path_new_blockage_stops_without_displacement);
    tcase_add_test(tc_core, test_incuna_unchanged_roof_level_remains_present);
    tcase_add_test(tc_core, test_map_exit_semantic_not_disclosed_by_boundary_geometry);
    tcase_add_test(tc_core, test_map_rgb_cache_tracks_hue_changes_and_neutral_reset);
    tcase_add_test(tc_core, test_map_exit_semantic_tracks_visible_layer_and_cache_changes);
    tcase_add_test(tc_core, test_dense_colored_level_splits_at_tile_boundaries);
    return s;
}

void check_server_request(void) {
    check_run_suite(suite(), __FILE__);
}
