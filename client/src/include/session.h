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
 * Renderer-independent client session model and semantic action API.
 *
 * This header deliberately depends only on the C standard library. It is
 * suitable for the SDL client, tests, and future headless/API consumers.
 */

#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SESSION_NAME_SIZE 128
#define SESSION_TEXT_SIZE 4096
#define SESSION_MAP_NAME_SIZE 256
#define SESSION_CHARACTER_SIZE 40
#define SESSION_DESCRIPTION_SIZE 512
#define SESSION_PROTECTIONS_MAX 32
#define SESSION_EQUIPMENT_MAX 32
#define SESSION_PARTY_MEMBERS_MAX 128
#define SESSION_MAP_LAYER_METADATA 0

typedef struct session session_t;

typedef enum session_lifecycle {
    SESSION_LIFECYCLE_DISCONNECTED,
    SESSION_LIFECYCLE_CONNECTED,
    SESSION_LIFECYCLE_PLAYING
} session_lifecycle_t;

typedef enum session_capability {
    SESSION_CAP_MOVE = UINT64_C(1) << 0,
    SESSION_CAP_TARGET = UINT64_C(1) << 1,
    SESSION_CAP_COMBAT = UINT64_C(1) << 2,
    SESSION_CAP_CAST = UINT64_C(1) << 3,
    SESSION_CAP_INVENTORY = UINT64_C(1) << 4,
    SESSION_CAP_TALK = UINT64_C(1) << 5,
    SESSION_CAP_SELECT_CHARACTER = UINT64_C(1) << 6,
    SESSION_CAP_PLAYER_COMMAND = UINT64_C(1) << 7
} session_capability_t;

#define SESSION_CAP_GAMEPLAY                                                         \
    (SESSION_CAP_MOVE | SESSION_CAP_TARGET | SESSION_CAP_COMBAT | SESSION_CAP_CAST | \
     SESSION_CAP_INVENTORY | SESSION_CAP_TALK | SESSION_CAP_PLAYER_COMMAND)

typedef enum session_handle_kind {
    SESSION_HANDLE_NONE,
    SESSION_HANDLE_MAP_ENTITY,
    SESSION_HANDLE_ITEM
} session_handle_kind_t;

/** A handle is valid only inside the session and collection generations. */
typedef struct session_handle {
    uint64_t session_generation;
    uint64_t collection_generation;
    uint64_t object_generation;
    uint32_t id;
    session_handle_kind_t kind;
} session_handle_t;

typedef struct session_player_stats {
    int8_t strength;
    int8_t dexterity;
    int8_t constitution;
    int8_t intelligence;
    int8_t power;
    int16_t wc;
    int16_t ac;
    uint32_t level;
    int32_t hp;
    int32_t max_hp;
    int32_t sp;
    int32_t max_sp;
    int64_t experience;
    int16_t food;
    int16_t damage;
    float speed;
    float weapon_speed;
    uint16_t flags;
    int16_t ranged_damage;
    int16_t ranged_wc;
    float ranged_weapon_speed;
    float hp_regeneration;
    float sp_regeneration;
    int8_t protections[SESSION_PROTECTIONS_MAX];
} session_player_stats_t;

typedef struct session_player {
    uint32_t id;
    char name[SESSION_CHARACTER_SIZE];
    session_player_stats_t stats;
    float weight;
    float weight_limit;
    float action_timer;
    uint8_t gender;
    bool dm;
    uint32_t equipment[SESSION_EQUIPMENT_MAX];
    uint32_t path_attuned;
    uint32_t path_repelled;
    uint32_t path_denied;
} session_player_t;

typedef struct session_target {
    uint32_t id;
    char name[SESSION_NAME_SIZE];
    char color[16];
    uint8_t code;
    uint8_t level;
    uint8_t hp;
    bool friend;
    bool combat;
    bool combat_force;
} session_target_t;

typedef struct session_map {
    char name[SESSION_MAP_NAME_SIZE];
    char path[SESSION_MAP_NAME_SIZE];
    int width;
    int height;
    int player_x;
    int player_y;
    int player_depth;
    uint8_t player_sub_layer;
} session_map_t;

typedef struct session_map_entity {
    uint32_t id;
    uint64_t generation;
    int16_t x;
    int16_t y;
    int8_t depth;
    uint8_t sub_layer;
    uint16_t face;
    uint32_t flags;
    uint8_t hp;
    bool friend;
    char name[SESSION_NAME_SIZE];
} session_map_entity_t;

