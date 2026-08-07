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

#include <session.h>

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                  \
    do {                                                                                   \
        if (!(expression)) {                                                               \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
            return false;                                                                  \
        }                                                                                  \
    } while (0)

typedef struct sink_record {
    session_action_t actions[128];
    size_t count;
    bool accept;
} sink_record_t;

static bool record_sink(void *context, const session_action_t *action) {
    sink_record_t *record = context;
    if (!record->accept || record->count == sizeof(record->actions) / sizeof(record->actions[0])) {
        return false;
    }

    record->actions[record->count++] = *action;
    return true;
}

static session_player_t player_make(uint32_t id, const char *name) {
    session_player_t player = {
        .id = id,
        .stats =
            {
                .strength = 10,
                .dexterity = 11,
                .constitution = 12,
                .intelligence = 13,
                .power = 14,
                .level = 2,
                .hp = 20,
                .max_hp = 25,
                .sp = 8,
                .max_sp = 10,
                .speed = 1.0f,
            },
    };
    snprintf(player.name, sizeof(player.name), "%s", name);
    return player;
}

static session_t *playing_session(const session_limits_t *limits, sink_record_t *sink) {
    session_t *session = session_create(limits, record_sink, sink);
    if (session == NULL) {
        return NULL;
    }

    session_reduce_connect(session,
                           SESSION_CAP_GAMEPLAY | SESSION_CAP_SELECT_CHARACTER,
                           "fixture.example");
    session_player_t player = player_make(42, "Tester");
    session_reduce_play(session, &player);
    return session;
}

static bool test_limits(void) {
    session_limits_t limits = session_limits_default();
    CHECK(limits.events > 0);
    session_t *session = session_create(&limits, NULL, NULL);
    CHECK(session != NULL);
    CHECK(session_revision(session) == 0);
    CHECK(session_oldest_event_revision(session) == 0);
    session_destroy(session);

    limits.events = 0;
    CHECK(session_create(&limits, NULL, NULL) == NULL);
    limits = session_limits_default();
    limits.items = 9000;
    CHECK(session_create(&limits, NULL, NULL) == NULL);
    return true;
}

static bool test_lifecycle_and_snapshot(void) {
    sink_record_t sink = {.accept = true};
    session_t *session = session_create(NULL, record_sink, &sink);
    CHECK(session != NULL);

    session_reduce_connect(session, SESSION_CAP_GAMEPLAY, "play.example");
    CHECK(session_revision(session) == 3);
    session_player_t player = player_make(77, "Revision Tester");
    session_reduce_play(session, &player);
    CHECK(session_revision(session) == 5);

    session_snapshot_t snapshot = {0};
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.lifecycle == SESSION_LIFECYCLE_PLAYING);
    CHECK(snapshot.capabilities == SESSION_CAP_GAMEPLAY);
    CHECK(snapshot.player.id == 77);
    CHECK(strcmp(snapshot.player.name, "Revision Tester") == 0);
    CHECK(strcmp(snapshot.server, "play.example") == 0);
    uint64_t generation = snapshot.session_generation;
    session_snapshot_t replay = snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    session_reduce_disconnect(session);
    session_event_t disconnect_event;
    size_t event_count = 0;
    CHECK(session_events_read(session, replay.revision, &disconnect_event, 1, &event_count) ==
          SESSION_EVENTS_OK);
    CHECK(event_count == 1);
    CHECK(session_snapshot_apply_event(&replay, &disconnect_event));
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.lifecycle == SESSION_LIFECYCLE_DISCONNECTED);
    CHECK(snapshot.capabilities == 0);
    CHECK(snapshot.player.id == 0);
    CHECK(snapshot.intent.pending_actions == 0);
    CHECK(snapshot.session_generation > generation);
    CHECK(replay.session_generation == snapshot.session_generation);
    CHECK(replay.lifecycle == snapshot.lifecycle);
    CHECK(replay.player.id == snapshot.player.id);
    CHECK(replay.items_count == snapshot.items_count);
    session_snapshot_free(&replay);
    session_snapshot_free(&snapshot);
    session_destroy(session);
    return true;
}

static bool test_player_target_and_events(void) {
    sink_record_t sink = {.accept = true};
    session_t *session = playing_session(NULL, &sink);
    CHECK(session != NULL);
    uint64_t start = session_revision(session);

    session_player_t player = player_make(42, "Tester");
    player.stats.hp = 9;
    player.stats.experience = 12345;
    session_reduce_player(session, &player);

    session_map_t map = {.width = 25, .height = 25, .player_x = 12, .player_y = 12};
    snprintf(map.name, sizeof(map.name), "Recorded Hall");
    session_reduce_map_reset(session, &map);
    session_map_entity_t entity = {
        .id = 99,
        .x = 14,
        .y = 12,
        .sub_layer = 0,
        .face = 7,
        .hp = 80,
    };
    snprintf(entity.name, sizeof(entity.name), "Training target");
    CHECK(session_reduce_map_entity(session, &entity));
    session_target_t target = {.id = 99, .code = 2, .level = 4, .hp = 80};
    snprintf(target.name, sizeof(target.name), "Training target");
    session_reduce_target(session, &target);

    session_event_t events[8];
    size_t count = 0;
    CHECK(session_events_read(session, start, events, 8, &count) == SESSION_EVENTS_OK);
    CHECK(count == 4);
    CHECK(events[0].type == SESSION_EVENT_PLAYER);
    CHECK(events[0].data.player.stats.hp == 9);
    CHECK(events[1].type == SESSION_EVENT_MAP_RESET);
    CHECK(strcmp(events[1].data.map.name, "Recorded Hall") == 0);
    CHECK(events[2].type == SESSION_EVENT_MAP_ENTITY_UPSERT);
    CHECK(events[2].data.map_entity.id == 99);
    CHECK(events[3].type == SESSION_EVENT_TARGET);

    session_snapshot_t snapshot = {0};
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.revision == events[3].revision);
    CHECK(snapshot.player.stats.hp == events[0].data.player.stats.hp);
    CHECK(strcmp(snapshot.map.name, events[1].data.map.name) == 0);
    CHECK(snapshot.map_entities_count == 1);
    CHECK(snapshot.map_entities[0].generation == events[2].data.map_entity.generation);
    CHECK(snapshot.target.id == events[3].data.target.id);
    session_snapshot_free(&snapshot);
    session_destroy(session);
    return true;
}

