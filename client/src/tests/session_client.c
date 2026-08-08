/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Atrinik Development Team                    *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Integration tests for the graphical-client/session adapter.
 */

#include <global.h>
#include <session_client.h>
#include <toolkit/packet.h>

#include <stdarg.h>

#define CHECK(expression)                                                                  \
    do {                                                                                   \
        if (!(expression)) {                                                               \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
            return false;                                                                  \
        }                                                                                  \
    } while (0)

#define PACKETS_MAX 128
#define PACKET_DATA_MAX 512

typedef struct captured_packet {
    uint8_t type;
    uint8_t data[PACKET_DATA_MAX];
    size_t length;
} captured_packet_t;

Client_Player cpl;
_mapdata MapData;

static captured_packet_t packets[PACKETS_MAX];
static size_t packets_count;
static int map_width = 25;
static int map_height = 25;

int map_get_width(void) {
    return map_width;
}

int map_get_height(void) {
    return map_height;
}

void socket_send_packet(struct packet_struct *packet) {
    HARD_ASSERT(packet != NULL);
    HARD_ASSERT(packets_count < PACKETS_MAX);
    HARD_ASSERT(packet->len <= PACKET_DATA_MAX);

    captured_packet_t *captured = &packets[packets_count++];
    captured->type = packet->type;
    captured->length = packet->len;
    if (packet->len != 0) {
        memcpy(captured->data, packet->data, packet->len);
    }
    packet_free(packet);
}

static captured_packet_t *last_packet(void) {
    HARD_ASSERT(packets_count > 0);
    return &packets[packets_count - 1];
}

static uint32_t packet_uint32(const captured_packet_t *packet, size_t position) {
    HARD_ASSERT(position <= packet->length);
    HARD_ASSERT(packet->length - position >= 4);
    return ((uint32_t)packet->data[position] << 24) | ((uint32_t)packet->data[position + 1] << 16) |
           ((uint32_t)packet->data[position + 2] << 8) | packet->data[position + 3];
}

static void legacy_player_prepare(object *player) {
    memset(&cpl, 0, sizeof(cpl));
    memset(player, 0, sizeof(*player));
    player->tag = 42;
    cpl.ob = player;
    snprintf(cpl.name, sizeof(cpl.name), "Adapter Hero");
    cpl.stats.Str = 10;
    cpl.stats.Dex = 11;
    cpl.stats.Con = 12;
    cpl.stats.Int = 13;
    cpl.stats.Pow = 14;
    cpl.stats.wc = 15;
    cpl.stats.ac = 16;
    cpl.stats.level = 17;
    cpl.stats.hp = 180;
    cpl.stats.maxhp = 200;
    cpl.stats.sp = 90;
    cpl.stats.maxsp = 100;
    cpl.stats.exp = 123456;
    cpl.stats.food = 500;
    cpl.stats.dam = 22;
    cpl.stats.speed = 1.25f;
    cpl.stats.weapon_speed = 0.75f;
    cpl.stats.flags = 3;
    cpl.stats.protection[0] = 25;
    cpl.stats.ranged_dam = 19;
    cpl.stats.ranged_wc = 7;
    cpl.stats.ranged_ws = 0.5f;
    cpl.gen_hp = 0.4f;
    cpl.gen_sp = 0.3f;
    cpl.real_weight = 12.5f;
    cpl.weight_limit = 75.0f;
    cpl.action_timer = 0.25f;
    cpl.gender = GENDER_FEMALE;
    cpl.dm = 1;
    cpl.path_attuned = 1;
    cpl.path_repelled = 2;
    cpl.path_denied = 4;
    cpl.equipment[0] = 100;
}