typedef struct session_map_cell {
    int16_t x;
    int16_t y;
    int8_t depth;
    uint8_t layer;
    uint8_t sub_layer;
    uint16_t face;
    uint32_t flags;
    int16_t height;
    uint8_t light_level;
    bool light_known;
    bool fogged;
    bool fog_known;
    char name[SESSION_NAME_SIZE];
} session_map_cell_t;

typedef enum session_item_category {
    SESSION_ITEM_INVENTORY,
    SESSION_ITEM_SKILL,
    SESSION_ITEM_SPELL,
    SESSION_ITEM_EFFECT
} session_item_category_t;

typedef struct session_item {
    uint32_t id;
    uint64_t generation;
    uint32_t container_id;
    uint32_t quantity;
    double weight;
    uint32_t flags;
    uint16_t face;
    uint8_t type;
    uint8_t subtype;
    uint8_t quality;
    uint8_t condition;
    uint8_t required_level;
    uint8_t direction;
    uint32_t required_skill_id;
    session_item_category_t category;
    uint8_t ability_level;
    int64_t ability_experience;
    uint16_t ability_cost;
    uint32_t ability_path;
    uint32_t ability_flags;
    int32_t effect_seconds;
    char name[SESSION_NAME_SIZE];
    char description[SESSION_DESCRIPTION_SIZE];
} session_item_t;

typedef struct session_document {
    uint64_t generation;
    char title[SESSION_NAME_SIZE];
    char text[SESSION_TEXT_SIZE];
} session_document_t;

typedef struct session_message {
    uint64_t revision;
    uint8_t type;
    char color[16];
    char text[SESSION_TEXT_SIZE];
} session_message_t;

typedef struct session_party_member {
    char name[SESSION_NAME_SIZE];
    uint8_t hp;
    uint8_t sp;
} session_party_member_t;

typedef struct session_intent {
    int8_t movement_direction;
    bool movement_held;
    bool run;
    bool fire;
    bool combat;
    bool combat_force;
    session_handle_t target;
    size_t pending_actions;
} session_intent_t;

typedef struct session_limits {
    size_t events;
    size_t actions;
    size_t map_cells;
    size_t map_entities;
    size_t items;
    size_t messages;
} session_limits_t;

typedef enum session_event_type {
    SESSION_EVENT_LIFECYCLE,
    SESSION_EVENT_CAPABILITIES,
    SESSION_EVENT_PLAYER,
    SESSION_EVENT_TARGET,
    SESSION_EVENT_MAP,
    SESSION_EVENT_MAP_RESET,
    SESSION_EVENT_MAP_SCROLL,
    SESSION_EVENT_MAP_CELL_UPSERT,
    SESSION_EVENT_MAP_CELL_CLEAR,
    SESSION_EVENT_MAP_CELL_SOFT_CLEAR,
    SESSION_EVENT_MAP_ENTITY_UPSERT,
    SESSION_EVENT_MAP_ENTITY_REMOVE,
    SESSION_EVENT_INVENTORY_REPLAY_BEGIN,
    SESSION_EVENT_ITEM_UPSERT,
    SESSION_EVENT_ITEM_REMOVE,
    SESSION_EVENT_INVENTORY_REPLAY_END,
    SESSION_EVENT_DIALOG,
    SESSION_EVENT_QUEST,
    SESSION_EVENT_MESSAGE,
    SESSION_EVENT_PARTY,
    SESSION_EVENT_PARTY_MEMBERS_CLEAR,
    SESSION_EVENT_PARTY_MEMBER_UPSERT,
    SESSION_EVENT_PARTY_MEMBER_REMOVE,
    SESSION_EVENT_SERVER,
    SESSION_EVENT_INTENT
} session_event_type_t;

typedef struct session_event {
    uint64_t revision;
    session_event_type_t type;
    union {
        struct {
            session_lifecycle_t lifecycle;
            uint64_t session_generation;
        } lifecycle;
        uint64_t capabilities;
        session_player_t player;
        session_target_t target;
        session_map_t map;
        struct {
            int x_offset;
            int y_offset;
            int depth_offset;
            session_map_t map;
        } scroll;
        session_map_cell_t map_cell;
        struct {
            int16_t x;
            int16_t y;
            int8_t depth;
            int16_t layer;
            int16_t sub_layer;
        } map_cell_clear;
        session_map_entity_t map_entity;
        struct {
            uint32_t id;
            uint64_t generation;
        } removed;
        struct {
            uint32_t container_id;
            uint64_t generation;
            bool open;
        } replay;
        session_item_t item;
        session_document_t document;
        session_message_t message;
        session_party_member_t party_member;
        char text[SESSION_TEXT_SIZE];
        session_intent_t intent;
    } data;
} session_event_t;

