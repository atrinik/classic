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

#include <stdlib.h>
#include <string.h>

#define SESSION_EVENTS_MAX 4096
#define SESSION_ACTIONS_MAX 1024
#define SESSION_MAP_CELLS_MAX 32768
#define SESSION_MAP_ENTITIES_MAX 8192
#define SESSION_ITEMS_MAX 8192
#define SESSION_MESSAGES_MAX 2048

struct session {
    uint64_t revision;
    uint64_t session_generation;
    uint64_t map_generation;
    uint64_t inventory_generation;
    uint32_t open_container_id;
    uint64_t next_object_generation;
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
    size_t inventory_replay_depth;
    session_document_t dialog;
    session_document_t quest;
    session_message_t *messages;
    size_t messages_head;
    size_t messages_count;
    size_t messages_capacity;
    char party[SESSION_NAME_SIZE];
    session_party_member_t party_members[SESSION_PARTY_MEMBERS_MAX];
    size_t party_members_count;
    char server[SESSION_NAME_SIZE];
    session_intent_t intent;
    session_event_t *events;
    size_t events_head;
    size_t events_count;
    size_t events_capacity;
    session_action_t *actions;
    size_t actions_head;
    size_t actions_count;
    size_t actions_capacity;
    session_command_sink_t sink;
    void *sink_context;
};

static size_t map_entity_index(const session_t *session, uint32_t id, bool *found);
static int map_cell_compare(const session_map_cell_t *cell,
                            int x,
                            int y,
                            int depth,
                            uint8_t layer,
                            uint8_t sub_layer);
static size_t map_cell_index(const session_t *session,
                             int x,
                             int y,
                             int depth,
                             uint8_t layer,
                             uint8_t sub_layer,
                             bool *found);
static size_t item_index(const session_t *session, uint32_t id, bool *found);

static void copy_string(char *destination, size_t size, const char *source) {
    if (size == 0) {
        return;
    }

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    size_t length = 0;
    while (length + 1 < size && source[length] != '\0') {
        length++;
    }

    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool string_valid(const char *text, size_t size, bool allow_empty) {
    if (text == NULL || memchr(text, '\0', size) == NULL) {
        return false;
    }

    return allow_empty || text[0] != '\0';
}

static bool map_dimensions_valid(const session_map_t *map) {
    return map != NULL && map->width > 0 && map->width <= INT16_MAX && map->height > 0 &&
           map->height <= INT16_MAX && map->player_x >= 0 && map->player_x < map->width &&
           map->player_y >= 0 && map->player_y < map->height && map->player_depth >= INT8_MIN &&
           map->player_depth <= INT8_MAX;
}

static bool map_position_valid(const session_map_t *map, int x, int y) {
    return map_dimensions_valid(map) && x >= 0 && x < map->width && y >= 0 && y < map->height;
}

static bool map_equal(const session_map_t *left, const session_map_t *right) {
    return strcmp(left->name, right->name) == 0 && strcmp(left->path, right->path) == 0 &&
           left->width == right->width && left->height == right->height &&
           left->player_x == right->player_x && left->player_y == right->player_y &&
           left->player_depth == right->player_depth &&
           left->player_sub_layer == right->player_sub_layer;
}

static bool generation_current_or_next(uint64_t current, uint64_t candidate) {
    return candidate == current || (current != UINT64_MAX && candidate == current + 1);
}

static bool player_stats_equal(const session_player_stats_t *left,
                               const session_player_stats_t *right) {
    return left->strength == right->strength && left->dexterity == right->dexterity &&
           left->constitution == right->constitution && left->intelligence == right->intelligence &&
           left->power == right->power && left->wc == right->wc && left->ac == right->ac &&
           left->level == right->level && left->hp == right->hp && left->max_hp == right->max_hp &&
           left->sp == right->sp && left->max_sp == right->max_sp &&
           left->experience == right->experience && left->food == right->food &&
           left->damage == right->damage && left->speed == right->speed &&
           left->weapon_speed == right->weapon_speed && left->flags == right->flags &&
           left->ranged_damage == right->ranged_damage && left->ranged_wc == right->ranged_wc &&
           left->ranged_weapon_speed == right->ranged_weapon_speed &&
           left->hp_regeneration == right->hp_regeneration &&
           left->sp_regeneration == right->sp_regeneration &&
           memcmp(left->protections, right->protections, sizeof(left->protections)) == 0;
}

static bool player_equal(const session_player_t *left, const session_player_t *right) {
    return left->id == right->id && strcmp(left->name, right->name) == 0 &&
           player_stats_equal(&left->stats, &right->stats) && left->weight == right->weight &&
           left->weight_limit == right->weight_limit && left->action_timer == right->action_timer &&
           left->gender == right->gender && left->dm == right->dm &&
           memcmp(left->equipment, right->equipment, sizeof(left->equipment)) == 0 &&
           left->path_attuned == right->path_attuned &&
           left->path_repelled == right->path_repelled && left->path_denied == right->path_denied;
}

static void event_emit(session_t *session, session_event_t *event) {
    session->revision++;
    event->revision = session->revision;

    size_t slot;
    if (session->events_count < session->events_capacity) {
        slot = (session->events_head + session->events_count) % session->events_capacity;
        session->events_count++;
    } else {
        slot = session->events_head;
        session->events_head = (session->events_head + 1) % session->events_capacity;
    }

    session->events[slot] = *event;
}

static void intent_emit(session_t *session) {
    session_event_t event = {.type = SESSION_EVENT_INTENT};
    event.data.intent = session->intent;
    event_emit(session, &event);
}

static void player_weight_recalculate(session_t *session) {
    if (session->player.id == 0) {
        return;
    }

    float weight = 0.0f;
    for (size_t i = 0; i < session->items_count; i++) {
        if (session->items[i].container_id == session->player.id) {
            weight += (float)(session->items[i].weight * session->items[i].quantity);
        }
    }
    if (session->player.weight == weight) {
        return;
    }

    session->player.weight = weight;
    session_event_t event = {.type = SESSION_EVENT_PLAYER};
    event.data.player = session->player;
    event_emit(session, &event);
}

static void state_clear(session_t *session) {
    memset(&session->player, 0, sizeof(session->player));
    session->player.stats.strength = -1;
    session->player.stats.dexterity = -1;
    session->player.stats.constitution = -1;
    session->player.stats.intelligence = -1;
    session->player.stats.power = -1;
    session->player.stats.max_hp = 1;
    session->player.stats.max_sp = 1;
    session->player.stats.speed = 1.0f;
    memset(&session->target, 0, sizeof(session->target));
    memset(&session->map, 0, sizeof(session->map));
    session->map_entities_count = 0;
    session->map_cells_count = 0;
    session->items_count = 0;
    session->inventory_replay_depth = 0;
    session->open_container_id = 0;
    memset(&session->dialog, 0, sizeof(session->dialog));
    memset(&session->quest, 0, sizeof(session->quest));
    session->messages_head = 0;
    session->messages_count = 0;
    session->party[0] = '\0';
    session->party_members_count = 0;
    memset(&session->intent, 0, sizeof(session->intent));
    session->actions_head = 0;
    session->actions_count = 0;
}

static bool limits_valid(const session_limits_t *limits) {
    return limits != NULL && limits->events > 0 && limits->events <= SESSION_EVENTS_MAX &&
           limits->actions > 0 && limits->actions <= SESSION_ACTIONS_MAX && limits->map_cells > 0 &&
           limits->map_cells <= SESSION_MAP_CELLS_MAX && limits->map_entities > 0 &&
           limits->map_entities <= SESSION_MAP_ENTITIES_MAX && limits->items > 0 &&
           limits->items <= SESSION_ITEMS_MAX && limits->messages > 0 &&
           limits->messages <= SESSION_MESSAGES_MAX;
}

session_limits_t session_limits_default(void) {
    return (session_limits_t){
        .events = 256,
        .actions = 64,
        .map_cells = SESSION_MAP_CELLS_MAX,
        .map_entities = 1024,
        .items = 1024,
        .messages = 128,
    };
}

session_t *
session_create(const session_limits_t *limits, session_command_sink_t sink, void *sink_context) {
    session_limits_t defaults;
    if (limits == NULL) {
        defaults = session_limits_default();
        limits = &defaults;
    }

    if (!limits_valid(limits)) {
        return NULL;
    }

    session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return NULL;
    }

    session->events = calloc(limits->events, sizeof(*session->events));
    session->actions = calloc(limits->actions, sizeof(*session->actions));
    session->map_cells = calloc(limits->map_cells, sizeof(*session->map_cells));
    session->map_entities = calloc(limits->map_entities, sizeof(*session->map_entities));
    session->items = calloc(limits->items, sizeof(*session->items));
    session->messages = calloc(limits->messages, sizeof(*session->messages));
    if (session->events == NULL || session->actions == NULL || session->map_cells == NULL ||
        session->map_entities == NULL || session->items == NULL || session->messages == NULL) {
        session_destroy(session);
        return NULL;
    }

    session->events_capacity = limits->events;
    session->actions_capacity = limits->actions;
    session->map_cells_capacity = limits->map_cells;
    session->map_entities_capacity = limits->map_entities;
    session->items_capacity = limits->items;
    session->messages_capacity = limits->messages;
    session->session_generation = 1;
    session->map_generation = 1;
    session->inventory_generation = 1;
    session->next_object_generation = 1;
    session->sink = sink;
    session->sink_context = sink_context;
    state_clear(session);
    return session;
}

void session_destroy(session_t *session) {
    if (session == NULL) {
        return;
    }

    free(session->events);
    free(session->actions);
    free(session->map_cells);
    free(session->map_entities);
    free(session->items);
    free(session->messages);
    free(session);
}

uint64_t session_revision(const session_t *session) {
    return session != NULL ? session->revision : 0;
}

uint64_t session_oldest_event_revision(const session_t *session) {
    if (session == NULL || session->events_count == 0) {
        return 0;
    }

    return session->events[session->events_head].revision;
}

void session_set_command_sink(session_t *session, session_command_sink_t sink, void *sink_context) {
    if (session != NULL) {
        session->sink = sink;
        session->sink_context = sink_context;
    }
}