static bool test_map_handles_and_scroll(void) {
    sink_record_t sink = {.accept = true};
    session_t *session = playing_session(NULL, &sink);
    CHECK(session != NULL);
    session_map_t map = {.width = 25, .height = 25, .player_x = 12, .player_y = 12};
    session_reduce_map_reset(session, &map);

    session_map_cell_t floor = {
        .x = 13,
        .y = 12,
        .layer = 1,
        .face = 17,
    };
    CHECK(session_reduce_map_cell(session, &floor));

    session_map_entity_t entity = {.id = 10, .x = 13, .y = 12, .face = 5};
    CHECK(session_reduce_map_entity(session, &entity));
    session_handle_t first = session_map_handle(session, 10);
    CHECK(first.kind == SESSION_HANDLE_MAP_ENTITY);
    CHECK(session_handle_valid(session, first));

    session_snapshot_t snapshot = {0};
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.map_cells_count == 1);
    CHECK(snapshot.map_cells[0].face == 17);
    session_snapshot_free(&snapshot);

    entity.hp = 50;
    CHECK(session_reduce_map_entity(session, &entity));
    session_handle_t updated = session_map_handle(session, 10);
    CHECK(updated.object_generation == first.object_generation);
    CHECK(session_handle_valid(session, first));

    CHECK(session_reduce_map_entity_remove(session, 10));
    CHECK(!session_handle_valid(session, first));
    CHECK(session_reduce_map_entity(session, &entity));
    session_handle_t reused = session_map_handle(session, 10);
    CHECK(reused.object_generation != first.object_generation);

    map.player_x++;
    session_reduce_map_scroll(session, 1, 0, 0, &map);
    CHECK(!session_handle_valid(session, reused));
    session_handle_t scrolled = session_map_handle(session, 10);
    CHECK(scrolled.kind == SESSION_HANDLE_MAP_ENTITY);
    CHECK(scrolled.collection_generation != reused.collection_generation);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.map_cells_count == 1);
    CHECK(snapshot.map_cells[0].x == 12);
    CHECK(snapshot.map_entities[0].x == 12);
    session_snapshot_free(&snapshot);
    session_destroy(session);
    return true;
}

static bool test_inventory_replay_and_handles(void) {
    session_limits_t limits = session_limits_default();
    limits.items = 3;
    sink_record_t sink = {.accept = true};
    session_t *session = playing_session(&limits, &sink);
    CHECK(session != NULL);

    session_reduce_inventory_begin(session, 42, true, false);
    session_item_t item = {.id = 100, .container_id = 42, .quantity = 2, .face = 11};
    snprintf(item.name, sizeof(item.name), "Potion");
    CHECK(session_reduce_item(session, &item));
    session_reduce_inventory_end(session, 42);
    session_handle_t handle = session_item_handle(session, 100);
    CHECK(session_handle_valid(session, handle));

    item.quantity = 3;
    CHECK(session_reduce_item(session, &item));
    CHECK(session_handle_valid(session, handle));
    CHECK(session_reduce_item_remove(session, 100));
    CHECK(!session_handle_valid(session, handle));

    CHECK(session_reduce_item(session, &item));
    session_handle_t reused = session_item_handle(session, 100);
    CHECK(reused.object_generation != handle.object_generation);
    session_reduce_inventory_begin(session, 42, true, false);
    session_reduce_inventory_end(session, 42);
    CHECK(!session_handle_valid(session, reused));

    session_item_t container = {.id = 10, .container_id = 42};
    session_item_t child = {.id = 11, .container_id = 10};
    CHECK(session_reduce_item(session, &container));
    CHECK(session_reduce_item(session, &child));
    container.container_id = 11;
    CHECK(!session_reduce_item(session, &container));
    session_handle_t child_handle = session_item_handle(session, 11);
    session_reduce_inventory_begin(session, 10, false, true);
    session_reduce_inventory_end(session, 10);
    CHECK(session_reduce_item_remove(session, 10));
    CHECK(!session_handle_valid(session, child_handle));
    session_snapshot_t removed_tree = {0};
    CHECK(session_snapshot_copy(session, &removed_tree));
    CHECK(removed_tree.items_count == 0);
    CHECK(removed_tree.open_container.id == 0);
    session_snapshot_free(&removed_tree);
    container.container_id = 42;
    CHECK(session_reduce_item(session, &container));
    CHECK(session_reduce_item(session, &child));
    session_reduce_inventory_begin(session, 10, false, true);
    session_reduce_inventory_end(session, 10);
    session_reduce_inventory_begin(session, 42, true, false);
    session_reduce_inventory_end(session, 42);
    session_snapshot_t cleared = {0};
    CHECK(session_snapshot_copy(session, &cleared));
    CHECK(cleared.items_count == 2);
    CHECK(cleared.open_container.id == 10);
    session_snapshot_free(&cleared);
    session_reduce_inventory_begin(session, 10, true, false);
    session_reduce_inventory_end(session, 10);
    CHECK(session_reduce_item_remove(session, 10));

    session_item_t first = {.id = 1, .container_id = 42};
    session_item_t second = {.id = 2, .container_id = 42};
    session_item_t third = {.id = 3, .container_id = 42};
    session_item_t overflow = {.id = 4, .container_id = 42};
    CHECK(session_reduce_item(session, &third));
    CHECK(session_reduce_item(session, &first));
    CHECK(session_reduce_item(session, &second));
    CHECK(!session_reduce_item(session, &overflow));

    session_snapshot_t snapshot = {0};
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.items_count == 3);
    CHECK(snapshot.items[0].id == 1);
    CHECK(snapshot.items[1].id == 2);
    CHECK(snapshot.items[2].id == 3);
    session_snapshot_free(&snapshot);
    session_destroy(session);
    return true;
}