typedef enum session_events_result {
    SESSION_EVENTS_OK,
    SESSION_EVENTS_GAP,
    SESSION_EVENTS_INVALID
} session_events_result_t;

typedef struct session_snapshot {
    session_limits_t limits;
    uint64_t revision;
    uint64_t session_generation;
    uint64_t map_generation;
    uint64_t inventory_generation;
    session_handle_t open_container;
    session_lifecycle_t lifecycle;
    uint64_t capabilities;
    session_player_t player;
    session_target_t target;
    session_map_t map;
    session_map_cell_t *map_cells;
    size_t map_cells_count;
    size_t map_cells_capacity;
    session_map_entity_t *map_entities;
    size_t map_entities_count;
    size_t map_entities_capacity;
    session_item_t *items;
    size_t items_count;
    size_t items_capacity;
    session_document_t dialog;
    session_document_t quest;
    session_message_t *messages;
    size_t messages_count;
    size_t messages_capacity;
    char party[SESSION_NAME_SIZE];
    session_party_member_t party_members[SESSION_PARTY_MEMBERS_MAX];
    size_t party_members_count;
    char server[SESSION_NAME_SIZE];
    session_intent_t intent;
} session_snapshot_t;

typedef enum session_action_type {
    SESSION_ACTION_MOVE,
    SESSION_ACTION_MOVE_PATH,
    SESSION_ACTION_STOP,
    SESSION_ACTION_TARGET,
    SESSION_ACTION_ATTACK,
    SESSION_ACTION_CAST,
    SESSION_ACTION_APPLY,
    SESSION_ACTION_GET,
    SESSION_ACTION_DROP,
    SESSION_ACTION_TALK,
    SESSION_ACTION_REPLY,
    SESSION_ACTION_SELECT_CHARACTER,
    SESSION_ACTION_CONTROL,
    SESSION_ACTION_PLAYER_COMMAND
} session_action_type_t;

typedef enum session_virtual_item_action {
    SESSION_VIRTUAL_ITEM_NONE,
    SESSION_VIRTUAL_ITEM_NEXT,
    SESSION_VIRTUAL_ITEM_PREVIOUS
} session_virtual_item_action_t;

typedef struct session_action {
    session_action_type_t type;
    union {
        struct {
            uint8_t direction;
            bool run;
            bool fire;
        } move;
        struct {
            int16_t x;
            int16_t y;
        } point;
        struct {
            session_handle_t handle;
            int16_t x;
            int16_t y;
            bool clear;
        } target;
        session_handle_t handle;
        struct {
            bool enabled;
            bool force;
        } attack;
        struct {
            bool run;
            bool fire;
        } control;
        struct {
            session_handle_t handle;
            uint8_t direction;
        } cast;
        struct {
            session_handle_t handle;
            session_handle_t container;
            uint32_t container_id;
            uint32_t quantity;
            session_virtual_item_action_t virtual_action;
            bool virtual_item;
        } item;
        struct {
            uint32_t reply;
            uint64_t dialog_generation;
            char text[SESSION_TEXT_SIZE];
        } reply;
        char text[SESSION_TEXT_SIZE];
    } data;
} session_action_t;

typedef enum session_action_result {
    SESSION_ACTION_ACCEPTED,
    SESSION_ACTION_REJECTED_ARGUMENT,
    SESSION_ACTION_REJECTED_CAPABILITY,
    SESSION_ACTION_REJECTED_LIFECYCLE,
    SESSION_ACTION_REJECTED_STALE_HANDLE,
    SESSION_ACTION_REJECTED_QUEUE_FULL,
    SESSION_ACTION_REJECTED_SINK
} session_action_result_t;

typedef bool (*session_command_sink_t)(void *context, const session_action_t *action);

session_limits_t session_limits_default(void);
session_t *
session_create(const session_limits_t *limits, session_command_sink_t sink, void *sink_context);
void session_destroy(session_t *session);

uint64_t session_revision(const session_t *session);
uint64_t session_oldest_event_revision(const session_t *session);
void session_set_command_sink(session_t *session, session_command_sink_t sink, void *sink_context);

