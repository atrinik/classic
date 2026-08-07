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
 * Graphical-client adapter for the renderer-independent session core.
 */

#include <global.h>
#include <client_socket.h>
#include <session_client.h>
#include <toolkit/packet.h>

static session_t *client_session;

static void copy_text(char *destination, size_t size, const char *source) {
    HARD_ASSERT(destination != NULL);
    HARD_ASSERT(size > 0);
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    size_t length = strnlen(source, size - 1);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool text_fits(const char *text, size_t size, bool allow_empty) {
    if (text == NULL) {
        return false;
    }
    size_t length = strnlen(text, size);
    return length < size && (allow_empty || length > 0);
}

static void packet_send_player_command(const char *command) {
    packet_struct *packet = packet_new(SERVER_CMD_PLAYER_CMD, 256, 128);
    packet_writer_write_cstring(packet, command);
    socket_send_packet(packet);
}

static bool command_sink(void *context, const session_action_t *action) {
    (void)context;

    packet_struct *packet;
    switch (action->type) {
        case SESSION_ACTION_MOVE:
            if (action->data.move.fire) {
                packet = packet_new(SERVER_CMD_FIRE, 64, 64);
                packet_writer_write_uint8(packet, action->data.move.direction);
            } else {
                packet = packet_new(SERVER_CMD_MOVE, 8, 0);
                packet_writer_write_uint8(packet, action->data.move.direction);
                packet_writer_write_uint8(packet, action->data.move.run);
            }
            socket_send_packet(packet);
            return true;

        case SESSION_ACTION_MOVE_PATH:
            packet = packet_new(SERVER_CMD_MOVE_PATH, 8, 0);
            packet_writer_write_uint8(packet, (uint8_t)action->data.point.x);
            packet_writer_write_uint8(packet, (uint8_t)action->data.point.y);
            socket_send_packet(packet);
            return true;

        case SESSION_ACTION_STOP:
            packet = packet_new(SERVER_CMD_CLEAR, 0, 0);
            socket_send_packet(packet);
            return true;

        case SESSION_ACTION_TARGET:
            packet = packet_new(SERVER_CMD_TARGET, 16, 0);
            if (action->data.target.clear) {
                packet_writer_write_uint8(packet, CMD_TARGET_CLEAR);
            } else {
                if (action->data.target.x < 0 || action->data.target.x > UINT8_MAX ||
                    action->data.target.y < 0 || action->data.target.y > UINT8_MAX) {
                    packet_free(packet);
                    return false;
                }
                packet_writer_write_uint8(packet, CMD_TARGET_MAPXY);
                packet_writer_write_uint8(packet, (uint8_t)action->data.target.x);
                packet_writer_write_uint8(packet, (uint8_t)action->data.target.y);
                packet_writer_write_uint32(packet, action->data.target.handle.id);
            }
            socket_send_packet(packet);
            return true;

        case SESSION_ACTION_ATTACK:
            packet = packet_new(SERVER_CMD_COMBAT, 8, 0);
            packet_writer_write_uint8(packet, action->data.attack.enabled);
            packet_writer_write_uint8(packet, action->data.attack.force);
            socket_send_packet(packet);
            return true;

        case SESSION_ACTION_CAST:
            packet = packet_new(SERVER_CMD_FIRE, 64, 64);
            packet_writer_write_uint8(packet, action->data.cast.direction);
            packet_writer_write_uint32(packet, action->data.cast.handle.id);
            socket_send_packet(packet);
            return true;

        case SESSION_ACTION_APPLY:
            packet = packet_new(SERVER_CMD_ITEM_APPLY, 8, 0);
            packet_writer_write_uint32(packet, action->data.item.handle.id);
            if (action->data.item.virtual_item) {
                uint8_t virtual_action;
                switch (action->data.item.virtual_action) {
                    case SESSION_VIRTUAL_ITEM_NONE:
                        virtual_action = CMD_APPLY_ACTION_NONE;
                        break;
                    case SESSION_VIRTUAL_ITEM_NEXT:
                        virtual_action = CMD_APPLY_ACTION_BELOW_NEXT;
                        break;
                    case SESSION_VIRTUAL_ITEM_PREVIOUS:
                        virtual_action = CMD_APPLY_ACTION_BELOW_PREV;
                        break;
                    default:
                        packet_free(packet);
                        return false;
                }
                packet_writer_write_uint8(packet, virtual_action);
            }
            socket_send_packet(packet);
            return true;

        case SESSION_ACTION_GET:
        case SESSION_ACTION_DROP:
            packet = packet_new(SERVER_CMD_ITEM_MOVE, 32, 0);
            packet_writer_write_uint32(packet, action->data.item.container_id);
            packet_writer_write_uint32(packet, action->data.item.handle.id);
            packet_writer_write_uint32(packet, action->data.item.quantity);
            socket_send_packet(packet);
            return true;

        case SESSION_ACTION_TALK:
            packet_send_player_command("/talk 1 hello");
            return true;

        case SESSION_ACTION_REPLY: {
            char command[SESSION_TEXT_SIZE + 32];
            if (action->data.reply.reply == 0) {
                snprintf(command, sizeof(command), "/talk 1 %s", action->data.reply.text);
            } else {
                snprintf(command,
                         sizeof(command),
                         "/talk 1 reply %" PRIu32 " %s",
                         action->data.reply.reply,
                         action->data.reply.text);
            }
            packet_send_player_command(command);
            return true;
        }

        case SESSION_ACTION_SELECT_CHARACTER:
            packet = packet_new(SERVER_CMD_ACCOUNT, 64, 64);
            packet_writer_write_uint8(packet, CMD_ACCOUNT_LOGIN_CHAR);
            packet_writer_write_cstring(packet, action->data.text);
            socket_send_packet(packet);
            return true;

        case SESSION_ACTION_CONTROL:
            return true;

        case SESSION_ACTION_PLAYER_COMMAND:
            packet_send_player_command(action->data.text);
            return true;
    }

    return false;
}

void client_session_init(void) {
    if (client_session != NULL) {
        return;
    }

    client_session = session_create(NULL, command_sink, NULL);
    HARD_ASSERT(client_session != NULL);
}

void client_session_deinit(void) {
    session_destroy(client_session);
    client_session = NULL;
}

session_t *client_session_get(void) {
    if (client_session == NULL) {
        client_session_init();
    }
    return client_session;
}

void client_session_drain_actions(void) {
    session_action_result_t results[16];
    size_t count;
    do {
        count = session_actions_drain(client_session_get(),
                                      results,
                                      sizeof(results) / sizeof(results[0]));
        for (size_t i = 0; i < count; i++) {
            if (results[i] != SESSION_ACTION_ACCEPTED) {
                LOG(INFO,
                    "Rejected queued session action: %s",
                    session_action_result_string(results[i]));
            }
        }
    } while (count == sizeof(results) / sizeof(results[0]));
}

void client_session_connected(const char *server) {
    session_reduce_connect(client_session_get(), SESSION_CAP_SELECT_CHARACTER, server);
}

void client_session_disconnected(void) {
    if (client_session != NULL) {
        session_reduce_disconnect(client_session);
    }
}

void client_session_character_reset(void) {
    if (client_session != NULL) {
        session_reduce_character_reset(client_session);
    }
}

static session_player_t player_from_legacy(void) {
    session_player_t player = {
        .id = cpl.ob != NULL ? cpl.ob->tag : 0,
        .stats =
            {
                .strength = cpl.stats.Str,
                .dexterity = cpl.stats.Dex,
                .constitution = cpl.stats.Con,
                .intelligence = cpl.stats.Int,
                .power = cpl.stats.Pow,
                .wc = cpl.stats.wc,
                .ac = cpl.stats.ac,
                .level = cpl.stats.level,
                .hp = cpl.stats.hp,
                .max_hp = cpl.stats.maxhp,
                .sp = cpl.stats.sp,
                .max_sp = cpl.stats.maxsp,
                .experience = cpl.stats.exp,
                .food = cpl.stats.food,
                .damage = cpl.stats.dam,
                .speed = cpl.stats.speed,
                .weapon_speed = cpl.stats.weapon_speed,
                .flags = cpl.stats.flags,
                .ranged_damage = cpl.stats.ranged_dam,
                .ranged_wc = cpl.stats.ranged_wc,
                .ranged_weapon_speed = cpl.stats.ranged_ws,
                .hp_regeneration = cpl.gen_hp,
                .sp_regeneration = cpl.gen_sp,
            },
        .weight = cpl.real_weight,
        .weight_limit = cpl.weight_limit,
        .action_timer = cpl.action_timer,
        .gender = cpl.gender,
        .dm = cpl.dm != 0,
        .path_attuned = cpl.path_attuned,
        .path_repelled = cpl.path_repelled,
        .path_denied = cpl.path_denied,
    };
    _Static_assert(sizeof(player.stats.protections) >= sizeof(cpl.stats.protection),
                   "session protection capacity is too small");
    _Static_assert(sizeof(player.equipment) >= sizeof(cpl.equipment),
                   "session equipment capacity is too small");
    memcpy(player.stats.protections, cpl.stats.protection, sizeof(cpl.stats.protection));
    memcpy(player.equipment, cpl.equipment, sizeof(cpl.equipment));
    return player;
}

void client_session_playing(void) {
    session_player_t player = player_from_legacy();
    copy_text(player.name, sizeof(player.name), cpl.name);
    session_reduce_capabilities(client_session_get(),
                                SESSION_CAP_GAMEPLAY | SESSION_CAP_SELECT_CHARACTER);
    session_reduce_play(client_session_get(), &player);
}

void client_session_sync_player(void) {
    session_player_t player = player_from_legacy();
    session_player_t current = {0};
    if (session_player_view(client_session_get(), &current)) {
        player.weight = current.weight;
    }
    copy_text(player.name, sizeof(player.name), cpl.name);
    session_reduce_player(client_session_get(), &player);
}

void client_session_sync_target(void) {
    session_target_t target = {
        .code = (uint8_t)cpl.target_code,
        .level = cpl.target_level,
        .hp = (uint8_t)cpl.target_hp,
        .friend = cpl.target_is_friend != 0,
        .combat = cpl.combat != 0,
        .combat_force = cpl.combat_force != 0,
    };
    copy_text(target.name, sizeof(target.name), cpl.target_name);
    copy_text(target.color, sizeof(cpl.target_color), cpl.target_color);
    session_reduce_target(client_session_get(), &target);
}

void client_session_sync_map(bool reset, int x_offset, int y_offset, int depth_offset) {
    session_map_t map = {
        .width = map_get_width(),
        .height = map_get_height(),
        .player_x = MapData.posx,
        .player_y = MapData.posy,
        .player_depth = 0,
        .player_sub_layer = MapData.player_sub_layer,
    };
    copy_text(map.name,
              sizeof(map.name),
              MapData.name_new[0] != '\0' ? MapData.name_new : MapData.name);
    copy_text(map.path, sizeof(map.path), MapData.map_path);
    if (reset) {
        session_reduce_map_reset(client_session_get(), &map);
    } else if (x_offset != 0 || y_offset != 0 || depth_offset != 0) {
        session_reduce_map_scroll(client_session_get(), x_offset, y_offset, depth_offset, &map);
    } else {
        session_reduce_map(client_session_get(), &map);
    }
}

void client_session_sync_map_entity(uint32_t id,
                                    int x,
                                    int y,
                                    int depth,
                                    uint8_t sub_layer,
                                    uint16_t face,
                                    uint32_t flags,
                                    uint8_t hp,
                                    bool is_friend,
                                    const char *name) {
    if (id == 0) {
        return;
    }

    session_map_entity_t entity = {
        .id = id,
        .x = (int16_t)x,
        .y = (int16_t)y,
        .depth = (int8_t)depth,
        .sub_layer = sub_layer,
        .face = face,
        .flags = flags,
        .hp = hp,
        .friend = is_friend,
    };
    copy_text(entity.name, sizeof(entity.name), name);
    session_reduce_map_entity(client_session_get(), &entity);
}

void client_session_sync_map_cell(int x,
                                  int y,
                                  int depth,
                                  uint8_t layer,
                                  uint8_t sub_layer,
                                  uint16_t face,
                                  uint32_t flags,
                                  int16_t height,
                                  uint8_t light_level,
                                  bool light_known,
                                  bool fogged,
                                  bool fog_known,
                                  const char *name) {
    session_map_cell_t cell = {
        .x = (int16_t)x,
        .y = (int16_t)y,
        .depth = (int8_t)depth,
        .layer = layer,
        .sub_layer = sub_layer,
        .face = face,
        .flags = flags,
        .height = height,
        .light_level = light_level,
        .light_known = light_known,
        .fogged = fogged,
        .fog_known = fog_known,
    };
    copy_text(cell.name, sizeof(cell.name), name);
    session_reduce_map_cell(client_session_get(), &cell);
}

void client_session_remove_map_entity(uint32_t id) {
    session_reduce_map_entity_remove(client_session_get(), id);
}

void client_session_clear_map_cell(int x, int y, int depth, int sub_layer, bool hard) {
    if (hard) {
        session_reduce_map_cell_clear(client_session_get(), x, y, depth, sub_layer);
    } else {
        session_reduce_map_cell_soft_clear(client_session_get(), x, y, depth);
    }
}

void client_session_clear_map_layer(int x, int y, int depth, uint8_t layer, uint8_t sub_layer) {
    session_reduce_map_layer_clear(client_session_get(), x, y, depth, layer, sub_layer);
}

void client_session_clear_map_entities(int x, int y, int depth, int sub_layer) {
    session_reduce_map_entities_clear(client_session_get(), x, y, depth, sub_layer);
}

void client_session_inventory_begin(uint32_t container_id, bool clear, bool open) {
    session_reduce_inventory_begin(client_session_get(), container_id, clear, open);
}

static session_item_category_t item_category(const struct obj *item) {
    if (item->itype == TYPE_SKILL) {
        return SESSION_ITEM_SKILL;
    }
    if (item->itype == TYPE_SPELL) {
        return SESSION_ITEM_SPELL;
    }
    if (item->itype == TYPE_FORCE || item->itype == TYPE_POISONING) {
        return SESSION_ITEM_EFFECT;
    }
    return SESSION_ITEM_INVENTORY;
}

static session_item_t item_from_legacy(const struct obj *item) {
    session_item_t session_item = {0};
    session_handle_t handle = session_item_handle(client_session_get(), item->tag);
    session_item_view(client_session_get(), handle, &session_item);

    session_item_category_t category = item_category(item);
    if (session_item.category != category) {
        session_item.ability_level = 0;
        session_item.ability_experience = 0;
        session_item.ability_cost = 0;
        session_item.ability_path = 0;
        session_item.ability_flags = 0;
        session_item.effect_seconds = 0;
        session_item.description[0] = '\0';
    }

    session_item.id = item->tag;
    session_item.container_id = item->env != NULL ? item->env->tag : 0;
    session_item.quantity = item->nrof;
    session_item.weight = item->weight;
    session_item.flags = item->flags;
    session_item.face = item->face;
    session_item.type = item->itype;
    session_item.subtype = item->stype;
    session_item.quality = item->item_qua;
    session_item.condition = item->item_con;
    session_item.required_level = item->item_level;
    session_item.direction = item->direction;
    session_item.required_skill_id = item->item_skill_tag;
    session_item.category = category;
    copy_text(session_item.name, sizeof(session_item.name), item->s_name);
    return session_item;
}

void client_session_sync_item(const struct obj *item) {
    if (item == NULL || item->tag == 0) {
        return;
    }

    session_item_t session_item = item_from_legacy(item);
    session_reduce_item(client_session_get(), &session_item);
}

void client_session_sync_skill(const struct obj *item,
                               uint8_t level,
                               int64_t experience,
                               const char *description) {
    if (item == NULL || item->tag == 0) {
        return;
    }
    session_item_t session_item = item_from_legacy(item);
    session_item.category = SESSION_ITEM_SKILL;
    session_item.ability_level = level;
    session_item.ability_experience = experience;
    copy_text(session_item.description, sizeof(session_item.description), description);
    session_reduce_item(client_session_get(), &session_item);
}

void client_session_sync_spell(const struct obj *item,
                               uint16_t cost,
                               uint32_t path,
                               uint32_t flags,
                               const char *description) {
    if (item == NULL || item->tag == 0) {
        return;
    }
    session_item_t session_item = item_from_legacy(item);
    session_item.category = SESSION_ITEM_SPELL;
    session_item.ability_cost = cost;
    session_item.ability_path = path;
    session_item.ability_flags = flags;
    copy_text(session_item.description, sizeof(session_item.description), description);
    session_reduce_item(client_session_get(), &session_item);
}

void client_session_sync_effect(const struct obj *item, int32_t seconds, const char *description) {
    if (item == NULL || item->tag == 0) {
        return;
    }
    session_item_t session_item = item_from_legacy(item);
    session_item.category = SESSION_ITEM_EFFECT;
    session_item.effect_seconds = seconds;
    copy_text(session_item.description, sizeof(session_item.description), description);
    session_reduce_item(client_session_get(), &session_item);
}

void client_session_remove_item(uint32_t id) {
    session_reduce_item_remove(client_session_get(), id);
}

void client_session_inventory_end(uint32_t container_id) {
    session_reduce_inventory_end(client_session_get(), container_id);
}

session_action_result_t client_session_move(uint8_t direction, bool run, bool fire) {
    session_action_t action = {
        .type = SESSION_ACTION_MOVE,
        .data.move = {.direction = direction, .run = run, .fire = fire},
    };
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_move_path(int x, int y) {
    if (x < 0 || x > UINT8_MAX || y < 0 || y > UINT8_MAX) {
        return SESSION_ACTION_REJECTED_ARGUMENT;
    }
    session_action_t action = {
        .type = SESSION_ACTION_MOVE_PATH,
        .data.point = {.x = (int16_t)x, .y = (int16_t)y},
    };
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_stop(void) {
    session_action_t action = {.type = SESSION_ACTION_STOP};
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_target(uint32_t id) {
    session_action_t action = {.type = SESSION_ACTION_TARGET};
    if (id == 0) {
        action.data.target.clear = true;
    } else {
        action.data.target.handle = session_map_handle(client_session_get(), id);
        session_map_entity_t entity;
        if (!session_map_entity_view(client_session_get(), action.data.target.handle, &entity)) {
            return SESSION_ACTION_REJECTED_STALE_HANDLE;
        }
        action.data.target.x = entity.x;
        action.data.target.y = entity.y;
    }
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_target_at(int x, int y, uint32_t id) {
    if (!(x == -1 && y == -1) && (x < 0 || x > UINT8_MAX || y < 0 || y > UINT8_MAX)) {
        return SESSION_ACTION_REJECTED_ARGUMENT;
    }
    session_action_t action = {
        .type = SESSION_ACTION_TARGET,
        .data.target = {.x = (int16_t)x, .y = (int16_t)y},
    };
    if (x == -1 && y == -1) {
        action.data.target.clear = true;
    } else if (id != 0) {
        action.data.target.handle = session_map_handle(client_session_get(), id);
        if (action.data.target.handle.id == 0) {
            return SESSION_ACTION_REJECTED_STALE_HANDLE;
        }
    } else {
        action.data.target.handle = session_map_handle_at(client_session_get(), x, y);
    }
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_attack(bool enabled, bool force) {
    session_action_t action = {
        .type = SESSION_ACTION_ATTACK,
        .data.attack = {.enabled = enabled, .force = force},
    };
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_controls(bool run, bool fire) {
    session_action_t action = {
        .type = SESSION_ACTION_CONTROL,
        .data.control = {.run = run, .fire = fire},
    };
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_cast(uint32_t id, uint8_t direction) {
    session_action_t action = {
        .type = SESSION_ACTION_CAST,
        .data.cast =
            {
                .handle = session_item_handle(client_session_get(), id),
                .direction = direction,
            },
    };
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_apply(uint32_t id, uint8_t virtual_action) {
    session_action_t action = {
        .type = SESSION_ACTION_APPLY,
        .data.item =
            {
                .handle = session_item_handle(client_session_get(), id),
                .virtual_action = virtual_action,
                .virtual_item = id == 0,
            },
    };
    if (id == 0) {
        switch (virtual_action) {
            case CMD_APPLY_ACTION_NONE:
                action.data.item.virtual_action = SESSION_VIRTUAL_ITEM_NONE;
                break;
            case CMD_APPLY_ACTION_BELOW_NEXT:
                action.data.item.virtual_action = SESSION_VIRTUAL_ITEM_NEXT;
                break;
            case CMD_APPLY_ACTION_BELOW_PREV:
                action.data.item.virtual_action = SESSION_VIRTUAL_ITEM_PREVIOUS;
                break;
            default:
                return SESSION_ACTION_REJECTED_ARGUMENT;
        }
    }
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t
client_session_move_item(uint32_t destination, uint32_t id, uint32_t quantity, bool get) {
    session_player_t player = {0};
    if (!session_player_view(client_session_get(), &player)) {
        return SESSION_ACTION_REJECTED_LIFECYCLE;
    }
    session_handle_t container = {0};
    if (destination != 0 && destination != player.id) {
        container = session_item_handle(client_session_get(), destination);
    }
    session_action_t action = {
        .type = get ? SESSION_ACTION_GET : SESSION_ACTION_DROP,
        .data.item =
            {
                .handle = session_item_handle(client_session_get(), id),
                .container = container,
                .container_id = destination,
                .quantity = quantity,
            },
    };
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_talk(uint32_t id) {
    session_action_t action = {
        .type = SESSION_ACTION_TALK,
        .data.handle = session_map_handle(client_session_get(), id),
    };
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_reply(uint32_t reply, const char *text) {
    session_action_t action = {.type = SESSION_ACTION_REPLY};
    if (!text_fits(text, sizeof(action.data.reply.text), true)) {
        return SESSION_ACTION_REJECTED_ARGUMENT;
    }
    action.data.reply.reply = reply;
    session_document_t dialog;
    if (!session_dialog_view(client_session_get(), &dialog)) {
        return SESSION_ACTION_REJECTED_LIFECYCLE;
    }
    action.data.reply.dialog_generation = dialog.generation;
    copy_text(action.data.reply.text, sizeof(action.data.reply.text), text);
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_select_character(const char *name) {
    session_action_t action = {.type = SESSION_ACTION_SELECT_CHARACTER};
    if (!text_fits(name, sizeof(action.data.text), false)) {
        return SESSION_ACTION_REJECTED_ARGUMENT;
    }
    copy_text(action.data.text, sizeof(action.data.text), name);
    return session_action_dispatch(client_session_get(), &action);
}

session_action_result_t client_session_player_command(const char *command) {
    session_action_t action = {.type = SESSION_ACTION_PLAYER_COMMAND};
    if (!text_fits(command, sizeof(action.data.text), false)) {
        return SESSION_ACTION_REJECTED_ARGUMENT;
    }
    copy_text(action.data.text, sizeof(action.data.text), command);
    return session_action_dispatch(client_session_get(), &action);
}