static bool test_documents_messages_and_bounding(void) {
    session_limits_t limits = session_limits_default();
    limits.messages = 2;
    sink_record_t sink = {.accept = true};
    session_t *session = playing_session(&limits, &sink);
    CHECK(session != NULL);

    session_reduce_dialog(session, "Innkeeper", "Welcome");
    session_reduce_dialog(session, "Innkeeper", "A room?");
    session_reduce_quest(session, "First Steps", "Talk to the guard");
    session_reduce_message(session, 1, "ffffff", "one");
    session_reduce_message(session, 2, "00ff00", "two");
    session_reduce_message(session, 3, "ff0000", "three");
    session_reduce_party(session, "Reviewers");
    session_reduce_party_members_clear(session);
    session_party_member_t member = {.hp = 80, .sp = 60};
    snprintf(member.name, sizeof(member.name), "Alice");
    CHECK(session_reduce_party_member(session, &member));
    member.hp = 70;
    CHECK(session_reduce_party_member(session, &member));

    session_snapshot_t snapshot = {0};
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.dialog.generation == 2);
    CHECK(strcmp(snapshot.dialog.text, "A room?") == 0);
    CHECK(snapshot.quest.generation == 1);
    CHECK(snapshot.messages_count == 2);
    CHECK(strcmp(snapshot.messages[0].text, "two") == 0);
    CHECK(strcmp(snapshot.messages[1].text, "three") == 0);
    CHECK(snapshot.messages[1].revision <= snapshot.revision);
    CHECK(strcmp(snapshot.party, "Reviewers") == 0);
    CHECK(snapshot.party_members_count == 1);
    CHECK(snapshot.party_members[0].hp == 70);
    CHECK(session_reduce_party_member_remove(session, "Alice"));
    session_snapshot_free(&snapshot);
    session_destroy(session);
    return true;
}

static bool test_reducer_boundaries_and_views(void) {
    session_limits_t limits = session_limits_default();
    limits.events = 64;
    limits.map_cells = 2;
    limits.map_entities = 3;
    limits.items = 3;
    sink_record_t sink = {.accept = true};
    session_t *session = playing_session(&limits, &sink);
    CHECK(session != NULL);

    session_player_t player = player_make(42, "Tester");
    memset(player.name, 'p', sizeof(player.name));
    session_reduce_player(session, &player);
    CHECK(session_player_view(session, &player));
    CHECK(player.name[sizeof(player.name) - 1] == '\0');

    session_map_t map = {0};
    uint64_t revision = session_revision(session);
    session_reduce_map_reset(session, &map);
    CHECK(session_revision(session) == revision);
    map = (session_map_t){.width = 8, .height = 8, .player_x = 4, .player_y = 4};
    memset(map.name, 'm', sizeof(map.name));
    memset(map.path, 'q', sizeof(map.path));
    session_reduce_map_reset(session, &map);
    CHECK(session_map_view(session, &map));
    CHECK(map.name[sizeof(map.name) - 1] == '\0');
    CHECK(map.path[sizeof(map.path) - 1] == '\0');
    map.player_sub_layer = 2;
    session_reduce_map(session, &map);
    CHECK(session_map_view(session, &map));
    CHECK(map.player_sub_layer == 2);
    revision = session_revision(session);
    map.width--;
    session_reduce_map(session, &map);
    CHECK(session_revision(session) == revision);
    map.width++;

    session_map_cell_t first = {
        .x = 2,
        .y = 3,
        .layer = 1,
        .face = 10,
        .light_level = 4,
        .light_known = true,
        .fogged = true,
        .fog_known = true,
    };
    CHECK(session_reduce_map_cell(session, &first));
    session_map_cell_t merged = first;
    merged.face = 11;
    merged.light_level = 0;
    merged.light_known = false;
    merged.fogged = false;
    merged.fog_known = false;
    CHECK(session_reduce_map_cell(session, &merged));
    session_map_cell_t second = {.x = 1, .y = 1, .layer = 1, .face = 20};
    session_map_cell_t overflow = {.x = 4, .y = 4, .layer = 1, .face = 30};
    CHECK(session_reduce_map_cell(session, &second));
    CHECK(!session_reduce_map_cell(session, &overflow));
    overflow.x = 8;
    CHECK(!session_reduce_map_cell(session, &overflow));

    session_snapshot_t snapshot = {0};
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.map_cells_count == 2);
    CHECK(snapshot.map_cells[1].face == 11);
    CHECK(snapshot.map_cells[1].light_known);
    CHECK(snapshot.map_cells[1].light_level == 4);
    CHECK(snapshot.map_cells[1].fog_known);
    CHECK(snapshot.map_cells[1].fogged);
    session_snapshot_free(&snapshot);
    CHECK(session_reduce_map_cell_soft_clear(session, 2, 3, 0) == 1);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.map_cells_count == 2);
    CHECK(snapshot.map_cells[1].face == 11);
    CHECK(!snapshot.map_cells[1].light_known);
    CHECK(snapshot.map_cells[1].fog_known);
    CHECK(snapshot.map_cells[1].fogged);
    session_snapshot_free(&snapshot);
    CHECK(session_reduce_map_layer_clear(session, 2, 3, 0, 1, 0));
    CHECK(!session_reduce_map_layer_clear(session, 2, 3, 0, 1, 0));
    CHECK(session_reduce_map_cell_soft_clear(session, 1, 1, 0) == 2);
    CHECK(session_reduce_map_cell_clear(session, 1, 1, 0, -1) == 2);
    CHECK(session_reduce_map_cell(session, &second));

    session_map_entity_t entity = {.id = 30, .x = 4, .y = 4, .sub_layer = 1};
    CHECK(session_reduce_map_entity(session, &entity));
    session_handle_t replaced = session_map_handle(session, 30);
    entity.id = 31;
    CHECK(session_reduce_map_entity(session, &entity));
    CHECK(!session_handle_valid(session, replaced));
    session_handle_t current = session_map_handle_at(session, 4, 4);
    CHECK(current.id == 31);
    CHECK(session_reduce_map_entities_clear(session, 4, 4, 0, 1) == 1);
    CHECK(!session_handle_valid(session, current));
    session_reduce_map_scroll(session, INT_MIN, 0, 0, &map);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.map_cells_count == 0);
    CHECK(snapshot.map_entities_count == 0);
    session_snapshot_free(&snapshot);

    session_reduce_inventory_begin(session, 42, true, false);
    session_item_t item = {
        .id = 5,
        .container_id = 42,
        .quantity = 2,
        .weight = 1.25,
        .category = SESSION_ITEM_EFFECT,
        .quality = 90,
        .condition = 75,
        .required_level = 4,
        .required_skill_id = 99,
    };
    memset(item.name, 'i', sizeof(item.name));
    memset(item.description, 'd', sizeof(item.description));
    CHECK(session_reduce_item(session, &item));
    session_reduce_inventory_end(session, 42);
    session_reduce_inventory_begin(session, 5, false, true);
    session_reduce_inventory_end(session, 5);
    session_handle_t item_handle = session_item_handle(session, 5);
    session_item_t item_view;
    CHECK(session_item_view(session, item_handle, &item_view));
    CHECK(item_view.name[sizeof(item_view.name) - 1] == '\0');
    CHECK(item_view.description[sizeof(item_view.description) - 1] == '\0');
    CHECK(item_view.weight == 1.25);
    CHECK(item_view.quality == 90);
    CHECK(item_view.required_skill_id == 99);
    CHECK(session_player_view(session, &player));
    CHECK(player.weight == 2.5f);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.open_container.id == 5);
    session_snapshot_free(&snapshot);
    session_reduce_inventory_begin(session, 5, false, false);
    session_reduce_inventory_end(session, 5);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.open_container.id == 0);
    CHECK(snapshot.items_count == 1);
    session_snapshot_free(&snapshot);
    item.category = (session_item_category_t)99;
    CHECK(!session_reduce_item(session, &item));

    session_reduce_dialog(session, "Dialog", "Text");
    session_reduce_quest(session, "Quest", "Objective");
    session_document_t document;
    CHECK(session_dialog_view(session, &document));
    CHECK(strcmp(document.title, "Dialog") == 0);
    CHECK(session_quest_view(session, &document));
    CHECK(strcmp(document.title, "Quest") == 0);
    session_reduce_party(session, "Core Team");
    char party[SESSION_NAME_SIZE];
    CHECK(session_party_name_view(session, party, sizeof(party)));
    CHECK(strcmp(party, "Core Team") == 0);
    CHECK(!session_party_name_view(session, party, 0));
    session_reduce_server(session, "new.example");

    session_reduce_character_reset(session);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.lifecycle == SESSION_LIFECYCLE_CONNECTED);
    CHECK(snapshot.player.id == 0);
    CHECK(snapshot.map_cells_count == 0);
    CHECK(snapshot.items_count == 0);
    CHECK(strcmp(snapshot.server, "new.example") == 0);
    session_snapshot_free(&snapshot);
    session_destroy(session);
    return true;
}