bool session_snapshot_copy(const session_t *session, session_snapshot_t *snapshot) {
    if (session == NULL || snapshot == NULL) {
        return false;
    }

    session_snapshot_t copy = {
        .limits =
            {
                .events = session->events_capacity,
                .actions = session->actions_capacity,
                .map_cells = session->map_cells_capacity,
                .map_entities = session->map_entities_capacity,
                .items = session->items_capacity,
                .messages = session->messages_capacity,
            },
        .revision = session->revision,
        .session_generation = session->session_generation,
        .map_generation = session->map_generation,
        .inventory_generation = session->inventory_generation,
        .open_container = session_item_handle(session, session->open_container_id),
        .lifecycle = session->lifecycle,
        .capabilities = session->capabilities,
        .player = session->player,
        .target = session->target,
        .map = session->map,
        .map_cells_count = session->map_cells_count,
        .map_cells_capacity = session->map_cells_count,
        .map_entities_count = session->map_entities_count,
        .map_entities_capacity = session->map_entities_count,
        .items_count = session->items_count,
        .items_capacity = session->items_count,
        .dialog = session->dialog,
        .quest = session->quest,
        .messages_count = session->messages_count,
        .messages_capacity = session->messages_count,
        .party_members_count = session->party_members_count,
        .intent = session->intent,
    };
    copy_string(copy.party, sizeof(copy.party), session->party);
    copy_string(copy.server, sizeof(copy.server), session->server);
    memcpy(copy.party_members,
           session->party_members,
           session->party_members_count * sizeof(*copy.party_members));

    if (copy.map_cells_count > 0) {
        copy.map_cells = malloc(copy.map_cells_count * sizeof(*copy.map_cells));
        if (copy.map_cells == NULL) {
            return false;
        }
        memcpy(copy.map_cells, session->map_cells, copy.map_cells_count * sizeof(*copy.map_cells));
    }

    if (copy.map_entities_count > 0) {
        copy.map_entities = malloc(copy.map_entities_count * sizeof(*copy.map_entities));
        if (copy.map_entities == NULL) {
            session_snapshot_free(&copy);
            return false;
        }
        memcpy(copy.map_entities,
               session->map_entities,
               copy.map_entities_count * sizeof(*copy.map_entities));
    }

    if (copy.items_count > 0) {
        copy.items = malloc(copy.items_count * sizeof(*copy.items));
        if (copy.items == NULL) {
            session_snapshot_free(&copy);
            return false;
        }
        memcpy(copy.items, session->items, copy.items_count * sizeof(*copy.items));
    }

    if (copy.messages_count > 0) {
        copy.messages = malloc(copy.messages_count * sizeof(*copy.messages));
        if (copy.messages == NULL) {
            session_snapshot_free(&copy);
            return false;
        }

        for (size_t i = 0; i < copy.messages_count; i++) {
            size_t source = (session->messages_head + i) % session->messages_capacity;
            copy.messages[i] = session->messages[source];
        }
    }

    *snapshot = copy;
    return true;
}

void session_snapshot_free(session_snapshot_t *snapshot) {
    if (snapshot == NULL) {
        return;
    }

    free(snapshot->map_entities);
    free(snapshot->map_cells);
    free(snapshot->items);
    free(snapshot->messages);
    memset(snapshot, 0, sizeof(*snapshot));
}

static void snapshot_generation_reset(session_snapshot_t *snapshot) {
    snapshot->capabilities = 0;
    memset(&snapshot->player, 0, sizeof(snapshot->player));
    snapshot->player.stats.strength = -1;
    snapshot->player.stats.dexterity = -1;
    snapshot->player.stats.constitution = -1;
    snapshot->player.stats.intelligence = -1;
    snapshot->player.stats.power = -1;
    snapshot->player.stats.max_hp = 1;
    snapshot->player.stats.max_sp = 1;
    snapshot->player.stats.speed = 1.0f;
    memset(&snapshot->target, 0, sizeof(snapshot->target));
    memset(&snapshot->map, 0, sizeof(snapshot->map));
    snapshot->map_cells_count = 0;
    snapshot->map_entities_count = 0;
    snapshot->items_count = 0;
    memset(&snapshot->open_container, 0, sizeof(snapshot->open_container));
    memset(&snapshot->dialog, 0, sizeof(snapshot->dialog));
    memset(&snapshot->quest, 0, sizeof(snapshot->quest));
    snapshot->messages_count = 0;
    snapshot->party[0] = '\0';
    snapshot->party_members_count = 0;
    memset(&snapshot->intent, 0, sizeof(snapshot->intent));
    snapshot->map_generation++;
    snapshot->inventory_generation++;
}

static bool snapshot_valid(const session_snapshot_t *snapshot) {
    return snapshot != NULL && limits_valid(&snapshot->limits) &&
           snapshot->map_cells_count <= snapshot->map_cells_capacity &&
           snapshot->map_cells_capacity <= snapshot->limits.map_cells &&
           (snapshot->map_cells_capacity == 0 || snapshot->map_cells != NULL) &&
           snapshot->map_entities_count <= snapshot->map_entities_capacity &&
           snapshot->map_entities_capacity <= snapshot->limits.map_entities &&
           (snapshot->map_entities_capacity == 0 || snapshot->map_entities != NULL) &&
           snapshot->items_count <= snapshot->items_capacity &&
           snapshot->items_capacity <= snapshot->limits.items &&
           (snapshot->items_capacity == 0 || snapshot->items != NULL) &&
           snapshot->messages_count <= snapshot->messages_capacity &&
           snapshot->messages_capacity <= snapshot->limits.messages &&
           (snapshot->messages_capacity == 0 || snapshot->messages != NULL) &&
           snapshot->party_members_count <= SESSION_PARTY_MEMBERS_MAX &&
           snapshot->revision != UINT64_MAX;
}

static bool
snapshot_reserve(void **items, size_t *capacity, size_t required, size_t limit, size_t item_size) {
    if (required <= *capacity) {
        return true;
    }
    if (required > limit) {
        return false;
    }

    size_t next = *capacity > 0 ? *capacity : 1;
    while (next < required) {
        if (next > limit / 2) {
            next = limit;
            break;
        }
        next *= 2;
    }

    void *resized = realloc(*items, next * item_size);
    if (resized == NULL) {
        return false;
    }
    *items = resized;
    *capacity = next;
    return true;
}

static bool snapshot_map_cell_upsert(session_snapshot_t *snapshot, const session_map_cell_t *cell) {
    size_t low = 0;
    size_t high = snapshot->map_cells_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (map_cell_compare(&snapshot->map_cells[middle],
                             cell->x,
                             cell->y,
                             cell->depth,
                             cell->layer,
                             cell->sub_layer) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < snapshot->map_cells_count && map_cell_compare(&snapshot->map_cells[low],
                                                            cell->x,
                                                            cell->y,
                                                            cell->depth,
                                                            cell->layer,
                                                            cell->sub_layer) == 0) {
        snapshot->map_cells[low] = *cell;
        return true;
    }
    if (!snapshot_reserve((void **)&snapshot->map_cells,
                          &snapshot->map_cells_capacity,
                          snapshot->map_cells_count + 1,
                          snapshot->limits.map_cells,
                          sizeof(*snapshot->map_cells))) {
        return false;
    }
    memmove(&snapshot->map_cells[low + 1],
            &snapshot->map_cells[low],
            (snapshot->map_cells_count - low) * sizeof(*snapshot->map_cells));
    snapshot->map_cells[low] = *cell;
    snapshot->map_cells_count++;
    return true;
}

static bool snapshot_map_entity_upsert(session_snapshot_t *snapshot,
                                       const session_map_entity_t *entity) {
    size_t low = 0;
    size_t high = snapshot->map_entities_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (snapshot->map_entities[middle].id < entity->id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < snapshot->map_entities_count && snapshot->map_entities[low].id == entity->id) {
        snapshot->map_entities[low] = *entity;
        return true;
    }
    if (!snapshot_reserve((void **)&snapshot->map_entities,
                          &snapshot->map_entities_capacity,
                          snapshot->map_entities_count + 1,
                          snapshot->limits.map_entities,
                          sizeof(*snapshot->map_entities))) {
        return false;
    }
    memmove(&snapshot->map_entities[low + 1],
            &snapshot->map_entities[low],
            (snapshot->map_entities_count - low) * sizeof(*snapshot->map_entities));
    snapshot->map_entities[low] = *entity;
    snapshot->map_entities_count++;
    return true;
}

static bool snapshot_item_upsert(session_snapshot_t *snapshot, const session_item_t *item) {
    size_t low = 0;
    size_t high = snapshot->items_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (snapshot->items[middle].id < item->id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < snapshot->items_count && snapshot->items[low].id == item->id) {
        snapshot->items[low] = *item;
        return true;
    }
    if (!snapshot_reserve((void **)&snapshot->items,
                          &snapshot->items_capacity,
                          snapshot->items_count + 1,
                          snapshot->limits.items,
                          sizeof(*snapshot->items))) {
        return false;
    }
    memmove(&snapshot->items[low + 1],
            &snapshot->items[low],
            (snapshot->items_count - low) * sizeof(*snapshot->items));
    snapshot->items[low] = *item;
    snapshot->items_count++;
    return true;
}

static void snapshot_remove_entity(session_snapshot_t *snapshot, uint32_t id) {
    for (size_t i = 0; i < snapshot->map_entities_count; i++) {
        if (snapshot->map_entities[i].id != id) {
            continue;
        }
        memmove(&snapshot->map_entities[i],
                &snapshot->map_entities[i + 1],
                (snapshot->map_entities_count - i - 1) * sizeof(*snapshot->map_entities));
        snapshot->map_entities_count--;
        return;
    }
}

static void snapshot_remove_item(session_snapshot_t *snapshot, uint32_t id) {
    for (size_t i = 0; i < snapshot->items_count; i++) {
        if (snapshot->items[i].id != id) {
            continue;
        }
        memmove(&snapshot->items[i],
                &snapshot->items[i + 1],
                (snapshot->items_count - i - 1) * sizeof(*snapshot->items));
        snapshot->items_count--;
        return;
    }
}

static bool snapshot_item_descends_from(const session_snapshot_t *snapshot,
                                        const session_item_t *item,
                                        uint32_t container_id) {
    uint32_t parent = item->container_id;
    for (size_t depth = 0; depth <= snapshot->items_count; depth++) {
        if (parent == container_id) {
            return true;
        }
        if (parent == 0) {
            return false;
        }

        const session_item_t *container = NULL;
        for (size_t i = 0; i < snapshot->items_count; i++) {
            if (snapshot->items[i].id == parent) {
                container = &snapshot->items[i];
                break;
            }
        }
        if (container == NULL) {
            return false;
        }
        parent = container->container_id;
    }
    return false;
}

static bool snapshot_item_parent_valid(const session_snapshot_t *snapshot,
                                       const session_item_t *item) {
    uint32_t parent = item->container_id;
    for (size_t depth = 0; parent != 0 && depth <= snapshot->items_count; depth++) {
        if (parent == item->id) {
            return false;
        }
        const session_item_t *container = NULL;
        for (size_t i = 0; i < snapshot->items_count; i++) {
            if (snapshot->items[i].id == parent) {
                container = &snapshot->items[i];
                break;
            }
        }
        if (container == NULL) {
            return true;
        }
        parent = container->container_id;
    }
    return parent == 0;
}

static bool snapshot_entity_generation_valid(const session_snapshot_t *snapshot,
                                             const session_map_entity_t *entity) {
    for (size_t i = 0; i < snapshot->map_entities_count; i++) {
        if (snapshot->map_entities[i].id == entity->id) {
            return snapshot->map_entities[i].generation == entity->generation;
        }
    }
    return true;
}

static bool snapshot_item_generation_valid(const session_snapshot_t *snapshot,
                                           const session_item_t *item) {
    for (size_t i = 0; i < snapshot->items_count; i++) {
        if (snapshot->items[i].id == item->id) {
            return snapshot->items[i].generation == item->generation;
        }
    }
    return true;
}