static bool test_lifecycle_and_legacy_sync(void) {
    object player;
    legacy_player_prepare(&player);
    memset(&MapData, 0, sizeof(MapData));

    client_session_disconnected();
    client_session_character_reset();
    client_session_deinit();
    client_session_init();
    session_t *session = client_session_get();
    CHECK(session == client_session_get());

    client_session_connected(NULL);
    client_session_sync_player();
    CHECK(client_session_select_character("Adapter Hero") == SESSION_ACTION_ACCEPTED);
    CHECK(last_packet()->type == SERVER_CMD_ACCOUNT);
    CHECK(last_packet()->data[0] == CMD_ACCOUNT_LOGIN_CHAR);
    CHECK(strcmp((char *)&last_packet()->data[1], "Adapter Hero") == 0);
    CHECK(client_session_select_character(NULL) == SESSION_ACTION_REJECTED_ARGUMENT);
    CHECK(client_session_select_character("") == SESSION_ACTION_REJECTED_ARGUMENT);

    client_session_playing();
    session_player_t state = {0};
    CHECK(session_player_view(session, &state));
    CHECK(state.id == 42);
    CHECK(strcmp(state.name, "Adapter Hero") == 0);
    CHECK(state.stats.strength == 10);
    CHECK(state.stats.protections[0] == 25);
    CHECK(state.equipment[0] == 100);
    CHECK(state.weight == 12.5f);
    CHECK(state.dm);

    cpl.real_weight = 99.0f;
    cpl.stats.hp = 175;
    client_session_sync_player();
    CHECK(session_player_view(session, &state));
    CHECK(state.stats.hp == 175);
    CHECK(state.weight == 12.5f);

    cpl.target_code = CMD_TARGET_ENEMY;
    cpl.target_level = 21;
    cpl.target_hp = 65;
    cpl.target_is_friend = 1;
    cpl.combat = 1;
    cpl.combat_force = 1;
    snprintf(cpl.target_name, sizeof(cpl.target_name), "Clockwork foe");
    snprintf(cpl.target_color, sizeof(cpl.target_color), "ff0000");
    client_session_sync_target();
    session_snapshot_t snapshot = {0};
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.target.level == 21);
    CHECK(snapshot.target.friend);
    CHECK(snapshot.target.combat_force);
    CHECK(strcmp(snapshot.target.name, "Clockwork foe") == 0);
    CHECK(strcmp(snapshot.target.color, "ff0000") == 0);
    session_snapshot_free(&snapshot);

    MapData.posx = 12;
    MapData.posy = 13;
    MapData.player_sub_layer = 2;
    snprintf(MapData.name, sizeof(MapData.name), "Old map name");
    snprintf(MapData.name_new, sizeof(MapData.name_new), "Current map name");
    snprintf(MapData.map_path, sizeof(MapData.map_path), "/tests/adapter");
    client_session_sync_map(true, 0, 0, 0);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.map.width == map_width);
    CHECK(snapshot.map.height == map_height);
    CHECK(snapshot.map.player_x == 12);
    CHECK(strcmp(snapshot.map.name, "Current map name") == 0);
    session_snapshot_free(&snapshot);

    MapData.name_new[0] = '\0';
    MapData.posx = 13;
    client_session_sync_map(false, 1, 0, 0);
    client_session_sync_map(false, 0, 0, 0);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.map.player_x == 13);
    CHECK(strcmp(snapshot.map.name, "Old map name") == 0);
    session_snapshot_free(&snapshot);

    client_session_character_reset();
    client_session_playing();
    client_session_disconnected();
    client_session_deinit();
    return true;
}