static bool test_actions(void) {
    sink_record_t sink = {.accept = true};
    session_t *session = playing_session(NULL, &sink);
    CHECK(session != NULL);

    session_map_t map = {.width = 25, .height = 25};
    session_reduce_map_reset(session, &map);
    session_map_entity_t elevated = {.id = 10, .x = 4, .y = 5, .depth = 1, .sub_layer = 2};
    CHECK(session_reduce_map_entity(session, &elevated));
    session_map_entity_t entity = {.id = 9, .x = 4, .y = 5};
    CHECK(session_reduce_map_entity(session, &entity));
    CHECK(session_map_handle_at(session, 4, 5).id == 9);
    session_reduce_inventory_begin(session, 42, true, false);
    session_item_t item = {.id = 8, .container_id = 42, .quantity = 1};
    CHECK(session_reduce_item(session, &item));
    session_reduce_inventory_end(session, 42);

    session_action_t move = {
        .type = SESSION_ACTION_MOVE,
        .data.move = {.direction = 3, .run = true},
    };
    CHECK(session_action_dispatch(session, &move) == SESSION_ACTION_ACCEPTED);
    CHECK(sink.count == 1);

    session_action_t target = {
        .type = SESSION_ACTION_TARGET,
        .data.target =
            {
                .handle = session_map_handle(session, 9),
                .x = 4,
                .y = 5,
            },
    };
    CHECK(session_action_dispatch(session, &target) == SESSION_ACTION_ACCEPTED);
    session_action_t apply = {
        .type = SESSION_ACTION_APPLY,
        .data.item = {.handle = session_item_handle(session, 8), .quantity = 1},
    };
    CHECK(session_action_dispatch(session, &apply) == SESSION_ACTION_ACCEPTED);
    session_action_t virtual_apply = {
        .type = SESSION_ACTION_APPLY,
        .data.item = {.virtual_item = true, .virtual_action = SESSION_VIRTUAL_ITEM_NEXT},
    };
    CHECK(session_action_dispatch(session, &virtual_apply) == SESSION_ACTION_ACCEPTED);
    CHECK(sink.actions[sink.count - 1].data.item.handle.id == 0);
    CHECK(sink.actions[sink.count - 1].data.item.virtual_action == SESSION_VIRTUAL_ITEM_NEXT);

    session_snapshot_t snapshot = {0};
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.intent.movement_held);
    CHECK(snapshot.intent.movement_direction == 3);
    CHECK(snapshot.intent.run);
    CHECK(snapshot.intent.target.id == 9);
    session_snapshot_free(&snapshot);

    session_reduce_map_scroll(session, 1, 0, 0, &map);
    CHECK(session_action_dispatch(session, &target) == SESSION_ACTION_REJECTED_STALE_HANDLE);
    session_action_t invalid_move = move;
    invalid_move.data.move.direction = 9;
    CHECK(session_action_dispatch(session, &invalid_move) == SESSION_ACTION_REJECTED_ARGUMENT);

    sink.accept = false;
    session_action_t stop = {.type = SESSION_ACTION_STOP};
    CHECK(session_action_dispatch(session, &stop) == SESSION_ACTION_REJECTED_SINK);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.intent.movement_held);
    session_snapshot_free(&snapshot);

    sink.accept = true;
    session_reduce_capabilities(session, SESSION_CAP_MOVE);
    CHECK(session_action_dispatch(session, &apply) == SESSION_ACTION_REJECTED_CAPABILITY);
    CHECK(session_action_dispatch(session, &stop) == SESSION_ACTION_ACCEPTED);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(!snapshot.intent.movement_held);
    CHECK(!snapshot.intent.run);
    session_snapshot_free(&snapshot);
    session_destroy(session);
    return true;
}