static void snapshot_inventory_clear(session_snapshot_t *snapshot,
                                     uint32_t container_id,
                                     uint32_t preserve_container_id) {
    bool remove[SESSION_ITEMS_MAX] = {false};
    for (size_t i = 0; i < snapshot->items_count; i++) {
        bool preserve =
            preserve_container_id != 0 &&
            (snapshot->items[i].id == preserve_container_id ||
             snapshot_item_descends_from(snapshot, &snapshot->items[i], preserve_container_id));
        remove[i] =
            !preserve && snapshot_item_descends_from(snapshot, &snapshot->items[i], container_id);
    }
    size_t write = 0;
    for (size_t i = 0; i < snapshot->items_count; i++) {
        if (!remove[i]) {
            snapshot->items[write++] = snapshot->items[i];
        }
    }
    snapshot->items_count = write;
}

static bool snapshot_message_append(session_snapshot_t *snapshot,
                                    const session_message_t *message) {
    if (snapshot->messages_count == snapshot->limits.messages) {
        memmove(&snapshot->messages[0],
                &snapshot->messages[1],
                (snapshot->messages_count - 1) * sizeof(*snapshot->messages));
        snapshot->messages[snapshot->messages_count - 1] = *message;
        return true;
    }
    if (!snapshot_reserve((void **)&snapshot->messages,
                          &snapshot->messages_capacity,
                          snapshot->messages_count + 1,
                          snapshot->limits.messages,
                          sizeof(*snapshot->messages))) {
        return false;
    }
    snapshot->messages[snapshot->messages_count++] = *message;
    return true;
}

static bool snapshot_map_soft_clear(session_snapshot_t *snapshot, int x, int y, int depth) {
    bool metadata_found = false;
    for (size_t i = 0; i < snapshot->map_cells_count; i++) {
        const session_map_cell_t *cell = &snapshot->map_cells[i];
        metadata_found |= cell->x == x && cell->y == y && cell->depth == depth &&
                          cell->layer == SESSION_MAP_LAYER_METADATA && cell->sub_layer == 0;
    }
    bool insert_metadata =
        !metadata_found && snapshot->map_cells_count < snapshot->limits.map_cells;
    if (insert_metadata && !snapshot_reserve((void **)&snapshot->map_cells,
                                             &snapshot->map_cells_capacity,
                                             snapshot->map_cells_count + 1,
                                             snapshot->limits.map_cells,
                                             sizeof(*snapshot->map_cells))) {
        return false;
    }

    for (size_t i = 0; i < snapshot->map_cells_count; i++) {
        session_map_cell_t *cell = &snapshot->map_cells[i];
        if (cell->x != x || cell->y != y || cell->depth != depth) {
            continue;
        }
        cell->light_level = 0;
        cell->light_known = false;
        cell->fogged = true;
        cell->fog_known = true;
        cell->name[0] = '\0';
    }

    if (insert_metadata) {
        session_map_cell_t metadata = {
            .x = (int16_t)x,
            .y = (int16_t)y,
            .depth = (int8_t)depth,
            .layer = SESSION_MAP_LAYER_METADATA,
            .fogged = true,
            .fog_known = true,
        };
        return snapshot_map_cell_upsert(snapshot, &metadata);
    }
    return true;
}

static session_handle_t snapshot_map_handle(const session_snapshot_t *snapshot, uint32_t id) {
    session_handle_t handle = {0};
    size_t low = 0;
    size_t high = snapshot->map_entities_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (snapshot->map_entities[middle].id < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < snapshot->map_entities_count && snapshot->map_entities[low].id == id) {
        handle.session_generation = snapshot->session_generation;
        handle.collection_generation = snapshot->map_generation;
        handle.object_generation = snapshot->map_entities[low].generation;
        handle.id = id;
        handle.kind = SESSION_HANDLE_MAP_ENTITY;
    }
    return handle;
}

static session_handle_t snapshot_item_handle(const session_snapshot_t *snapshot, uint32_t id) {
    session_handle_t handle = {0};
    for (size_t i = 0; i < snapshot->items_count; i++) {
        if (snapshot->items[i].id != id) {
            continue;
        }
        handle.session_generation = snapshot->session_generation;
        handle.collection_generation = snapshot->inventory_generation;
        handle.object_generation = snapshot->items[i].generation;
        handle.id = id;
        handle.kind = SESSION_HANDLE_ITEM;
        break;
    }
    return handle;
}

static bool snapshot_open_container_valid(const session_snapshot_t *snapshot) {
    if (snapshot->open_container.id == 0) {
        return snapshot->open_container.kind == SESSION_HANDLE_NONE;
    }
    session_handle_t current = snapshot_item_handle(snapshot, snapshot->open_container.id);
    return snapshot->open_container.kind == SESSION_HANDLE_ITEM &&
           snapshot->open_container.session_generation == current.session_generation &&
           snapshot->open_container.collection_generation == current.collection_generation &&
           snapshot->open_container.object_generation == current.object_generation &&
           snapshot->open_container.id == current.id;
}

static bool map_position_shift(int16_t x,
                               int16_t y,
                               int8_t depth,
                               int x_offset,
                               int y_offset,
                               int depth_offset,
                               const session_map_t *map,
                               int16_t *shifted_x,
                               int16_t *shifted_y,
                               int8_t *shifted_depth) {
    int64_t new_x = (int64_t)x - x_offset;
    int64_t new_y = (int64_t)y - y_offset;
    int64_t new_depth = (int64_t)depth - depth_offset;
    if (map->width <= 0 || map->height <= 0 || new_x < 0 || new_x >= map->width || new_y < 0 ||
        new_y >= map->height || new_x > INT16_MAX || new_y > INT16_MAX || new_depth < INT8_MIN ||
        new_depth > INT8_MAX) {
        return false;
    }
    *shifted_x = (int16_t)new_x;
    *shifted_y = (int16_t)new_y;
    *shifted_depth = (int8_t)new_depth;
    return true;
}

static bool snapshot_event_valid(const session_snapshot_t *snapshot, const session_event_t *event) {
    switch (event->type) {
        case SESSION_EVENT_LIFECYCLE:
            return event->data.lifecycle.lifecycle >= SESSION_LIFECYCLE_DISCONNECTED &&
                   event->data.lifecycle.lifecycle <= SESSION_LIFECYCLE_PLAYING &&
                   event->data.lifecycle.session_generation != 0 &&
                   generation_current_or_next(snapshot->session_generation,
                                              event->data.lifecycle.session_generation);
        case SESSION_EVENT_CAPABILITIES:
            return true;
        case SESSION_EVENT_PLAYER:
            return string_valid(event->data.player.name, sizeof(event->data.player.name), true);
        case SESSION_EVENT_TARGET:
            return string_valid(event->data.target.name, sizeof(event->data.target.name), true) &&
                   string_valid(event->data.target.color, sizeof(event->data.target.color), true);
        case SESSION_EVENT_MAP:
            return map_dimensions_valid(&event->data.map) &&
                   event->data.map.width == snapshot->map.width &&
                   event->data.map.height == snapshot->map.height &&
                   string_valid(event->data.map.name, sizeof(event->data.map.name), true) &&
                   string_valid(event->data.map.path, sizeof(event->data.map.path), true);
        case SESSION_EVENT_MAP_RESET:
            return map_dimensions_valid(&event->data.map) &&
                   string_valid(event->data.map.name, sizeof(event->data.map.name), true) &&
                   string_valid(event->data.map.path, sizeof(event->data.map.path), true);
        case SESSION_EVENT_MAP_SCROLL:
            return map_dimensions_valid(&event->data.scroll.map) &&
                   string_valid(event->data.scroll.map.name,
                                sizeof(event->data.scroll.map.name),
                                true) &&
                   string_valid(event->data.scroll.map.path,
                                sizeof(event->data.scroll.map.path),
                                true);
        case SESSION_EVENT_MAP_CELL_UPSERT:
            return map_position_valid(&snapshot->map,
                                      event->data.map_cell.x,
                                      event->data.map_cell.y) &&
                   string_valid(event->data.map_cell.name, sizeof(event->data.map_cell.name), true);
        case SESSION_EVENT_MAP_CELL_CLEAR:
            return map_position_valid(&snapshot->map,
                                      event->data.map_cell_clear.x,
                                      event->data.map_cell_clear.y) &&
                   event->data.map_cell_clear.layer >= -1 &&
                   event->data.map_cell_clear.layer <= UINT8_MAX &&
                   event->data.map_cell_clear.sub_layer >= -1 &&
                   event->data.map_cell_clear.sub_layer <= UINT8_MAX;
        case SESSION_EVENT_MAP_CELL_SOFT_CLEAR:
            return map_position_valid(&snapshot->map,
                                      event->data.map_cell_clear.x,
                                      event->data.map_cell_clear.y);
        case SESSION_EVENT_MAP_ENTITY_UPSERT:
            return event->data.map_entity.id != 0 && event->data.map_entity.generation != 0 &&
                   snapshot_entity_generation_valid(snapshot, &event->data.map_entity) &&
                   map_position_valid(&snapshot->map,
                                      event->data.map_entity.x,
                                      event->data.map_entity.y) &&
                   string_valid(event->data.map_entity.name,
                                sizeof(event->data.map_entity.name),
                                true);
        case SESSION_EVENT_MAP_ENTITY_REMOVE: {
            session_handle_t handle = snapshot_map_handle(snapshot, event->data.removed.id);
            return handle.id != 0 && handle.object_generation == event->data.removed.generation;
        }
        case SESSION_EVENT_INVENTORY_REPLAY_BEGIN:
        case SESSION_EVENT_INVENTORY_REPLAY_END:
            return (!event->data.replay.open || event->data.replay.container_id != 0) &&
                   generation_current_or_next(snapshot->inventory_generation,
                                              event->data.replay.generation);
        case SESSION_EVENT_ITEM_UPSERT:
            return event->data.item.id != 0 && event->data.item.generation != 0 &&
                   event->data.item.category >= SESSION_ITEM_INVENTORY &&
                   event->data.item.category <= SESSION_ITEM_EFFECT &&
                   snapshot_item_generation_valid(snapshot, &event->data.item) &&
                   snapshot_item_parent_valid(snapshot, &event->data.item) &&
                   string_valid(event->data.item.name, sizeof(event->data.item.name), true) &&
                   string_valid(event->data.item.description,
                                sizeof(event->data.item.description),
                                true);
        case SESSION_EVENT_ITEM_REMOVE:
            for (size_t i = 0; i < snapshot->items_count; i++) {
                if (snapshot->items[i].id == event->data.removed.id) {
                    return snapshot->items[i].generation == event->data.removed.generation;
                }
            }
            return false;
        case SESSION_EVENT_DIALOG:
        case SESSION_EVENT_QUEST:
            return string_valid(event->data.document.title,
                                sizeof(event->data.document.title),
                                true) &&
                   string_valid(event->data.document.text, sizeof(event->data.document.text), true);
        case SESSION_EVENT_MESSAGE:
            return event->data.message.revision == event->revision &&
                   string_valid(event->data.message.color,
                                sizeof(event->data.message.color),
                                true) &&
                   string_valid(event->data.message.text, sizeof(event->data.message.text), true);
        case SESSION_EVENT_PARTY:
        case SESSION_EVENT_SERVER:
            return string_valid(event->data.text, sizeof(event->data.text), true);
        case SESSION_EVENT_PARTY_MEMBERS_CLEAR:
            return true;
        case SESSION_EVENT_PARTY_MEMBER_UPSERT:
        case SESSION_EVENT_PARTY_MEMBER_REMOVE:
            return string_valid(event->data.party_member.name,
                                sizeof(event->data.party_member.name),
                                false);
        case SESSION_EVENT_INTENT: {
            if (event->data.intent.pending_actions > snapshot->limits.actions) {
                return false;
            }
            const session_handle_t *target = &event->data.intent.target;
            if (target->id == 0) {
                return target->kind == SESSION_HANDLE_NONE;
            }
            session_handle_t current = snapshot_map_handle(snapshot, target->id);
            return target->kind == SESSION_HANDLE_MAP_ENTITY &&
                   target->session_generation == current.session_generation &&
                   target->collection_generation == current.collection_generation &&
                   target->object_generation == current.object_generation &&
                   target->id == current.id;
        }
    }
    return false;
}