static bool test_world_and_inventory_sync(void) {
    object player;
    legacy_player_prepare(&player);
    client_session_connected("sync.example");
    client_session_playing();

    MapData.posx = 12;
    MapData.posy = 12;
    MapData.player_sub_layer = 0;
    MapData.name_new[0] = '\0';
    snprintf(MapData.name, sizeof(MapData.name), "Adapter arena");
    client_session_sync_map(true, 0, 0, 0);

    client_session_sync_map_entity(0, 3, 4, 0, 0, 1, 0, 100, false, "ignored");
    client_session_sync_map_entity(99, 3, 4, 0, 1, 77, 5, 64, true, NULL);
    client_session_sync_map_cell(3, 4, 0, 1, 0, 55, 7, -2, 180, true, true, true, "Floor");

    session_map_entity_t entity = {0};
    session_handle_t entity_handle = session_map_handle(client_session_get(), 99);
    CHECK(session_map_entity_view(client_session_get(), entity_handle, &entity));
    CHECK(entity.x == 3 && entity.y == 4 && entity.face == 77);
    CHECK(entity.name[0] == '\0');

    client_session_clear_map_layer(3, 4, 0, 1, 0);
    client_session_sync_map_cell(3, 4, 0, 1, 0, 55, 7, -2, 180, true, true, true, "Floor");
    client_session_clear_map_cell(3, 4, 0, 0, false);
    client_session_clear_map_cell(3, 4, 0, 0, true);
    client_session_clear_map_entities(3, 4, 0, 1);
    client_session_sync_map_entity(99, 3, 4, 0, 1, 77, 5, 64, true, "Training foe");
    client_session_remove_map_entity(99);

    object container = {0};
    container.tag = 200;
    container.nrof = 1;
    container.itype = TYPE_CONTAINER;
    snprintf(container.s_name, sizeof(container.s_name), "Satchel");
    client_session_inventory_begin(42, true, true);
    client_session_sync_item(NULL);
    object empty = {0};
    client_session_sync_item(&empty);
    client_session_sync_item(&container);

    object item = {0};
    item.env = &container;
    item.tag = 100;
    item.nrof = 3;
    item.weight = 0.25;
    item.flags = 9;
    item.face = 101;
    item.itype = TYPE_SKILL;
    item.stype = 2;
    item.item_qua = 90;
    item.item_con = 80;
    item.item_level = 17;
    item.direction = 6;
    item.item_skill_tag = 1234;
    snprintf(item.s_name, sizeof(item.s_name), "Arcane technique");
    client_session_sync_item(&item);
    client_session_sync_skill(NULL, 1, 2, "ignored");
    client_session_sync_skill(&empty, 1, 2, "ignored");
    client_session_sync_skill(&item, 18, 987654, NULL);

    session_item_t item_state = {0};
    session_handle_t item_handle = session_item_handle(client_session_get(), 100);
    CHECK(session_item_view(client_session_get(), item_handle, &item_state));
    CHECK(item_state.category == SESSION_ITEM_SKILL);
    CHECK(item_state.ability_level == 18);
    CHECK(item_state.ability_experience == 987654);
    CHECK(item_state.container_id == 200);

    item.itype = TYPE_SPELL;
    client_session_sync_spell(NULL, 1, 2, 3, "ignored");
    client_session_sync_spell(&empty, 1, 2, 3, "ignored");
    client_session_sync_spell(&item, 30, 0x1234, 0x5678, "Spell description");
    CHECK(session_item_view(client_session_get(), item_handle, &item_state));
    CHECK(item_state.category == SESSION_ITEM_SPELL);
    CHECK(item_state.ability_level == 0);
    CHECK(item_state.ability_cost == 30);
    CHECK(strcmp(item_state.description, "Spell description") == 0);

    item.itype = TYPE_FORCE;
    client_session_sync_effect(NULL, 10, "ignored");
    client_session_sync_effect(&empty, 10, "ignored");
    client_session_sync_effect(&item, 45, "Effect description");
    CHECK(session_item_view(client_session_get(), item_handle, &item_state));
    CHECK(item_state.category == SESSION_ITEM_EFFECT);
    CHECK(item_state.ability_cost == 0);
    CHECK(item_state.effect_seconds == 45);

    item.itype = TYPE_POISONING;
    client_session_sync_item(&item);
    CHECK(session_item_view(client_session_get(), item_handle, &item_state));
    CHECK(item_state.category == SESSION_ITEM_EFFECT);

    item.itype = TYPE_WEAPON;
    client_session_sync_item(&item);
    CHECK(session_item_view(client_session_get(), item_handle, &item_state));
    CHECK(item_state.category == SESSION_ITEM_INVENTORY);
    CHECK(item_state.effect_seconds == 0);
    CHECK(item_state.description[0] == '\0');

    client_session_inventory_end(42);
    client_session_remove_item(100);
    CHECK(!session_handle_valid(client_session_get(), item_handle));
    client_session_deinit();
    return true;
}

static bool expect_action_packet(session_action_result_t result, uint8_t type, size_t before) {
    CHECK(result == SESSION_ACTION_ACCEPTED);
    CHECK(packets_count == before + 1);
    CHECK(last_packet()->type == type);
    return true;
}