static bool test_all_semantic_actions_and_validation(void) {
    sink_record_t sink = {.accept = true};
    session_t *session = playing_session(NULL, &sink);
    CHECK(session != NULL);
    session_action_t no_dialog_reply = {.type = SESSION_ACTION_REPLY};
    CHECK(session_action_dispatch(session, &no_dialog_reply) ==
          SESSION_ACTION_REJECTED_STALE_HANDLE);
    session_map_t map = {.width = 16, .height = 16};
    session_reduce_map_reset(session, &map);
    session_map_entity_t entity = {.id = 90, .x = 3, .y = 4};
    CHECK(session_reduce_map_entity(session, &entity));
    session_reduce_inventory_begin(session, 42, true, false);
    session_item_t item = {.id = 80, .container_id = 42, .quantity = 2};
    CHECK(session_reduce_item(session, &item));
    session_item_t container = {.id = 70, .container_id = 42};
    CHECK(session_reduce_item(session, &container));
    session_reduce_inventory_end(session, 42);
    session_reduce_dialog(session, "Question", "Choose");

    session_action_t actions[] = {
        {.type = SESSION_ACTION_MOVE_PATH, .data.point = {.x = 6, .y = 7}},
        {.type = SESSION_ACTION_ATTACK, .data.attack = {.enabled = true, .force = true}},
        {.type = SESSION_ACTION_CAST,
         .data.cast = {.handle = session_item_handle(session, 80), .direction = 8}},
        {.type = SESSION_ACTION_GET,
         .data.item = {.handle = session_item_handle(session, 80),
                       .container = session_item_handle(session, 70),
                       .container_id = 70,
                       .quantity = 1}},
        {.type = SESSION_ACTION_DROP,
         .data.item = {.handle = session_item_handle(session, 80),
                       .container_id = 0,
                       .quantity = 1}},
        {.type = SESSION_ACTION_TALK, .data.handle = session_map_handle(session, 90)},
        {.type = SESSION_ACTION_CONTROL, .data.control = {.run = true, .fire = true}},
    };
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        CHECK(session_action_dispatch(session, &actions[i]) == SESSION_ACTION_ACCEPTED);
    }

    session_document_t dialog;
    CHECK(session_dialog_view(session, &dialog));
    session_action_t reply = {
        .type = SESSION_ACTION_REPLY,
        .data.reply = {.reply = 2, .dialog_generation = dialog.generation},
    };
    snprintf(reply.data.reply.text, sizeof(reply.data.reply.text), "second");
    CHECK(session_action_dispatch(session, &reply) == SESSION_ACTION_ACCEPTED);

    session_action_t player_command = {.type = SESSION_ACTION_PLAYER_COMMAND};
    snprintf(player_command.data.text, sizeof(player_command.data.text), "/party list");
    CHECK(session_action_dispatch(session, &player_command) == SESSION_ACTION_ACCEPTED);
    CHECK(sink.count == sizeof(actions) / sizeof(actions[0]) + 2);

    session_action_t invalid = {.type = (session_action_type_t)999};
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_ARGUMENT);
    invalid = actions[0];
    invalid.data.point.x = -1;
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_ARGUMENT);
    invalid = actions[0];
    invalid.data.point.x = 16;
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_ARGUMENT);
    invalid = actions[2];
    invalid.data.cast.direction = 9;
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_ARGUMENT);
    invalid = (session_action_t){.type = SESSION_ACTION_TARGET, .data.target = {.x = 256, .y = 0}};
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_ARGUMENT);
    invalid = (session_action_t){
        .type = SESSION_ACTION_TARGET,
        .data.target = {.handle = session_map_handle(session, 90), .clear = true}};
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_ARGUMENT);
    invalid = (session_action_t){
        .type = SESSION_ACTION_TARGET,
        .data.target = {.handle = session_map_handle(session, 90), .x = 2, .y = 4}};
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_ARGUMENT);
    invalid = (session_action_t){
        .type = SESSION_ACTION_APPLY,
        .data.item = {.handle = session_item_handle(session, 80), .virtual_item = true}};
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_ARGUMENT);
    invalid = (session_action_t){
        .type = SESSION_ACTION_APPLY,
        .data.item = {.virtual_action = (session_virtual_item_action_t)99, .virtual_item = true}};
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_ARGUMENT);
    invalid = actions[3];
    memset(&invalid.data.item.container, 0, sizeof(invalid.data.item.container));
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_STALE_HANDLE);
    invalid = actions[3];
    invalid.data.item.container = invalid.data.item.handle;
    invalid.data.item.container_id = invalid.data.item.handle.id;
    CHECK(session_action_dispatch(session, &invalid) == SESSION_ACTION_REJECTED_ARGUMENT);
    memset(player_command.data.text, 'x', sizeof(player_command.data.text));
    CHECK(session_action_dispatch(session, &player_command) == SESSION_ACTION_REJECTED_ARGUMENT);

    session_action_t queued_item = actions[3];
    CHECK(session_action_enqueue(session, &queued_item) == SESSION_ACTION_ACCEPTED);
    session_reduce_inventory_begin(session, 42, true, false);
    session_reduce_inventory_end(session, 42);
    session_action_result_t queued_result;
    CHECK(session_actions_drain(session, &queued_result, 1) == 1);
    CHECK(queued_result == SESSION_ACTION_REJECTED_STALE_HANDLE);

    session_destroy(session);

    session = session_create(NULL, record_sink, &sink);
    CHECK(session != NULL);
    session_reduce_connect(session, SESSION_CAP_SELECT_CHARACTER, "select.example");
    session_action_t select = {.type = SESSION_ACTION_SELECT_CHARACTER};
    snprintf(select.data.text, sizeof(select.data.text), "Tester");
    size_t before_select = sink.count;
    CHECK(session_action_dispatch(session, &select) == SESSION_ACTION_ACCEPTED);
    CHECK(sink.count == before_select + 1);
    select.data.text[0] = '\0';
    CHECK(session_action_dispatch(session, &select) == SESSION_ACTION_REJECTED_ARGUMENT);
    session_player_t player = player_make(42, "Tester");
    session_reduce_play(session, &player);
    snprintf(select.data.text, sizeof(select.data.text), "Tester");
    CHECK(session_action_dispatch(session, &select) == SESSION_ACTION_REJECTED_LIFECYCLE);
    session_destroy(session);
    return true;
}