bool session_snapshot_apply_event(session_snapshot_t *snapshot, const session_event_t *event) {
    if (!snapshot_valid(snapshot) || !snapshot_open_container_valid(snapshot) || event == NULL ||
        event->type < SESSION_EVENT_LIFECYCLE || event->type > SESSION_EVENT_INTENT ||
        event->revision != snapshot->revision + 1 || !snapshot_event_valid(snapshot, event)) {
        return false;
    }

    switch (event->type) {
        case SESSION_EVENT_LIFECYCLE:
            if (event->data.lifecycle.session_generation != snapshot->session_generation) {
                snapshot_generation_reset(snapshot);
                snapshot->session_generation = event->data.lifecycle.session_generation;
            }
            snapshot->lifecycle = event->data.lifecycle.lifecycle;
            break;
        case SESSION_EVENT_CAPABILITIES:
            snapshot->capabilities = event->data.capabilities;
            break;
        case SESSION_EVENT_PLAYER:
            snapshot->player = event->data.player;
            break;
        case SESSION_EVENT_TARGET:
            snapshot->target = event->data.target;
            snapshot->intent.combat = event->data.target.combat;
            snapshot->intent.combat_force = event->data.target.combat_force;
            if (event->data.target.id == 0 && event->data.target.code == 0) {
                memset(&snapshot->intent.target, 0, sizeof(snapshot->intent.target));
            } else if (event->data.target.id != 0) {
                snapshot->intent.target = snapshot_map_handle(snapshot, event->data.target.id);
            }
            break;
        case SESSION_EVENT_MAP:
            snapshot->map = event->data.map;
            break;
        case SESSION_EVENT_MAP_RESET:
            snapshot->map_generation++;
            snapshot->map = event->data.map;
            snapshot->map_cells_count = 0;
            snapshot->map_entities_count = 0;
            memset(&snapshot->target, 0, sizeof(snapshot->target));
            memset(&snapshot->intent.target, 0, sizeof(snapshot->intent.target));
            break;
        case SESSION_EVENT_MAP_SCROLL:
            snapshot->map_generation++;
            snapshot->map = event->data.scroll.map;
            memset(&snapshot->target, 0, sizeof(snapshot->target));
            memset(&snapshot->intent.target, 0, sizeof(snapshot->intent.target));
            for (size_t i = 0; i < snapshot->map_cells_count;) {
                session_map_cell_t *cell = &snapshot->map_cells[i];
                int16_t shifted_x, shifted_y;
                int8_t shifted_depth;
                if (map_position_shift(cell->x,
                                       cell->y,
                                       cell->depth,
                                       event->data.scroll.x_offset,
                                       event->data.scroll.y_offset,
                                       event->data.scroll.depth_offset,
                                       &snapshot->map,
                                       &shifted_x,
                                       &shifted_y,
                                       &shifted_depth)) {
                    cell->x = shifted_x;
                    cell->y = shifted_y;
                    cell->depth = shifted_depth;
                    i++;
                } else {
                    memmove(&snapshot->map_cells[i],
                            &snapshot->map_cells[i + 1],
                            (snapshot->map_cells_count - i - 1) * sizeof(*snapshot->map_cells));
                    snapshot->map_cells_count--;
                }
            }
            for (size_t i = 0; i < snapshot->map_entities_count;) {
                session_map_entity_t *entity = &snapshot->map_entities[i];
                int16_t shifted_x, shifted_y;
                int8_t shifted_depth;
                if (map_position_shift(entity->x,
                                       entity->y,
                                       entity->depth,
                                       event->data.scroll.x_offset,
                                       event->data.scroll.y_offset,
                                       event->data.scroll.depth_offset,
                                       &snapshot->map,
                                       &shifted_x,
                                       &shifted_y,
                                       &shifted_depth)) {
                    entity->x = shifted_x;
                    entity->y = shifted_y;
                    entity->depth = shifted_depth;
                    i++;
                } else {
                    memmove(&snapshot->map_entities[i],
                            &snapshot->map_entities[i + 1],
                            (snapshot->map_entities_count - i - 1) *
                                sizeof(*snapshot->map_entities));
                    snapshot->map_entities_count--;
                }
            }
            break;
        case SESSION_EVENT_MAP_CELL_UPSERT:
            if (!snapshot_map_cell_upsert(snapshot, &event->data.map_cell)) {
                return false;
            }
            break;
        case SESSION_EVENT_MAP_CELL_CLEAR:
            for (size_t i = 0; i < snapshot->map_cells_count;) {
                const session_map_cell_t *cell = &snapshot->map_cells[i];
                bool matches = cell->x == event->data.map_cell_clear.x &&
                               cell->y == event->data.map_cell_clear.y &&
                               cell->depth == event->data.map_cell_clear.depth &&
                               (event->data.map_cell_clear.layer < 0 ||
                                cell->layer == event->data.map_cell_clear.layer) &&
                               (event->data.map_cell_clear.sub_layer < 0 ||
                                cell->sub_layer == event->data.map_cell_clear.sub_layer);
                if (!matches) {
                    i++;
                    continue;
                }
                memmove(&snapshot->map_cells[i],
                        &snapshot->map_cells[i + 1],
                        (snapshot->map_cells_count - i - 1) * sizeof(*snapshot->map_cells));
                snapshot->map_cells_count--;
            }
            break;
        case SESSION_EVENT_MAP_CELL_SOFT_CLEAR:
            if (!snapshot_map_soft_clear(snapshot,
                                         event->data.map_cell_clear.x,
                                         event->data.map_cell_clear.y,
                                         event->data.map_cell_clear.depth)) {
                return false;
            }
            break;
        case SESSION_EVENT_MAP_ENTITY_UPSERT:
            if (!snapshot_map_entity_upsert(snapshot, &event->data.map_entity)) {
                return false;
            }
            break;
        case SESSION_EVENT_MAP_ENTITY_REMOVE:
            snapshot_remove_entity(snapshot, event->data.removed.id);
            break;
        case SESSION_EVENT_INVENTORY_REPLAY_BEGIN:
            if (event->data.replay.generation != snapshot->inventory_generation) {
                uint32_t preserve_container_id =
                    snapshot->open_container.id != event->data.replay.container_id
                        ? snapshot->open_container.id
                        : 0;
                snapshot_inventory_clear(snapshot,
                                         event->data.replay.container_id,
                                         preserve_container_id);
                snapshot->inventory_generation = event->data.replay.generation;
                snapshot->open_container = snapshot_item_handle(snapshot, preserve_container_id);
            }
            if (event->data.replay.open) {
                snapshot->open_container =
                    snapshot_item_handle(snapshot, event->data.replay.container_id);
            } else if (snapshot->open_container.id == event->data.replay.container_id) {
                memset(&snapshot->open_container, 0, sizeof(snapshot->open_container));
            }
            break;
        case SESSION_EVENT_ITEM_UPSERT:
            if (!snapshot_item_upsert(snapshot, &event->data.item)) {
                return false;
            }
            break;
        case SESSION_EVENT_ITEM_REMOVE:
            if (snapshot->open_container.id == event->data.removed.id) {
                memset(&snapshot->open_container, 0, sizeof(snapshot->open_container));
            }
            snapshot_remove_item(snapshot, event->data.removed.id);
            break;
        case SESSION_EVENT_INVENTORY_REPLAY_END:
            break;
        case SESSION_EVENT_DIALOG:
            snapshot->dialog = event->data.document;
            break;
        case SESSION_EVENT_QUEST:
            snapshot->quest = event->data.document;
            break;
        case SESSION_EVENT_MESSAGE:
            if (!snapshot_message_append(snapshot, &event->data.message)) {
                return false;
            }
            break;
        case SESSION_EVENT_PARTY:
            copy_string(snapshot->party, sizeof(snapshot->party), event->data.text);
            break;
        case SESSION_EVENT_PARTY_MEMBERS_CLEAR:
            snapshot->party_members_count = 0;
            break;
        case SESSION_EVENT_PARTY_MEMBER_UPSERT: {
            size_t index = 0;
            while (index < snapshot->party_members_count &&
                   strcmp(snapshot->party_members[index].name, event->data.party_member.name) < 0) {
                index++;
            }
            if (index < snapshot->party_members_count &&
                strcmp(snapshot->party_members[index].name, event->data.party_member.name) == 0) {
                snapshot->party_members[index] = event->data.party_member;
            } else {
                if (snapshot->party_members_count == SESSION_PARTY_MEMBERS_MAX) {
                    return false;
                }
                memmove(&snapshot->party_members[index + 1],
                        &snapshot->party_members[index],
                        (snapshot->party_members_count - index) * sizeof(*snapshot->party_members));
                snapshot->party_members[index] = event->data.party_member;
                snapshot->party_members_count++;
            }
            break;
        }
        case SESSION_EVENT_PARTY_MEMBER_REMOVE:
            for (size_t i = 0; i < snapshot->party_members_count; i++) {
                if (strcmp(snapshot->party_members[i].name, event->data.party_member.name) != 0) {
                    continue;
                }
                memmove(&snapshot->party_members[i],
                        &snapshot->party_members[i + 1],
                        (snapshot->party_members_count - i - 1) * sizeof(*snapshot->party_members));
                snapshot->party_members_count--;
                break;
            }
            break;
        case SESSION_EVENT_SERVER:
            copy_string(snapshot->server, sizeof(snapshot->server), event->data.text);
            break;
        case SESSION_EVENT_INTENT:
            snapshot->intent = event->data.intent;
            break;
    }

    snapshot->revision = event->revision;
    return true;
}

bool session_player_view(const session_t *session, session_player_t *player) {
    if (session == NULL || player == NULL) {
        return false;
    }

    *player = session->player;
    return true;
}

bool session_target_view(const session_t *session, session_target_t *target) {
    if (session == NULL || target == NULL) {
        return false;
    }

    *target = session->target;
    return true;
}

bool session_map_view(const session_t *session, session_map_t *map) {
    if (session == NULL || map == NULL) {
        return false;
    }

    *map = session->map;
    return true;
}