/**
 * Create an owned snapshot in a zero-initialized destination.
 *
 * The caller must pass a destination initialized to all zeroes and release a
 * successful copy with session_snapshot_free() before reusing it.
 */
bool session_snapshot_copy(const session_t *session, session_snapshot_t *snapshot);
/** Release all owned storage and reset the snapshot to all zeroes. */
void session_snapshot_free(session_snapshot_t *snapshot);
/** Apply exactly the next ordered event to an initialized owned snapshot. */
bool session_snapshot_apply_event(session_snapshot_t *snapshot, const session_event_t *event);
bool session_player_view(const session_t *session, session_player_t *player);
bool session_target_view(const session_t *session, session_target_t *target);
bool session_map_view(const session_t *session, session_map_t *map);
bool session_dialog_view(const session_t *session, session_document_t *dialog);
bool session_quest_view(const session_t *session, session_document_t *quest);
bool session_party_name_view(const session_t *session, char *party, size_t size);
bool session_map_entity_view(const session_t *session,
                             session_handle_t handle,
                             session_map_entity_t *entity);
bool session_item_view(const session_t *session, session_handle_t handle, session_item_t *item);
bool session_intent_view(const session_t *session, session_intent_t *intent);
session_events_result_t session_events_read(const session_t *session,
                                            uint64_t after_revision,
                                            session_event_t *events,
                                            size_t capacity,
                                            size_t *count);

void session_reduce_connect(session_t *session, uint64_t capabilities, const char *server);
void session_reduce_play(session_t *session, const session_player_t *player);
void session_reduce_disconnect(session_t *session);
void session_reduce_character_reset(session_t *session);
void session_reduce_capabilities(session_t *session, uint64_t capabilities);
void session_reduce_player(session_t *session, const session_player_t *player);
void session_reduce_target(session_t *session, const session_target_t *target);
void session_reduce_map(session_t *session, const session_map_t *map);
void session_reduce_map_reset(session_t *session, const session_map_t *map);
void session_reduce_map_scroll(session_t *session,
                               int x_offset,
                               int y_offset,
                               int depth_offset,
                               const session_map_t *map);
bool session_reduce_map_entity(session_t *session, const session_map_entity_t *entity);
bool session_reduce_map_entity_remove(session_t *session, uint32_t id);
bool session_reduce_map_cell(session_t *session, const session_map_cell_t *cell);
bool session_reduce_map_layer_clear(session_t *session,
                                    int x,
                                    int y,
                                    int depth,
                                    uint8_t layer,
                                    uint8_t sub_layer);
size_t
session_reduce_map_entities_clear(session_t *session, int x, int y, int depth, int sub_layer);
size_t session_reduce_map_cell_clear(session_t *session, int x, int y, int depth, int sub_layer);
size_t session_reduce_map_cell_soft_clear(session_t *session, int x, int y, int depth);
void session_reduce_inventory_begin(session_t *session,
                                    uint32_t container_id,
                                    bool clear,
                                    bool open);
bool session_reduce_item(session_t *session, const session_item_t *item);
bool session_reduce_item_remove(session_t *session, uint32_t id);
void session_reduce_inventory_end(session_t *session, uint32_t container_id);
void session_reduce_dialog(session_t *session, const char *title, const char *text);
void session_reduce_quest(session_t *session, const char *title, const char *text);
void session_reduce_message(session_t *session, uint8_t type, const char *color, const char *text);
void session_reduce_party(session_t *session, const char *party);
void session_reduce_party_members_clear(session_t *session);
bool session_reduce_party_member(session_t *session, const session_party_member_t *member);
bool session_reduce_party_member_remove(session_t *session, const char *name);
void session_reduce_server(session_t *session, const char *server);

session_handle_t session_map_handle(const session_t *session, uint32_t id);
session_handle_t session_map_handle_at(const session_t *session, int x, int y);
session_handle_t session_item_handle(const session_t *session, uint32_t id);
bool session_handle_valid(const session_t *session, session_handle_t handle);

session_action_result_t session_action_dispatch(session_t *session, const session_action_t *action);
session_action_result_t session_action_enqueue(session_t *session, const session_action_t *action);
size_t session_actions_drain(session_t *session, session_action_result_t *results, size_t capacity);
void session_actions_clear(session_t *session);

const char *session_action_result_string(session_action_result_t result);

#endif