static bool test_action_queue_and_disconnect(void) {
    session_limits_t limits = session_limits_default();
    limits.actions = 2;
    sink_record_t sink = {.accept = true};
    session_t *session = playing_session(&limits, &sink);
    CHECK(session != NULL);

    session_action_t first = {
        .type = SESSION_ACTION_MOVE,
        .data.move = {.direction = 1, .run = true, .fire = true},
    };
    session_action_t second = {.type = SESSION_ACTION_STOP};
    CHECK(session_action_enqueue(session, &first) == SESSION_ACTION_ACCEPTED);
    CHECK(session_action_enqueue(session, &second) == SESSION_ACTION_ACCEPTED);
    CHECK(session_action_enqueue(session, &first) == SESSION_ACTION_REJECTED_QUEUE_FULL);

    session_action_result_t results[2];
    uint64_t before_drain = session_revision(session);
    CHECK(session_actions_drain(session, results, 1) == 1);
    CHECK(results[0] == SESSION_ACTION_ACCEPTED);
    CHECK(session_revision(session) == before_drain + 1);
    session_snapshot_t snapshot = {0};
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.intent.pending_actions == 1);
    CHECK(snapshot.intent.fire);
    CHECK(!snapshot.intent.movement_held);
    session_snapshot_free(&snapshot);

    session_reduce_disconnect(session);
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.intent.pending_actions == 0);
    CHECK(!snapshot.intent.movement_held);
    CHECK(!snapshot.intent.fire);
    CHECK(!snapshot.intent.run);
    CHECK(!snapshot.intent.combat);
    CHECK(snapshot.intent.target.kind == SESSION_HANDLE_NONE);
    session_snapshot_free(&snapshot);
    CHECK(session_actions_drain(session, results, 2) == 0);
    CHECK(session_action_dispatch(session, &second) == SESSION_ACTION_REJECTED_CAPABILITY);
    session_destroy(session);
    return true;
}

static bool test_revision_gap(void) {
    session_limits_t limits = session_limits_default();
    limits.events = 3;
    sink_record_t sink = {.accept = true};
    session_t *session = session_create(&limits, record_sink, &sink);
    CHECK(session != NULL);

    session_reduce_connect(session, SESSION_CAP_GAMEPLAY, "gap.example");
    session_player_t player = player_make(1, "Gap");
    session_reduce_play(session, &player);
    session_reduce_party(session, "one");
    session_reduce_party(session, "two");

    session_event_t events[3];
    size_t count = 123;
    CHECK(session_oldest_event_revision(session) > 1);
    CHECK(session_events_read(session, 0, events, 3, &count) == SESSION_EVENTS_GAP);
    CHECK(count == 0);

    uint64_t oldest = session_oldest_event_revision(session);
    CHECK(session_events_read(session, oldest - 1, events, 3, &count) == SESSION_EVENTS_OK);
    CHECK(count == 3);
    CHECK(events[0].revision == oldest);
    CHECK(session_events_read(session, session_revision(session) + 1, events, 3, &count) ==
          SESSION_EVENTS_INVALID);
    session_destroy(session);
    return true;
}

static bool test_dialog_replacement_rejects_queued_reply(void) {
    sink_record_t sink = {.accept = true};
    session_t *session = playing_session(NULL, &sink);
    CHECK(session != NULL);
    session_reduce_dialog(session, "First", "Choose");
    session_document_t dialog;
    CHECK(session_dialog_view(session, &dialog));

    session_action_t reply = {.type = SESSION_ACTION_REPLY};
    reply.data.reply.dialog_generation = dialog.generation;
    snprintf(reply.data.reply.text, sizeof(reply.data.reply.text), "yes");
    CHECK(session_action_enqueue(session, &reply) == SESSION_ACTION_ACCEPTED);
    session_reduce_dialog(session, "Second", "The first choice expired");

    session_action_result_t result;
    CHECK(session_actions_drain(session, &result, 1) == 1);
    CHECK(result == SESSION_ACTION_REJECTED_STALE_HANDLE);
    CHECK(sink.count == 0);
    session_destroy(session);
    return true;
}