bool session_dialog_view(const session_t *session, session_document_t *dialog) {
    if (session == NULL || dialog == NULL) {
        return false;
    }
    *dialog = session->dialog;
    return true;
}

bool session_quest_view(const session_t *session, session_document_t *quest) {
    if (session == NULL || quest == NULL) {
        return false;
    }
    *quest = session->quest;
    return true;
}

bool session_party_name_view(const session_t *session, char *party, size_t size) {
    if (session == NULL || party == NULL || size == 0) {
        return false;
    }
    copy_string(party, size, session->party);
    return true;
}

bool session_map_entity_view(const session_t *session,
                             session_handle_t handle,
                             session_map_entity_t *entity) {
    if (entity == NULL || !session_handle_valid(session, handle) ||
        handle.kind != SESSION_HANDLE_MAP_ENTITY) {
        return false;
    }

    bool found;
    size_t index = map_entity_index(session, handle.id, &found);
    if (!found) {
        return false;
    }
    *entity = session->map_entities[index];
    return true;
}

bool session_item_view(const session_t *session, session_handle_t handle, session_item_t *item) {
    if (item == NULL || !session_handle_valid(session, handle) ||
        handle.kind != SESSION_HANDLE_ITEM) {
        return false;
    }

    bool found;
    size_t index = item_index(session, handle.id, &found);
    if (!found) {
        return false;
    }
    *item = session->items[index];
    return true;
}

bool session_intent_view(const session_t *session, session_intent_t *intent) {
    if (session == NULL || intent == NULL) {
        return false;
    }

    *intent = session->intent;
    return true;
}

session_events_result_t session_events_read(const session_t *session,
                                            uint64_t after_revision,
                                            session_event_t *events,
                                            size_t capacity,
                                            size_t *count) {
    if (session == NULL || count == NULL || (capacity > 0 && events == NULL) ||
        after_revision > session->revision) {
        return SESSION_EVENTS_INVALID;
    }

    *count = 0;
    if (session->events_count == 0 || after_revision == session->revision) {
        return SESSION_EVENTS_OK;
    }

    uint64_t oldest = session->events[session->events_head].revision;
    if (after_revision + 1 < oldest) {
        return SESSION_EVENTS_GAP;
    }

    for (size_t i = 0; i < session->events_count && *count < capacity; i++) {
        size_t source = (session->events_head + i) % session->events_capacity;
        if (session->events[source].revision > after_revision) {
            events[(*count)++] = session->events[source];
        }
    }

    return SESSION_EVENTS_OK;
}

static void lifecycle_reset(session_t *session, session_lifecycle_t lifecycle) {
    session->session_generation++;
    session->map_generation++;
    session->inventory_generation++;
    session->capabilities = 0;
    state_clear(session);
    session->lifecycle = lifecycle;

    session_event_t event = {.type = SESSION_EVENT_LIFECYCLE};
    event.data.lifecycle.lifecycle = lifecycle;
    event.data.lifecycle.session_generation = session->session_generation;
    event_emit(session, &event);
}

void session_reduce_connect(session_t *session, uint64_t capabilities, const char *server) {
    if (session == NULL) {
        return;
    }

    lifecycle_reset(session, SESSION_LIFECYCLE_CONNECTED);
    session->capabilities = capabilities;
    copy_string(session->server, sizeof(session->server), server);

    session_event_t capabilities_event = {.type = SESSION_EVENT_CAPABILITIES};
    capabilities_event.data.capabilities = capabilities;
    event_emit(session, &capabilities_event);

    session_event_t server_event = {.type = SESSION_EVENT_SERVER};
    copy_string(server_event.data.text, sizeof(server_event.data.text), session->server);
    event_emit(session, &server_event);
}

void session_reduce_play(session_t *session, const session_player_t *player) {
    if (session == NULL || player == NULL) {
        return;
    }

    session->lifecycle = SESSION_LIFECYCLE_PLAYING;
    session->player = *player;
    copy_string(session->player.name, sizeof(session->player.name), player->name);

    session_event_t event = {.type = SESSION_EVENT_PLAYER};
    event.data.player = session->player;
    event_emit(session, &event);

    session_event_t lifecycle_event = {.type = SESSION_EVENT_LIFECYCLE};
    lifecycle_event.data.lifecycle.lifecycle = session->lifecycle;
    lifecycle_event.data.lifecycle.session_generation = session->session_generation;
    event_emit(session, &lifecycle_event);
}

void session_reduce_disconnect(session_t *session) {
    if (session != NULL && session->lifecycle != SESSION_LIFECYCLE_DISCONNECTED) {
        lifecycle_reset(session, SESSION_LIFECYCLE_DISCONNECTED);
    }
}

void session_reduce_character_reset(session_t *session) {
    if (session == NULL) {
        return;
    }

    session_lifecycle_t lifecycle = session->lifecycle == SESSION_LIFECYCLE_DISCONNECTED
                                        ? SESSION_LIFECYCLE_DISCONNECTED
                                        : SESSION_LIFECYCLE_CONNECTED;
    lifecycle_reset(session, lifecycle);
}

void session_reduce_capabilities(session_t *session, uint64_t capabilities) {
    if (session == NULL || session->capabilities == capabilities) {
        return;
    }

    session->capabilities = capabilities;
    session_event_t event = {.type = SESSION_EVENT_CAPABILITIES};
    event.data.capabilities = capabilities;
    event_emit(session, &event);
}

void session_reduce_player(session_t *session, const session_player_t *player) {
    if (session == NULL || player == NULL) {
        return;
    }

    session_player_t updated = *player;
    copy_string(updated.name, sizeof(updated.name), player->name);
    if (player_equal(&session->player, &updated)) {
        return;
    }
    session->player = updated;
    session_event_t event = {.type = SESSION_EVENT_PLAYER};
    event.data.player = session->player;
    event_emit(session, &event);
}

void session_reduce_target(session_t *session, const session_target_t *target) {
    if (session == NULL || target == NULL) {
        return;
    }

    session_target_t updated = *target;
    copy_string(updated.name, sizeof(updated.name), target->name);
    copy_string(updated.color, sizeof(updated.color), target->color);
    session->target = updated;
    session->intent.combat = updated.combat;
    session->intent.combat_force = updated.combat_force;
    if (updated.id == 0 && updated.code == 0) {
        memset(&session->intent.target, 0, sizeof(session->intent.target));
    } else if (updated.id != 0) {
        session->intent.target = session_map_handle(session, updated.id);
    }

    session_event_t event = {.type = SESSION_EVENT_TARGET};
    event.data.target = session->target;
    event_emit(session, &event);
}

void session_reduce_map(session_t *session, const session_map_t *map) {
    if (session == NULL || !map_dimensions_valid(&session->map) || !map_dimensions_valid(map) ||
        map->width != session->map.width || map->height != session->map.height) {
        return;
    }

    session_map_t updated = *map;
    copy_string(updated.name, sizeof(updated.name), map->name);
    copy_string(updated.path, sizeof(updated.path), map->path);
    if (map_equal(&session->map, &updated)) {
        return;
    }
    session->map = updated;
    session_event_t event = {.type = SESSION_EVENT_MAP};
    event.data.map = session->map;
    event_emit(session, &event);
}

static void map_invalidate(session_t *session) {
    session->map_generation++;
    session->map_cells_count = 0;
    session->map_entities_count = 0;
    memset(&session->target, 0, sizeof(session->target));
    memset(&session->intent.target, 0, sizeof(session->intent.target));
}

void session_reduce_map_reset(session_t *session, const session_map_t *map) {
    if (session == NULL || !map_dimensions_valid(map)) {
        return;
    }

    map_invalidate(session);
    session->map = *map;
    copy_string(session->map.name, sizeof(session->map.name), map->name);
    copy_string(session->map.path, sizeof(session->map.path), map->path);
    session_event_t event = {.type = SESSION_EVENT_MAP_RESET};
    event.data.map = session->map;
    event_emit(session, &event);
}

void session_reduce_map_scroll(session_t *session,
                               int x_offset,
                               int y_offset,
                               int depth_offset,
                               const session_map_t *map) {
    if (session == NULL || !map_dimensions_valid(map)) {
        return;
    }

    session->map_generation++;
    memset(&session->target, 0, sizeof(session->target));
    memset(&session->intent.target, 0, sizeof(session->intent.target));
    for (size_t i = 0; i < session->map_cells_count;) {
        session_map_cell_t *cell = &session->map_cells[i];
        int16_t shifted_x, shifted_y;
        int8_t shifted_depth;
        if (map_position_shift(cell->x,
                               cell->y,
                               cell->depth,
                               x_offset,
                               y_offset,
                               depth_offset,
                               map,
                               &shifted_x,
                               &shifted_y,
                               &shifted_depth)) {
            cell->x = shifted_x;
            cell->y = shifted_y;
            cell->depth = shifted_depth;
            i++;
            continue;
        }
        memmove(&session->map_cells[i],
                &session->map_cells[i + 1],
                (session->map_cells_count - i - 1) * sizeof(*session->map_cells));
        session->map_cells_count--;
    }
    for (size_t i = 0; i < session->map_entities_count;) {
        session_map_entity_t *entity = &session->map_entities[i];
        int16_t shifted_x, shifted_y;
        int8_t shifted_depth;
        if (map_position_shift(entity->x,
                               entity->y,
                               entity->depth,
                               x_offset,
                               y_offset,
                               depth_offset,
                               map,
                               &shifted_x,
                               &shifted_y,
                               &shifted_depth)) {
            entity->x = shifted_x;
            entity->y = shifted_y;
            entity->depth = shifted_depth;
            i++;
            continue;
        }
        memmove(&session->map_entities[i],
                &session->map_entities[i + 1],
                (session->map_entities_count - i - 1) * sizeof(*session->map_entities));
        session->map_entities_count--;
    }
    session->map = *map;
    copy_string(session->map.name, sizeof(session->map.name), map->name);
    copy_string(session->map.path, sizeof(session->map.path), map->path);
    session_event_t event = {.type = SESSION_EVENT_MAP_SCROLL};
    event.data.scroll.x_offset = x_offset;
    event.data.scroll.y_offset = y_offset;
    event.data.scroll.depth_offset = depth_offset;
    event.data.scroll.map = session->map;
    event_emit(session, &event);
}

static int map_cell_compare(const session_map_cell_t *cell,
                            int x,
                            int y,
                            int depth,
                            uint8_t layer,
                            uint8_t sub_layer) {
    if (cell->depth != depth) {
        return cell->depth < depth ? -1 : 1;
    }
    if (cell->y != y) {
        return cell->y < y ? -1 : 1;
    }
    if (cell->x != x) {
        return cell->x < x ? -1 : 1;
    }
    if (cell->sub_layer != sub_layer) {
        return cell->sub_layer < sub_layer ? -1 : 1;
    }
    if (cell->layer != layer) {
        return cell->layer < layer ? -1 : 1;
    }
    return 0;
}