static bool test_actions_and_packets(void) {
    object player;
    legacy_player_prepare(&player);
    packets_count = 0;
    client_session_connected("actions.example");
    client_session_playing();
    MapData.posx = 12;
    MapData.posy = 12;
    MapData.name[0] = '\0';
    MapData.name_new[0] = '\0';
    client_session_sync_map(true, 0, 0, 0);
    client_session_sync_map_entity(99, 3, 4, 0, 0, 7, 0, 100, false, "Target");

    object container = {0};
    container.tag = 200;
    container.nrof = 1;
    container.itype = TYPE_CONTAINER;
    snprintf(container.s_name, sizeof(container.s_name), "Bag");
    object item = {0};
    item.env = &container;
    item.tag = 100;
    item.nrof = 2;
    item.itype = TYPE_SPELL;
    snprintf(item.s_name, sizeof(item.s_name), "Test spell");
    client_session_sync_item(&container);
    client_session_sync_spell(&item, 10, 0, 0, "Test");

    size_t before = packets_count;
    CHECK(expect_action_packet(client_session_move(2, true, false), SERVER_CMD_MOVE, before));
    CHECK(last_packet()->length == 2);
    CHECK(last_packet()->data[0] == 2 && last_packet()->data[1] == 1);
    before = packets_count;
    CHECK(expect_action_packet(client_session_move(3, false, true), SERVER_CMD_FIRE, before));
    CHECK(last_packet()->length == 1 && last_packet()->data[0] == 3);
    CHECK(client_session_move(9, false, false) == SESSION_ACTION_REJECTED_ARGUMENT);

    before = packets_count;
    CHECK(expect_action_packet(client_session_move_path(5, 6), SERVER_CMD_MOVE_PATH, before));
    CHECK(last_packet()->data[0] == 5 && last_packet()->data[1] == 6);
    CHECK(client_session_move_path(-1, 6) == SESSION_ACTION_REJECTED_ARGUMENT);
    CHECK(client_session_move_path(5, 256) == SESSION_ACTION_REJECTED_ARGUMENT);
    CHECK(client_session_move_path(24, 24) == SESSION_ACTION_ACCEPTED);

    before = packets_count;
    CHECK(expect_action_packet(client_session_stop(), SERVER_CMD_CLEAR, before));
    before = packets_count;
    CHECK(expect_action_packet(client_session_target(99), SERVER_CMD_TARGET, before));
    CHECK(last_packet()->data[0] == CMD_TARGET_MAPXY);
    CHECK(last_packet()->data[1] == 3 && last_packet()->data[2] == 4);
    CHECK(packet_uint32(last_packet(), 3) == 99);
    CHECK(client_session_target(12345) == SESSION_ACTION_REJECTED_STALE_HANDLE);
    before = packets_count;
    CHECK(expect_action_packet(client_session_target(0), SERVER_CMD_TARGET, before));
    CHECK(last_packet()->data[0] == CMD_TARGET_CLEAR);

    CHECK(client_session_target_at(-2, -1, 0) == SESSION_ACTION_REJECTED_ARGUMENT);
    CHECK(client_session_target_at(256, 4, 0) == SESSION_ACTION_REJECTED_ARGUMENT);
    before = packets_count;
    CHECK(expect_action_packet(client_session_target_at(-1, -1, 0), SERVER_CMD_TARGET, before));
    before = packets_count;
    CHECK(expect_action_packet(client_session_target_at(3, 4, 99), SERVER_CMD_TARGET, before));
    CHECK(client_session_target_at(3, 4, 12345) == SESSION_ACTION_REJECTED_STALE_HANDLE);
    before = packets_count;
    CHECK(expect_action_packet(client_session_target_at(3, 4, 0), SERVER_CMD_TARGET, before));

    before = packets_count;
    CHECK(expect_action_packet(client_session_attack(true, true), SERVER_CMD_COMBAT, before));
    CHECK(last_packet()->data[0] == 1 && last_packet()->data[1] == 1);
    before = packets_count;
    CHECK(client_session_controls(true, false) == SESSION_ACTION_ACCEPTED);
    CHECK(packets_count == before);

    before = packets_count;
    CHECK(expect_action_packet(client_session_cast(100, 8), SERVER_CMD_FIRE, before));
    CHECK(packet_uint32(last_packet(), 1) == 100);
    CHECK(client_session_cast(100, 9) == SESSION_ACTION_REJECTED_ARGUMENT);
    CHECK(client_session_cast(12345, 1) == SESSION_ACTION_REJECTED_STALE_HANDLE);

    before = packets_count;
    CHECK(expect_action_packet(client_session_apply(100, CMD_APPLY_ACTION_NORMAL),
                               SERVER_CMD_ITEM_APPLY,
                               before));
    CHECK(last_packet()->length == 4 && packet_uint32(last_packet(), 0) == 100);
    const uint8_t virtual_actions[] = {
        CMD_APPLY_ACTION_NONE,
        CMD_APPLY_ACTION_BELOW_NEXT,
        CMD_APPLY_ACTION_BELOW_PREV,
    };
    for (size_t i = 0; i < sizeof(virtual_actions); i++) {
        before = packets_count;
        CHECK(expect_action_packet(client_session_apply(0, virtual_actions[i]),
                                   SERVER_CMD_ITEM_APPLY,
                                   before));
        CHECK(last_packet()->length == 5);
        CHECK(last_packet()->data[4] == virtual_actions[i]);
    }
    CHECK(client_session_apply(0, 99) == SESSION_ACTION_REJECTED_ARGUMENT);
    CHECK(client_session_apply(12345, 0) == SESSION_ACTION_REJECTED_STALE_HANDLE);

    before = packets_count;
    CHECK(expect_action_packet(client_session_move_item(42, 100, 2, true),
                               SERVER_CMD_ITEM_MOVE,
                               before));
    CHECK(packet_uint32(last_packet(), 0) == 42);
    CHECK(packet_uint32(last_packet(), 4) == 100);
    CHECK(packet_uint32(last_packet(), 8) == 2);
    before = packets_count;
    CHECK(expect_action_packet(client_session_move_item(200, 100, 1, false),
                               SERVER_CMD_ITEM_MOVE,
                               before));
    CHECK(client_session_move_item(100, 100, 1, false) == SESSION_ACTION_REJECTED_ARGUMENT);
    CHECK(client_session_move_item(12345, 100, 1, false) == SESSION_ACTION_REJECTED_STALE_HANDLE);

    before = packets_count;
    CHECK(expect_action_packet(client_session_talk(99), SERVER_CMD_PLAYER_CMD, before));
    CHECK(strcmp((char *)last_packet()->data, "/talk 1 hello") == 0);
    CHECK(client_session_talk(12345) == SESSION_ACTION_REJECTED_STALE_HANDLE);

    CHECK(client_session_reply(0, "hello") == SESSION_ACTION_REJECTED_STALE_HANDLE);
    session_reduce_dialog(client_session_get(), "Guide", "Choose wisely");
    before = packets_count;
    CHECK(expect_action_packet(client_session_reply(0, "hello"), SERVER_CMD_PLAYER_CMD, before));
    CHECK(strcmp((char *)last_packet()->data, "/talk 1 hello") == 0);
    before = packets_count;
    CHECK(expect_action_packet(client_session_reply(7, "second"), SERVER_CMD_PLAYER_CMD, before));
    CHECK(strcmp((char *)last_packet()->data, "/talk 1 reply 7 second") == 0);
    CHECK(client_session_reply(0, NULL) == SESSION_ACTION_REJECTED_ARGUMENT);
    char oversized[SESSION_TEXT_SIZE + 1];
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = '\0';
    CHECK(client_session_reply(0, oversized) == SESSION_ACTION_REJECTED_ARGUMENT);

    before = packets_count;
    CHECK(expect_action_packet(client_session_player_command("/statistics"),
                               SERVER_CMD_PLAYER_CMD,
                               before));
    CHECK(strcmp((char *)last_packet()->data, "/statistics") == 0);
    CHECK(client_session_player_command(NULL) == SESSION_ACTION_REJECTED_ARGUMENT);
    CHECK(client_session_player_command("") == SESSION_ACTION_REJECTED_ARGUMENT);
    CHECK(client_session_player_command(oversized) == SESSION_ACTION_REJECTED_ARGUMENT);

    session_action_t queued = {
        .type = SESSION_ACTION_MOVE,
        .data.move = {.direction = 1, .run = false, .fire = false},
    };
    for (size_t i = 0; i < 17; i++) {
        CHECK(session_action_enqueue(client_session_get(), &queued) == SESSION_ACTION_ACCEPTED);
    }
    before = packets_count;
    client_session_drain_actions();
    CHECK(packets_count == before + 17);

    session_handle_t target_handle = session_map_handle(client_session_get(), 99);
    queued.type = SESSION_ACTION_TARGET;
    memset(&queued.data, 0, sizeof(queued.data));
    queued.data.target.handle = target_handle;
    queued.data.target.x = 3;
    queued.data.target.y = 4;
    CHECK(session_action_enqueue(client_session_get(), &queued) == SESSION_ACTION_ACCEPTED);
    client_session_remove_map_entity(99);
    before = packets_count;
    client_session_drain_actions();
    CHECK(packets_count == before);

    client_session_deinit();
    CHECK(client_session_move_item(0, 100, 1, true) == SESSION_ACTION_REJECTED_CAPABILITY);
    client_session_deinit();
    return true;
}

int main(void) {
    toolkit_import(packet);
    bool passed = test_lifecycle_and_legacy_sync() && test_world_and_inventory_sync() &&
                  test_actions_and_packets();
    client_session_deinit();
    toolkit_deinit();
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