static bool test_recorded_stream(void) {
    char path[1024];
    snprintf(path,
             sizeof(path),
             "%s/src/tests/fixtures/session/recorded_stream.txt",
             ATRINIK_TEST_SOURCE_DIR);
    FILE *fp = fopen(path, "r");
    CHECK(fp != NULL);

    sink_record_t sink = {.accept = true};
    session_t *session = session_create(NULL, record_sink, &sink);
    CHECK(session != NULL);
    char line[2048];
    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        if (strncmp(line, "connect ", 8) == 0) {
            session_reduce_connect(session, SESSION_CAP_GAMEPLAY, line + 8);
        } else if (strncmp(line, "play ", 5) == 0) {
            unsigned int id;
            char name[SESSION_CHARACTER_SIZE];
            CHECK(sscanf(line + 5, "%u %39s", &id, name) == 2);
            session_player_t player = player_make(id, name);
            session_reduce_play(session, &player);
        } else if (strncmp(line, "player ", 7) == 0) {
            session_player_t player;
            CHECK(session_player_view(session, &player));
            CHECK(sscanf(line + 7,
                         "%" SCNd32 " %" SCNd32 " %" SCNd64,
                         &player.stats.hp,
                         &player.stats.max_hp,
                         &player.stats.experience) == 3);
            session_reduce_player(session, &player);
        } else if (strncmp(line, "map ", 4) == 0) {
            session_map_t map = {0};
            CHECK(sscanf(line + 4,
                         "%d %d %d %d %255s",
                         &map.width,
                         &map.height,
                         &map.player_x,
                         &map.player_y,
                         map.name) == 5);
            session_reduce_map_reset(session, &map);
        } else if (strncmp(line, "cell ", 5) == 0) {
            session_map_cell_t cell = {0};
            unsigned int layer;
            unsigned int face;
            CHECK(sscanf(line + 5,
                         "%" SCNd16 " %" SCNd16 " %" SCNd8 " %u %u",
                         &cell.x,
                         &cell.y,
                         &cell.depth,
                         &layer,
                         &face) == 5);
            cell.layer = (uint8_t)layer;
            cell.face = (uint16_t)face;
            CHECK(session_reduce_map_cell(session, &cell));
        } else if (strncmp(line, "entity ", 7) == 0) {
            session_map_entity_t entity = {0};
            unsigned int sub_layer;
            unsigned int face;
            CHECK(sscanf(line + 7,
                         "%" SCNu32 " %" SCNd16 " %" SCNd16 " %" SCNd8 " %u %u %127s",
                         &entity.id,
                         &entity.x,
                         &entity.y,
                         &entity.depth,
                         &sub_layer,
                         &face,
                         entity.name) == 7);
            entity.sub_layer = (uint8_t)sub_layer;
            entity.face = (uint16_t)face;
            CHECK(session_reduce_map_entity(session, &entity));
        } else if (strncmp(line, "inventory_begin ", 16) == 0) {
            unsigned int container;
            CHECK(sscanf(line + 16, "%u", &container) == 1);
            session_reduce_inventory_begin(session, container, true, false);
        } else if (strncmp(line, "item ", 5) == 0) {
            session_item_t item = {0};
            unsigned int face;
            CHECK(sscanf(line + 5,
                         "%" SCNu32 " %" SCNu32 " %" SCNu32 " %u %127s",
                         &item.id,
                         &item.container_id,
                         &item.quantity,
                         &face,
                         item.name) == 5);
            item.face = (uint16_t)face;
            CHECK(session_reduce_item(session, &item));
        } else if (strncmp(line, "inventory_end ", 14) == 0) {
            unsigned int container;
            CHECK(sscanf(line + 14, "%u", &container) == 1);
            session_reduce_inventory_end(session, container);
        } else if (strncmp(line, "dialog ", 7) == 0 || strncmp(line, "quest ", 6) == 0) {
            bool quest = line[0] == 'q';
            char *title = line + (quest ? 6 : 7);
            char *text = strchr(title, '|');
            CHECK(text != NULL);
            *text++ = '\0';
            if (quest) {
                session_reduce_quest(session, title, text);
            } else {
                session_reduce_dialog(session, title, text);
            }
        } else if (strncmp(line, "message ", 8) == 0) {
            char *type = line + 8;
            char *color = strchr(type, '|');
            CHECK(color != NULL);
            *color++ = '\0';
            char *text = strchr(color, '|');
            CHECK(text != NULL);
            *text++ = '\0';
            session_reduce_message(session, (uint8_t)strtoul(type, NULL, 10), color, text);
        } else if (strncmp(line, "party ", 6) == 0) {
            session_reduce_party(session, line + 6);
        } else {
            CHECK(false);
        }
    }
    CHECK(!ferror(fp));
    CHECK(fclose(fp) == 0);

    session_snapshot_t snapshot = {0};
    CHECK(session_snapshot_copy(session, &snapshot));
    CHECK(snapshot.lifecycle == SESSION_LIFECYCLE_PLAYING);
    CHECK(snapshot.player.id == 42);
    CHECK(snapshot.player.stats.hp == 18);
    CHECK(snapshot.player.stats.experience == 1234);
    CHECK(strcmp(snapshot.map.name, "Recorded_Hall") == 0);
    CHECK(snapshot.map_cells_count == 1);
    CHECK(snapshot.map_entities_count == 1);
    CHECK(snapshot.map_entities[0].id == 99);
    CHECK(snapshot.items_count == 1);
    CHECK(snapshot.items[0].id == 100);
    CHECK(strcmp(snapshot.dialog.title, "Innkeeper") == 0);
    CHECK(strcmp(snapshot.quest.title, "First Steps") == 0);
    CHECK(snapshot.messages_count == 1);
    CHECK(strcmp(snapshot.party, "Recorded_Party") == 0);
    session_snapshot_free(&snapshot);
    session_destroy(session);
    return true;
}