static size_t map_cell_index(const session_t *session,
                             int x,
                             int y,
                             int depth,
                             uint8_t layer,
                             uint8_t sub_layer,
                             bool *found) {
    size_t low = 0;
    size_t high = session->map_cells_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (map_cell_compare(&session->map_cells[middle], x, y, depth, layer, sub_layer) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    *found = low < session->map_cells_count &&
             map_cell_compare(&session->map_cells[low], x, y, depth, layer, sub_layer) == 0;
    return low;
}

bool session_reduce_map_cell(session_t *session, const session_map_cell_t *cell) {
    if (session == NULL || cell == NULL || !map_position_valid(&session->map, cell->x, cell->y)) {
        return false;
    }

    bool found;
    size_t index = map_cell_index(session,
                                  cell->x,
                                  cell->y,
                                  cell->depth,
                                  cell->layer,
                                  cell->sub_layer,
                                  &found);
    session_map_cell_t updated = *cell;
    copy_string(updated.name, sizeof(updated.name), cell->name);
    if (!found) {
        if (session->map_cells_count == session->map_cells_capacity) {
            return false;
        }
        memmove(&session->map_cells[index + 1],
                &session->map_cells[index],
                (session->map_cells_count - index) * sizeof(*session->map_cells));
        session->map_cells_count++;
    } else {
        if (!updated.light_known) {
            updated.light_level = session->map_cells[index].light_level;
            updated.light_known = session->map_cells[index].light_known;
        }
        if (!updated.fog_known) {
            updated.fogged = session->map_cells[index].fogged;
            updated.fog_known = session->map_cells[index].fog_known;
        }
    }
    session->map_cells[index] = updated;

    session_event_t event = {.type = SESSION_EVENT_MAP_CELL_UPSERT};
    event.data.map_cell = updated;
    event_emit(session, &event);
    return true;
}

bool session_reduce_map_layer_clear(session_t *session,
                                    int x,
                                    int y,
                                    int depth,
                                    uint8_t layer,
                                    uint8_t sub_layer) {
    if (session == NULL || !map_position_valid(&session->map, x, y)) {
        return false;
    }

    bool found;
    size_t index = map_cell_index(session, x, y, depth, layer, sub_layer, &found);
    if (found) {
        memmove(&session->map_cells[index],
                &session->map_cells[index + 1],
                (session->map_cells_count - index - 1) * sizeof(*session->map_cells));
        session->map_cells_count--;
    }

    session_event_t cleared = {.type = SESSION_EVENT_MAP_CELL_CLEAR};
    cleared.data.map_cell_clear.x = (int16_t)x;
    cleared.data.map_cell_clear.y = (int16_t)y;
    cleared.data.map_cell_clear.depth = (int8_t)depth;
    cleared.data.map_cell_clear.layer = layer;
    cleared.data.map_cell_clear.sub_layer = sub_layer;
    event_emit(session, &cleared);
    return found;
}

static size_t map_entity_index(const session_t *session, uint32_t id, bool *found) {
    size_t low = 0;
    size_t high = session->map_entities_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (session->map_entities[middle].id < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    *found = low < session->map_entities_count && session->map_entities[low].id == id;
    return low;
}

bool session_reduce_map_entity(session_t *session, const session_map_entity_t *entity) {
    if (session == NULL || entity == NULL || entity->id == 0 ||
        !map_position_valid(&session->map, entity->x, entity->y)) {
        return false;
    }

    for (size_t i = 0; i < session->map_entities_count;) {
        session_map_entity_t existing = session->map_entities[i];
        if (existing.id == entity->id || existing.x != entity->x || existing.y != entity->y ||
            existing.depth != entity->depth || existing.sub_layer != entity->sub_layer) {
            i++;
            continue;
        }

        memmove(&session->map_entities[i],
                &session->map_entities[i + 1],
                (session->map_entities_count - i - 1) * sizeof(*session->map_entities));
        session->map_entities_count--;
        session_event_t removed = {.type = SESSION_EVENT_MAP_ENTITY_REMOVE};
        removed.data.removed.id = existing.id;
        removed.data.removed.generation = existing.generation;
        event_emit(session, &removed);
    }

    bool found;
    size_t index = map_entity_index(session, entity->id, &found);
    session_map_entity_t updated = *entity;
    copy_string(updated.name, sizeof(updated.name), entity->name);
    if (found) {
        updated.generation = session->map_entities[index].generation;
    } else {
        if (session->map_entities_count == session->map_entities_capacity) {
            return false;
        }

        memmove(&session->map_entities[index + 1],
                &session->map_entities[index],
                (session->map_entities_count - index) * sizeof(*session->map_entities));
        session->map_entities_count++;
        updated.generation = session->next_object_generation++;
    }

    session->map_entities[index] = updated;
    session_event_t event = {.type = SESSION_EVENT_MAP_ENTITY_UPSERT};
    event.data.map_entity = updated;
    event_emit(session, &event);
    return true;
}

bool session_reduce_map_entity_remove(session_t *session, uint32_t id) {
    if (session == NULL || id == 0) {
        return false;
    }

    bool found;
    size_t index = map_entity_index(session, id, &found);
    if (!found) {
        return false;
    }

    uint64_t generation = session->map_entities[index].generation;
    memmove(&session->map_entities[index],
            &session->map_entities[index + 1],
            (session->map_entities_count - index - 1) * sizeof(*session->map_entities));
    session->map_entities_count--;

    session_event_t event = {.type = SESSION_EVENT_MAP_ENTITY_REMOVE};
    event.data.removed.id = id;
    event.data.removed.generation = generation;
    event_emit(session, &event);
    return true;
}

size_t
session_reduce_map_entities_clear(session_t *session, int x, int y, int depth, int sub_layer) {
    if (session == NULL || !map_position_valid(&session->map, x, y)) {
        return 0;
    }

    size_t removed = 0;
    for (size_t i = 0; i < session->map_entities_count;) {
        session_map_entity_t entity = session->map_entities[i];
        if (entity.x != x || entity.y != y || entity.depth != depth ||
            (sub_layer >= 0 && entity.sub_layer != sub_layer)) {
            i++;
            continue;
        }

        memmove(&session->map_entities[i],
                &session->map_entities[i + 1],
                (session->map_entities_count - i - 1) * sizeof(*session->map_entities));
        session->map_entities_count--;

        session_event_t event = {.type = SESSION_EVENT_MAP_ENTITY_REMOVE};
        event.data.removed.id = entity.id;
        event.data.removed.generation = entity.generation;
        event_emit(session, &event);
        removed++;
    }
    return removed;
}

size_t session_reduce_map_cell_clear(session_t *session, int x, int y, int depth, int sub_layer) {
    if (session == NULL || !map_position_valid(&session->map, x, y)) {
        return 0;
    }

    size_t removed = session_reduce_map_entities_clear(session, x, y, depth, sub_layer);
    for (size_t i = 0; i < session->map_cells_count;) {
        session_map_cell_t cell = session->map_cells[i];
        if (cell.x != x || cell.y != y || cell.depth != depth ||
            (sub_layer >= 0 && cell.sub_layer != sub_layer)) {
            i++;
            continue;
        }

        memmove(&session->map_cells[i],
                &session->map_cells[i + 1],
                (session->map_cells_count - i - 1) * sizeof(*session->map_cells));
        session->map_cells_count--;
        removed++;
    }

    session_event_t cleared = {.type = SESSION_EVENT_MAP_CELL_CLEAR};
    cleared.data.map_cell_clear.x = (int16_t)x;
    cleared.data.map_cell_clear.y = (int16_t)y;
    cleared.data.map_cell_clear.depth = (int8_t)depth;
    cleared.data.map_cell_clear.layer = -1;
    cleared.data.map_cell_clear.sub_layer = (int16_t)sub_layer;
    event_emit(session, &cleared);
    return removed;
}

size_t session_reduce_map_cell_soft_clear(session_t *session, int x, int y, int depth) {
    if (session == NULL || !map_position_valid(&session->map, x, y)) {
        return 0;
    }

    size_t updated = session_reduce_map_entities_clear(session, x, y, depth, -1);
    bool metadata_found = false;
    for (size_t i = 0; i < session->map_cells_count; i++) {
        session_map_cell_t *cell = &session->map_cells[i];
        if (cell->x != x || cell->y != y || cell->depth != depth) {
            continue;
        }
        metadata_found |= cell->layer == SESSION_MAP_LAYER_METADATA && cell->sub_layer == 0;
        cell->light_level = 0;
        cell->light_known = false;
        cell->fogged = true;
        cell->fog_known = true;
        cell->name[0] = '\0';
        updated++;
    }

    if (!metadata_found && session->map_cells_count < session->map_cells_capacity) {
        session_map_cell_t metadata = {
            .x = (int16_t)x,
            .y = (int16_t)y,
            .depth = (int8_t)depth,
            .layer = SESSION_MAP_LAYER_METADATA,
            .fogged = true,
            .fog_known = true,
        };
        bool found;
        size_t index = map_cell_index(session,
                                      metadata.x,
                                      metadata.y,
                                      metadata.depth,
                                      metadata.layer,
                                      metadata.sub_layer,
                                      &found);
        if (!found) {
            memmove(&session->map_cells[index + 1],
                    &session->map_cells[index],
                    (session->map_cells_count - index) * sizeof(*session->map_cells));
            session->map_cells[index] = metadata;
            session->map_cells_count++;
            updated++;
        }
    }

    session_event_t event = {.type = SESSION_EVENT_MAP_CELL_SOFT_CLEAR};
    event.data.map_cell_clear.x = (int16_t)x;
    event.data.map_cell_clear.y = (int16_t)y;
    event.data.map_cell_clear.depth = (int8_t)depth;
    event_emit(session, &event);
    return updated;
}

static size_t item_index(const session_t *session, uint32_t id, bool *found) {
    size_t low = 0;
    size_t high = session->items_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (session->items[middle].id < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    *found = low < session->items_count && session->items[low].id == id;
    return low;
}

static bool
item_descends_from(const session_t *session, const session_item_t *item, uint32_t container_id) {
    if (item->container_id == container_id) {
        return true;
    }

    uint32_t parent = item->container_id;
    for (size_t depth = 0; parent != 0 && depth <= session->items_count; depth++) {
        if (parent == container_id) {
            return true;
        }

        bool found;
        size_t index = item_index(session, parent, &found);
        if (!found) {
            return false;
        }
        parent = session->items[index].container_id;
        if (parent == container_id) {
            return true;
        }
    }
    return false;
}

static bool item_parent_valid(const session_t *session, const session_item_t *item) {
    uint32_t parent = item->container_id;
    for (size_t depth = 0; parent != 0 && depth <= session->items_count; depth++) {
        if (parent == item->id) {
            return false;
        }
        bool found;
        size_t index = item_index(session, parent, &found);
        if (!found) {
            return true;
        }
        parent = session->items[index].container_id;
    }
    return parent == 0;
}

void session_reduce_inventory_begin(session_t *session,
                                    uint32_t container_id,
                                    bool clear,
                                    bool open) {
    if (session == NULL || (open && container_id == 0)) {
        return;
    }

    if (clear) {
        session->inventory_generation++;
        uint32_t preserve_container_id =
            session->open_container_id != container_id ? session->open_container_id : 0;
        bool remove[SESSION_ITEMS_MAX] = {false};
        for (size_t i = 0; i < session->items_count; i++) {
            bool preserve =
                preserve_container_id != 0 &&
                (session->items[i].id == preserve_container_id ||
                 item_descends_from(session, &session->items[i], preserve_container_id));
            remove[i] = !preserve && item_descends_from(session, &session->items[i], container_id);
        }
        size_t write = 0;
        for (size_t i = 0; i < session->items_count; i++) {
            if (!remove[i]) {
                session->items[write++] = session->items[i];
            }
        }
        session->items_count = write;
    }

    if (open) {
        session->open_container_id = container_id;
    } else if (session->open_container_id == container_id) {
        session->open_container_id = 0;
    }

    session_event_t event = {.type = SESSION_EVENT_INVENTORY_REPLAY_BEGIN};
    event.data.replay.container_id = container_id;
    event.data.replay.generation = session->inventory_generation;
    event.data.replay.open = open;
    event_emit(session, &event);
    session->inventory_replay_depth++;
    if (clear) {
        player_weight_recalculate(session);
    }
}

bool session_reduce_item(session_t *session, const session_item_t *item) {
    if (session == NULL || item == NULL || item->id == 0 ||
        item->category < SESSION_ITEM_INVENTORY || item->category > SESSION_ITEM_EFFECT ||
        !item_parent_valid(session, item)) {
        return false;
    }

    bool found;
    size_t index = item_index(session, item->id, &found);
    session_item_t updated = *item;
    copy_string(updated.name, sizeof(updated.name), item->name);
    copy_string(updated.description, sizeof(updated.description), item->description);
    if (found) {
        updated.generation = session->items[index].generation;
    } else {
        if (session->items_count == session->items_capacity) {
            return false;
        }

        memmove(&session->items[index + 1],
                &session->items[index],
                (session->items_count - index) * sizeof(*session->items));
        session->items_count++;
        updated.generation = session->next_object_generation++;
    }

    session->items[index] = updated;
    session_event_t event = {.type = SESSION_EVENT_ITEM_UPSERT};
    event.data.item = updated;
    event_emit(session, &event);
    if (session->inventory_replay_depth == 0) {
        player_weight_recalculate(session);
    }
    return true;
}

bool session_reduce_item_remove(session_t *session, uint32_t id) {
    if (session == NULL || id == 0) {
        return false;
    }

    bool found;
    item_index(session, id, &found);
    if (!found) {
        return false;
    }

    bool remove[SESSION_ITEMS_MAX] = {false};
    for (size_t i = 0; i < session->items_count; i++) {
        remove[i] =
            session->items[i].id == id || item_descends_from(session, &session->items[i], id);
    }

    size_t write = 0;
    for (size_t i = 0; i < session->items_count; i++) {
        session_item_t item = session->items[i];
        if (!remove[i]) {
            session->items[write++] = item;
            continue;
        }

        if (session->open_container_id == item.id) {
            session->open_container_id = 0;
        }

        session_event_t event = {.type = SESSION_EVENT_ITEM_REMOVE};
        event.data.removed.id = item.id;
        event.data.removed.generation = item.generation;
        event_emit(session, &event);
    }
    session->items_count = write;
    if (session->inventory_replay_depth == 0) {
        player_weight_recalculate(session);
    }
    return true;
}

void session_reduce_inventory_end(session_t *session, uint32_t container_id) {
    if (session == NULL) {
        return;
    }

    session_event_t event = {.type = SESSION_EVENT_INVENTORY_REPLAY_END};
    event.data.replay.container_id = container_id;
    event.data.replay.generation = session->inventory_generation;
    event_emit(session, &event);
    if (session->inventory_replay_depth > 0) {
        session->inventory_replay_depth--;
    }
    if (session->inventory_replay_depth == 0) {
        player_weight_recalculate(session);
    }
}

static void document_reduce(session_t *session,
                            session_document_t *document,
                            session_event_type_t type,
                            const char *title,
                            const char *text) {
    document->generation++;
    copy_string(document->title, sizeof(document->title), title);
    copy_string(document->text, sizeof(document->text), text);

    session_event_t event = {.type = type};
    event.data.document = *document;
    event_emit(session, &event);
}

void session_reduce_dialog(session_t *session, const char *title, const char *text) {
    if (session != NULL) {
        document_reduce(session, &session->dialog, SESSION_EVENT_DIALOG, title, text);
    }
}

void session_reduce_quest(session_t *session, const char *title, const char *text) {
    if (session != NULL) {
        document_reduce(session, &session->quest, SESSION_EVENT_QUEST, title, text);
    }
}

void session_reduce_message(session_t *session, uint8_t type, const char *color, const char *text) {
    if (session == NULL) {
        return;
    }

    session_message_t message = {.revision = session->revision + 1, .type = type};
    copy_string(message.color, sizeof(message.color), color);
    copy_string(message.text, sizeof(message.text), text);

    size_t slot;
    if (session->messages_count < session->messages_capacity) {
        slot = (session->messages_head + session->messages_count) % session->messages_capacity;
        session->messages_count++;
    } else {
        slot = session->messages_head;
        session->messages_head = (session->messages_head + 1) % session->messages_capacity;
    }
    session->messages[slot] = message;

    session_event_t event = {.type = SESSION_EVENT_MESSAGE};
    event.data.message = message;
    event_emit(session, &event);
}

void session_reduce_party(session_t *session, const char *party) {
    if (session == NULL) {
        return;
    }

    copy_string(session->party, sizeof(session->party), party);
    session_event_t event = {.type = SESSION_EVENT_PARTY};
    copy_string(event.data.text, sizeof(event.data.text), session->party);
    event_emit(session, &event);
}

void session_reduce_party_members_clear(session_t *session) {
    if (session == NULL) {
        return;
    }
    session->party_members_count = 0;
    session_event_t event = {.type = SESSION_EVENT_PARTY_MEMBERS_CLEAR};
    event_emit(session, &event);
}

bool session_reduce_party_member(session_t *session, const session_party_member_t *member) {
    if (session == NULL || member == NULL ||
        !string_valid(member->name, sizeof(member->name), false)) {
        return false;
    }

    size_t index = 0;
    while (index < session->party_members_count &&
           strcmp(session->party_members[index].name, member->name) < 0) {
        index++;
    }
    if (index < session->party_members_count &&
        strcmp(session->party_members[index].name, member->name) == 0) {
        session->party_members[index] = *member;
    } else {
        if (session->party_members_count == SESSION_PARTY_MEMBERS_MAX) {
            return false;
        }
        memmove(&session->party_members[index + 1],
                &session->party_members[index],
                (session->party_members_count - index) * sizeof(*session->party_members));
        session->party_members[index] = *member;
        session->party_members_count++;
    }

    session_event_t event = {.type = SESSION_EVENT_PARTY_MEMBER_UPSERT};
    event.data.party_member = *member;
    event_emit(session, &event);
    return true;
}

bool session_reduce_party_member_remove(session_t *session, const char *name) {
    if (session == NULL || name == NULL || name[0] == '\0' || strlen(name) >= SESSION_NAME_SIZE) {
        return false;
    }
    for (size_t i = 0; i < session->party_members_count; i++) {
        if (strcmp(session->party_members[i].name, name) != 0) {
            continue;
        }
        session_party_member_t removed = session->party_members[i];
        memmove(&session->party_members[i],
                &session->party_members[i + 1],
                (session->party_members_count - i - 1) * sizeof(*session->party_members));
        session->party_members_count--;
        session_event_t event = {.type = SESSION_EVENT_PARTY_MEMBER_REMOVE};
        event.data.party_member = removed;
        event_emit(session, &event);
        return true;
    }
    return false;
}

void session_reduce_server(session_t *session, const char *server) {
    if (session == NULL) {
        return;
    }

    copy_string(session->server, sizeof(session->server), server);
    session_event_t event = {.type = SESSION_EVENT_SERVER};
    copy_string(event.data.text, sizeof(event.data.text), session->server);
    event_emit(session, &event);
}

session_handle_t session_map_handle(const session_t *session, uint32_t id) {
    session_handle_t handle = {0};
    if (session == NULL || id == 0) {
        return handle;
    }

    bool found;
    size_t index = map_entity_index(session, id, &found);
    if (found) {
        handle.session_generation = session->session_generation;
        handle.collection_generation = session->map_generation;
        handle.object_generation = session->map_entities[index].generation;
        handle.id = id;
        handle.kind = SESSION_HANDLE_MAP_ENTITY;
    }
    return handle;
}

session_handle_t session_map_handle_at(const session_t *session, int x, int y) {
    session_handle_t handle = {0};
    if (session == NULL) {
        return handle;
    }

    const session_map_entity_t *selected = NULL;
    for (size_t i = 0; i < session->map_entities_count; i++) {
        const session_map_entity_t *entity = &session->map_entities[i];
        if (entity->x == x && entity->y == y && entity->depth == session->map.player_depth &&
            (selected == NULL || entity->sub_layer > selected->sub_layer)) {
            selected = entity;
        }
    }
    if (selected != NULL) {
        handle.session_generation = session->session_generation;
        handle.collection_generation = session->map_generation;
        handle.object_generation = selected->generation;
        handle.id = selected->id;
        handle.kind = SESSION_HANDLE_MAP_ENTITY;
    }
    return handle;
}

session_handle_t session_item_handle(const session_t *session, uint32_t id) {
    session_handle_t handle = {0};
    if (session == NULL || id == 0) {
        return handle;
    }

    bool found;
    size_t index = item_index(session, id, &found);
    if (found) {
        handle.session_generation = session->session_generation;
        handle.collection_generation = session->inventory_generation;
        handle.object_generation = session->items[index].generation;
        handle.id = id;
        handle.kind = SESSION_HANDLE_ITEM;
    }
    return handle;
}

bool session_handle_valid(const session_t *session, session_handle_t handle) {
    if (session == NULL || handle.id == 0 ||
        handle.session_generation != session->session_generation) {
        return false;
    }

    bool found;
    if (handle.kind == SESSION_HANDLE_MAP_ENTITY) {
        if (handle.collection_generation != session->map_generation) {
            return false;
        }
        size_t index = map_entity_index(session, handle.id, &found);
        return found && session->map_entities[index].generation == handle.object_generation;
    }

    if (handle.kind == SESSION_HANDLE_ITEM) {
        if (handle.collection_generation != session->inventory_generation) {
            return false;
        }
        size_t index = item_index(session, handle.id, &found);
        return found && session->items[index].generation == handle.object_generation;
    }

    return false;
}

static uint64_t action_capability(session_action_type_t type) {
    switch (type) {
        case SESSION_ACTION_MOVE:
        case SESSION_ACTION_MOVE_PATH:
        case SESSION_ACTION_STOP:
            return SESSION_CAP_MOVE;
        case SESSION_ACTION_TARGET:
            return SESSION_CAP_TARGET;
        case SESSION_ACTION_ATTACK:
            return SESSION_CAP_COMBAT;
        case SESSION_ACTION_CAST:
            return SESSION_CAP_CAST;
        case SESSION_ACTION_APPLY:
        case SESSION_ACTION_GET:
        case SESSION_ACTION_DROP:
            return SESSION_CAP_INVENTORY;
        case SESSION_ACTION_TALK:
        case SESSION_ACTION_REPLY:
            return SESSION_CAP_TALK;
        case SESSION_ACTION_SELECT_CHARACTER:
            return SESSION_CAP_SELECT_CHARACTER;
        case SESSION_ACTION_CONTROL:
            return SESSION_CAP_MOVE;
        case SESSION_ACTION_PLAYER_COMMAND:
            return SESSION_CAP_PLAYER_COMMAND;
    }

    return 0;
}

static session_action_result_t action_validate(const session_t *session,
                                               const session_action_t *action) {
    if (session == NULL || action == NULL || action->type < SESSION_ACTION_MOVE ||
        action->type > SESSION_ACTION_PLAYER_COMMAND) {
        return SESSION_ACTION_REJECTED_ARGUMENT;
    }

    uint64_t capability = action_capability(action->type);
    if ((session->capabilities & capability) == 0) {
        return SESSION_ACTION_REJECTED_CAPABILITY;
    }

    if (action->type == SESSION_ACTION_SELECT_CHARACTER) {
        if (session->lifecycle != SESSION_LIFECYCLE_CONNECTED) {
            return SESSION_ACTION_REJECTED_LIFECYCLE;
        }
    } else if (session->lifecycle != SESSION_LIFECYCLE_PLAYING) {
        return SESSION_ACTION_REJECTED_LIFECYCLE;
    }

    switch (action->type) {
        case SESSION_ACTION_MOVE:
            if (action->data.move.direction > 8) {
                return SESSION_ACTION_REJECTED_ARGUMENT;
            }
            break;
        case SESSION_ACTION_MOVE_PATH:
            if (action->data.point.x < 0 || action->data.point.y < 0 ||
                action->data.point.x > UINT8_MAX || action->data.point.y > UINT8_MAX ||
                !map_position_valid(&session->map, action->data.point.x, action->data.point.y)) {
                return SESSION_ACTION_REJECTED_ARGUMENT;
            }
            break;
        case SESSION_ACTION_TARGET:
            if (action->data.target.clear) {
                if (action->data.target.handle.id != 0 ||
                    action->data.target.handle.kind != SESSION_HANDLE_NONE) {
                    return SESSION_ACTION_REJECTED_ARGUMENT;
                }
                break;
            }
            if (action->data.target.x < 0 || action->data.target.y < 0 ||
                action->data.target.x > UINT8_MAX || action->data.target.y > UINT8_MAX ||
                !map_position_valid(&session->map, action->data.target.x, action->data.target.y)) {
                return SESSION_ACTION_REJECTED_ARGUMENT;
            }
            if (action->data.target.handle.id != 0 &&
                (action->data.target.handle.kind != SESSION_HANDLE_MAP_ENTITY ||
                 !session_handle_valid(session, action->data.target.handle))) {
                return SESSION_ACTION_REJECTED_STALE_HANDLE;
            }
            if (action->data.target.handle.id != 0) {
                bool found;
                size_t index = map_entity_index(session, action->data.target.handle.id, &found);
                if (!found || session->map_entities[index].x != action->data.target.x ||
                    session->map_entities[index].y != action->data.target.y ||
                    session->map_entities[index].depth != session->map.player_depth) {
                    return SESSION_ACTION_REJECTED_ARGUMENT;
                }
            }
            break;
        case SESSION_ACTION_CAST:
            if (action->data.cast.direction > 8) {
                return SESSION_ACTION_REJECTED_ARGUMENT;
            }
            if (action->data.cast.handle.kind != SESSION_HANDLE_ITEM ||
                !session_handle_valid(session, action->data.cast.handle)) {
                return SESSION_ACTION_REJECTED_STALE_HANDLE;
            }
            break;
        case SESSION_ACTION_APPLY:
            if (action->data.item.virtual_item) {
                if (action->data.item.handle.id != 0 ||
                    action->data.item.virtual_action < SESSION_VIRTUAL_ITEM_NONE ||
                    action->data.item.virtual_action > SESSION_VIRTUAL_ITEM_PREVIOUS) {
                    return SESSION_ACTION_REJECTED_ARGUMENT;
                }
                break;
            }
            if (action->data.item.handle.kind != SESSION_HANDLE_ITEM ||
                !session_handle_valid(session, action->data.item.handle)) {
                return SESSION_ACTION_REJECTED_STALE_HANDLE;
            }
            break;
        case SESSION_ACTION_GET:
        case SESSION_ACTION_DROP:
            if (action->data.item.handle.kind != SESSION_HANDLE_ITEM ||
                !session_handle_valid(session, action->data.item.handle)) {
                return SESSION_ACTION_REJECTED_STALE_HANDLE;
            }
            if (action->data.item.container_id == action->data.item.handle.id) {
                return SESSION_ACTION_REJECTED_ARGUMENT;
            }
            if (action->data.item.container_id == 0 ||
                action->data.item.container_id == session->player.id) {
                if (action->data.item.container.id != 0 ||
                    action->data.item.container.kind != SESSION_HANDLE_NONE) {
                    return SESSION_ACTION_REJECTED_ARGUMENT;
                }
            } else if (action->data.item.container.id != action->data.item.container_id ||
                       action->data.item.container.kind != SESSION_HANDLE_ITEM ||
                       !session_handle_valid(session, action->data.item.container)) {
                return SESSION_ACTION_REJECTED_STALE_HANDLE;
            }
            break;
        case SESSION_ACTION_TALK:
            if (action->data.handle.kind != SESSION_HANDLE_MAP_ENTITY ||
                !session_handle_valid(session, action->data.handle)) {
                return SESSION_ACTION_REJECTED_STALE_HANDLE;
            }
            break;
        case SESSION_ACTION_REPLY:
            if (!string_valid(action->data.reply.text, sizeof(action->data.reply.text), true)) {
                return SESSION_ACTION_REJECTED_ARGUMENT;
            }
            if (session->dialog.generation == 0 ||
                (session->dialog.title[0] == '\0' && session->dialog.text[0] == '\0') ||
                action->data.reply.dialog_generation != session->dialog.generation) {
                return SESSION_ACTION_REJECTED_STALE_HANDLE;
            }
            break;
        case SESSION_ACTION_SELECT_CHARACTER:
            if (!string_valid(action->data.text, sizeof(action->data.text), false)) {
                return SESSION_ACTION_REJECTED_ARGUMENT;
            }
            break;
        case SESSION_ACTION_PLAYER_COMMAND:
            if (!string_valid(action->data.text, sizeof(action->data.text), false)) {
                return SESSION_ACTION_REJECTED_ARGUMENT;
            }
            break;
        case SESSION_ACTION_STOP:
        case SESSION_ACTION_ATTACK:
        case SESSION_ACTION_CONTROL:
            break;
    }

    return SESSION_ACTION_ACCEPTED;
}

static void action_update_intent(session_t *session, const session_action_t *action) {
    switch (action->type) {
        case SESSION_ACTION_MOVE:
            session->intent.movement_direction =
                action->data.move.fire ? 0 : (int8_t)action->data.move.direction;
            session->intent.movement_held =
                !action->data.move.fire && action->data.move.direction != 0;
            session->intent.run = action->data.move.run;
            session->intent.fire = action->data.move.fire;
            break;
        case SESSION_ACTION_MOVE_PATH:
            session->intent.movement_direction = 0;
            session->intent.movement_held = false;
            break;
        case SESSION_ACTION_STOP:
            session->intent.movement_direction = 0;
            session->intent.movement_held = false;
            session->intent.run = false;
            session->intent.fire = false;
            break;
        case SESSION_ACTION_TARGET:
            session->intent.target = action->data.target.handle;
            break;
        case SESSION_ACTION_ATTACK:
            session->intent.combat = action->data.attack.enabled;
            session->intent.combat_force = action->data.attack.force;
            break;
        case SESSION_ACTION_CONTROL:
            session->intent.run = action->data.control.run;
            session->intent.fire = action->data.control.fire;
            break;
        case SESSION_ACTION_CAST:
        case SESSION_ACTION_APPLY:
        case SESSION_ACTION_GET:
        case SESSION_ACTION_DROP:
        case SESSION_ACTION_TALK:
        case SESSION_ACTION_REPLY:
        case SESSION_ACTION_SELECT_CHARACTER:
        case SESSION_ACTION_PLAYER_COMMAND:
            break;
    }
    intent_emit(session);
}

session_action_result_t session_action_dispatch(session_t *session,
                                                const session_action_t *action) {
    session_action_result_t result = action_validate(session, action);
    if (result != SESSION_ACTION_ACCEPTED) {
        return result;
    }

    if (session->sink == NULL || !session->sink(session->sink_context, action)) {
        return SESSION_ACTION_REJECTED_SINK;
    }

    action_update_intent(session, action);
    return SESSION_ACTION_ACCEPTED;
}

session_action_result_t session_action_enqueue(session_t *session, const session_action_t *action) {
    session_action_result_t result = action_validate(session, action);
    if (result != SESSION_ACTION_ACCEPTED) {
        return result;
    }
    if (session->actions_count == session->actions_capacity) {
        return SESSION_ACTION_REJECTED_QUEUE_FULL;
    }

    size_t slot = (session->actions_head + session->actions_count) % session->actions_capacity;
    session->actions[slot] = *action;
    session->actions_count++;
    session->intent.pending_actions = session->actions_count;
    intent_emit(session);
    return SESSION_ACTION_ACCEPTED;
}

size_t
session_actions_drain(session_t *session, session_action_result_t *results, size_t capacity) {
    if (session == NULL || (capacity > 0 && results == NULL)) {
        return 0;
    }

    size_t count = 0;
    while (count < capacity && session->actions_count > 0) {
        session_action_t action = session->actions[session->actions_head];
        session->actions_head = (session->actions_head + 1) % session->actions_capacity;
        session->actions_count--;
        session->intent.pending_actions = session->actions_count;
        results[count] = session_action_dispatch(session, &action);
        if (results[count] != SESSION_ACTION_ACCEPTED) {
            intent_emit(session);
        }
        count++;
    }
    return count;
}

void session_actions_clear(session_t *session) {
    if (session == NULL || session->actions_count == 0) {
        return;
    }

    session->actions_head = 0;
    session->actions_count = 0;
    session->intent.pending_actions = 0;
    intent_emit(session);
}

const char *session_action_result_string(session_action_result_t result) {
    switch (result) {
        case SESSION_ACTION_ACCEPTED:
            return "accepted";
        case SESSION_ACTION_REJECTED_ARGUMENT:
            return "invalid argument";
        case SESSION_ACTION_REJECTED_CAPABILITY:
            return "capability unavailable";
        case SESSION_ACTION_REJECTED_LIFECYCLE:
            return "invalid lifecycle";
        case SESSION_ACTION_REJECTED_STALE_HANDLE:
            return "stale handle";
        case SESSION_ACTION_REJECTED_QUEUE_FULL:
            return "action queue full";
        case SESSION_ACTION_REJECTED_SINK:
            return "command sink rejected action";
    }

    return "unknown result";
}
