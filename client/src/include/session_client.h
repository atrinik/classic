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

#ifndef SESSION_CLIENT_H
#define SESSION_CLIENT_H

#include <session.h>

struct obj;

void client_session_init(void);
void client_session_deinit(void);
session_t *client_session_get(void);
void client_session_drain_actions(void);

void client_session_connected(const char *server);
void client_session_disconnected(void);
void client_session_character_reset(void);
void client_session_playing(void);
void client_session_sync_player(void);
void client_session_sync_target(void);
void client_session_sync_map(bool reset, int x_offset, int y_offset, int depth_offset);
void client_session_sync_map_entity(uint32_t id,
                                    int x,
                                    int y,
                                    int depth,
                                    uint8_t sub_layer,
                                    uint16_t face,
                                    uint32_t flags,
                                    uint8_t hp,
                                    bool is_friend,
                                    const char *name);
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
                                  const char *name);
void client_session_remove_map_entity(uint32_t id);
void client_session_clear_map_cell(int x, int y, int depth, int sub_layer, bool hard);
void client_session_clear_map_layer(int x, int y, int depth, uint8_t layer, uint8_t sub_layer);
void client_session_clear_map_entities(int x, int y, int depth, int sub_layer);
void client_session_inventory_begin(uint32_t container_id, bool clear, bool open);
void client_session_sync_item(const struct obj *item);
void client_session_sync_skill(const struct obj *item,
                               uint8_t level,
                               int64_t experience,
                               const char *description);
void client_session_sync_spell(const struct obj *item,
                               uint16_t cost,
                               uint32_t path,
                               uint32_t flags,
                               const char *description);
void client_session_sync_effect(const struct obj *item, int32_t seconds, const char *description);
void client_session_remove_item(uint32_t id);
void client_session_inventory_end(uint32_t container_id);

session_action_result_t client_session_move(uint8_t direction, bool run, bool fire);
session_action_result_t client_session_move_path(int x, int y);
session_action_result_t client_session_stop(void);
session_action_result_t client_session_target(uint32_t id);
session_action_result_t client_session_target_at(int x, int y, uint32_t id);
session_action_result_t client_session_attack(bool enabled, bool force);
session_action_result_t client_session_controls(bool run, bool fire);
session_action_result_t client_session_cast(uint32_t id, uint8_t direction);
session_action_result_t client_session_apply(uint32_t id, uint8_t virtual_action);
session_action_result_t
client_session_move_item(uint32_t destination, uint32_t id, uint32_t quantity, bool get);
session_action_result_t client_session_talk(uint32_t id);
session_action_result_t client_session_reply(uint32_t reply, const char *text);
session_action_result_t client_session_select_character(const char *name);
session_action_result_t client_session_player_command(const char *command);

#endif