static bool test_snapshot_event_reconstruction(void) {
    session_limits_t limits = session_limits_default();
    limits.events = 64;
    limits.map_cells = 16;
    limits.map_entities = 8;
    limits.items = 8;
    limits.messages = 3;
    sink_record_t sink = {.accept = true};
    session_t *session = playing_session(&limits, &sink);
    CHECK(session != NULL);

    session_map_t map = {.width = 8, .height = 8, .player_x = 4, .player_y = 4};
    session_reduce_map_reset(session, &map);
    session_map_cell_t cell = {.x = 5, .y = 4, .layer = 1, .face = 10};
    CHECK(session_reduce_map_cell(session, &cell));
    session_map_entity_t entity = {.id = 70, .x = 5, .y = 4, .face = 20};
    CHECK(session_reduce_map_entity(session, &entity));
    session_reduce_inventory_begin(session, 42, true, false);
    session_item_t item = {.id = 80, .container_id = 42, .quantity = 2, .weight = 1.25};
    CHECK(session_reduce_item(session, &item));
    session_reduce_inventory_end(session, 42);
    session_player_t weighted_player;
    CHECK(session_player_view(session, &weighted_player));
    CHECK(weighted_player.weight == 2.5f);
    session_reduce_dialog(session, "Old", "Old dialog");
    session_reduce_quest(session, "Old quest", "Old objective");
    session_reduce_message(session, 1, "ffffff", "old message");

    session_snapshot_t replay = {0};
    CHECK(session_snapshot_copy(session, &replay));
    uint64_t baseline_revision = replay.revision;

    session_player_t player;
    CHECK(session_player_view(session, &player));
    player.stats.hp = 7;
    session_reduce_player(session, &player);
    map.player_sub_layer = 2;
    session_reduce_map(session, &map);
    map.player_x++;
    session_reduce_map_scroll(session, 1, 0, 0, &map);
    CHECK(session_reduce_map_layer_clear(session, 4, 4, 0, 1, 0));
    cell.x = 7;
    cell.face = 11;
    CHECK(session_reduce_map_cell(session, &cell));
    CHECK(session_reduce_map_cell_soft_clear(session, 7, 4, 0) == 2);
    entity.x = 7;
    entity.id = 71;
    CHECK(session_reduce_map_entity(session, &entity));
    entity.x = 6;
    entity.id = 5;
    CHECK(session_reduce_map_entity(session, &entity));
    session_target_t target = {.id = 71, .code = 2, .hp = 75, .combat = true};
    CHECK(snprintf(target.name, sizeof(target.name), "Reconstruction target") > 0);
    session_reduce_target(session, &target);
    session_reduce_inventory_begin(session, 42, true, false);
    item.id = 81;
    item.quantity = 4;
    CHECK(session_reduce_item(session, &item));
    item.id = 10;
    item.quantity = 2;
    CHECK(session_reduce_item(session, &item));
    session_reduce_inventory_end(session, 42);
    session_reduce_inventory_begin(session, 81, false, true);
    session_reduce_inventory_end(session, 81);
    item.id = 12;
    item.container_id = 10;
    item.quantity = 1;
    CHECK(session_reduce_item(session, &item));
    CHECK(session_reduce_item_remove(session, 10));
    session_reduce_dialog(session, "New", "New dialog");
    session_reduce_quest(session, "New quest", "New objective");
    session_reduce_message(session, 2, "00ff00", "new message");
    session_reduce_party(session, "Reconstructed");
    session_reduce_party_members_clear(session);
    session_party_member_t member = {.hp = 50, .sp = 60};
    snprintf(member.name, sizeof(member.name), "Bob");
    CHECK(session_reduce_party_member(session, &member));
    member.hp = 80;
    snprintf(member.name, sizeof(member.name), "Alice");
    CHECK(session_reduce_party_member(session, &member));
    CHECK(session_reduce_party_member_remove(session, "Bob"));
    session_reduce_server(session, "reconstructed.example");
    session_action_t control = {
        .type = SESSION_ACTION_CONTROL,
        .data.control = {.run = true, .fire = true},
    };
    CHECK(session_action_dispatch(session, &control) == SESSION_ACTION_ACCEPTED);

    session_event_t events[64];
    size_t event_count = 0;
    CHECK(session_events_read(session, baseline_revision, events, 64, &event_count) ==
          SESSION_EVENTS_OK);
    CHECK(event_count > 0);
    for (size_t i = 0; i < event_count; i++) {
        CHECK(session_snapshot_apply_event(&replay, &events[i]));
    }

    session_snapshot_t final = {0};
    CHECK(session_snapshot_copy(session, &final));
    CHECK(replay.revision == final.revision);
    CHECK(replay.session_generation == final.session_generation);
    CHECK(replay.map_generation == final.map_generation);
    CHECK(replay.inventory_generation == final.inventory_generation);
    CHECK(memcmp(&replay.open_container, &final.open_container, sizeof(replay.open_container)) ==
          0);
    CHECK(replay.lifecycle == final.lifecycle);
    CHECK(replay.capabilities == final.capabilities);
    CHECK(memcmp(&replay.player, &final.player, sizeof(replay.player)) == 0);
    CHECK(memcmp(&replay.target, &final.target, sizeof(replay.target)) == 0);
    CHECK(memcmp(&replay.map, &final.map, sizeof(replay.map)) == 0);
    CHECK(replay.map_cells_count == final.map_cells_count);
    CHECK(memcmp(replay.map_cells,
                 final.map_cells,
                 final.map_cells_count * sizeof(*final.map_cells)) == 0);
    CHECK(replay.map_entities_count == final.map_entities_count);
    CHECK(memcmp(replay.map_entities,
                 final.map_entities,
                 final.map_entities_count * sizeof(*final.map_entities)) == 0);
    CHECK(replay.items_count == final.items_count);
    CHECK(memcmp(replay.items, final.items, final.items_count * sizeof(*final.items)) == 0);
    CHECK(memcmp(&replay.dialog, &final.dialog, sizeof(replay.dialog)) == 0);
    CHECK(memcmp(&replay.quest, &final.quest, sizeof(replay.quest)) == 0);
    CHECK(replay.messages_count == final.messages_count);
    CHECK(memcmp(replay.messages, final.messages, final.messages_count * sizeof(*final.messages)) ==
          0);
    CHECK(strcmp(replay.party, final.party) == 0);
    CHECK(replay.party_members_count == final.party_members_count);
    CHECK(memcmp(replay.party_members,
                 final.party_members,
                 final.party_members_count * sizeof(*final.party_members)) == 0);
    CHECK(strcmp(replay.server, final.server) == 0);
    CHECK(memcmp(&replay.intent, &final.intent, sizeof(replay.intent)) == 0);

    session_event_t malformed = {
        .revision = replay.revision + 1,
        .type = SESSION_EVENT_INTENT,
        .data.intent = {.pending_actions = replay.limits.actions + 1},
    };
    CHECK(!session_snapshot_apply_event(&replay, &malformed));
    CHECK(replay.revision == final.revision);
    malformed.type = (session_event_type_t)999;
    CHECK(!session_snapshot_apply_event(&replay, &malformed));

    session_event_t skipped = events[event_count - 1];
    skipped.revision += 2;
    CHECK(!session_snapshot_apply_event(&replay, &skipped));
    session_snapshot_free(&replay);
    session_snapshot_free(&final);
    session_destroy(session);
    return true;
}

int main(void) {
    const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"limits", test_limits},
        {"lifecycle and snapshot", test_lifecycle_and_snapshot},
        {"player target and events", test_player_target_and_events},
        {"map handles and scroll", test_map_handles_and_scroll},
        {"inventory replay and handles", test_inventory_replay_and_handles},
        {"documents messages and bounding", test_documents_messages_and_bounding},
        {"reducer boundaries and views", test_reducer_boundaries_and_views},
        {"actions", test_actions},
        {"all semantic actions and validation", test_all_semantic_actions_and_validation},
        {"action queue and disconnect", test_action_queue_and_disconnect},
        {"revision gap", test_revision_gap},
        {"dialog replacement rejects queued reply", test_dialog_replacement_rejects_queued_reply},
        {"recorded stream", test_recorded_stream},
        {"snapshot event reconstruction", test_snapshot_event_reconstruction},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (!tests[i].run()) {
            fprintf(stderr, "FAILED: %s\n", tests[i].name);
            return EXIT_FAILURE;
        }
        printf("ok: %s\n", tests[i].name);
    }

    return EXIT_SUCCESS;
}
