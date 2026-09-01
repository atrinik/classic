/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Handles commands received by the server. This does not necessarily
 * handle all the commands - some might be in other files.
 */

#include <animations.h>
#include <asset.h>
#include <book.h>
#include <client.h>
#include <commands.h>
#include <effects.h>
#include <image.h>
#include <interface.h>
#include <item.h>
#include <keybind.h>
#include <lighting.h>
#include <main.h>
#include <map.h>
#include <menu.h>
#include <notification.h>
#include <player.h>
#include <player_status.h>
#include <popup.h>
#include <region_map.h>
#include <rich_presence.h>
#include <server_files.h>
#include <settings.h>
#include <sound.h>
#include <sprite.h>
#include <text.h>
#include <toolkit/memory.h>
#include <toolkit/socket.h>
#include <toolkit/toolkit.h>
#include <video.h>
#include <client_socket.h>
#include <packet_payload.h>
#include <item_packet.h>
#include <join_credentials.h>
#include <toolkit/map_protocol.h>
#include <toolkit/packet.h>
#include <toolkit/string.h>
#include <textwin.h>
#include <widget.h>
#include <zlib.h>

static_assert(MAP2_PROTOCOL_METADATA_LONG_MAX == HUGE_BUF - 1,
              "MAP long metadata bound must match its client destination");
static_assert(MAP2_PROTOCOL_METADATA_SHORT_MAX == MAX_BUF - 1,
              "MAP short metadata bound must match its client destination");

/** @copydoc socket_command_struct::handle_func */
void socket_command_book(uint8_t *data, size_t len, size_t pos) {
    if (!book_load((char *)data + pos, len)) {
        if (!client_command_retry_current()) {
            LOG(ERROR, "Could not retain BOOK command for GPU recovery");
        }
        return;
    }
    sound_play_effect("book.ogg", 100);
}
/** @copydoc socket_command_struct::handle_func */
void socket_command_setup(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    uint8_t type;
    uint8_t asset_capabilities = 0;
    bool asset_capabilities_present = false;

    while (packet_reader_error(&reader) == PACKET_ERROR_NONE && pos < len) {
        type = packet_reader_read_uint8(&reader);

        if (type == CMD_SETUP_SOUND) {
            packet_reader_read_uint8(&reader);
        } else if (type == CMD_SETUP_MAPSIZE) {
            int x, y;

            x = packet_reader_read_uint8(&reader);
            y = packet_reader_read_uint8(&reader);

            if (x < MAP_WIRE_SIZE_MIN || x > MAP_WIRE_SIZE_MAX || y < MAP_WIRE_SIZE_MIN ||
                y > MAP_WIRE_SIZE_MAX) {
                packet_reader_set_error(&reader, PACKET_ERROR_LIMIT_EXCEEDED);
                break;
            }
            setting_set_int(OPT_CAT_MAP, OPT_MAP_WIDTH, MAP_WIRE_TO_LOOK_SIZE(x));
            setting_set_int(OPT_CAT_MAP, OPT_MAP_HEIGHT, MAP_WIRE_TO_LOOK_SIZE(y));
        } else if (type == CMD_SETUP_DATA_URL) {
            packet_reader_read_string(&reader, cpl.http_url, sizeof(cpl.http_url));
        } else if (type == CMD_SETUP_ASSET_TRANSPORT) {
            asset_capabilities = packet_reader_read_uint8(&reader);
            asset_capabilities_present = true;
        } else if (type == CMD_SETUP_CONNECTION_MODE) {
            packet_reader_read_uint8(&reader);
        } else if (type == CMD_SETUP_JOIN_PASSWORD) {
            bool accepted = packet_reader_read_uint8(&reader) != 0;
            client_attempt_secrets_clear(
                selected_server != NULL ? &selected_server->join_password : NULL,
                &clioption_settings.join_password,
                selected_server != NULL ? &selected_server->rendezvous_invite : NULL);
            if (!accepted) {
                draw_info(COLOR_RED, "The server rejected the join password.");
                cpl.state = ST_START;
                return;
            }
        } else {
            packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
        }
    }

    if (!packet_reader_finish(&reader)) {
        return;
    }
    if (asset_capabilities_present) {
        cpl.asset_transport = (asset_capabilities & ASSET_TRANSPORT_CAP_GENERIC) != 0;
        asset_requests_set_capabilities(asset_capabilities);
    }

    if (cpl.state != ST_PLAY) {
        cpl.state = ST_REQUEST_FILES_LISTING;
    }
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_anim(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    if (pos > len || len - pos < 4) {
        LOG(ERROR, "Ignoring truncated animation packet");
        return;
    }

    uint16_t anim_id = packet_reader_read_uint16(&reader);
    uint8_t flags = packet_reader_read_uint8(&reader);
    uint8_t facings = packet_reader_read_uint8(&reader);

    if (anim_id >= animations_num || animations == NULL) {
        LOG(ERROR,
            "Ignoring invalid animation ID %u (count: %" PRIu64 ")",
            anim_id,
            (uint64_t)animations_num);
        return;
    }

    size_t num_animations = (len - pos) / 2;
    if ((len - pos) % 2 != 0 || num_animations == 0 || facings == 0 ||
        num_animations % facings != 0) {
        LOG(ERROR,
            "Ignoring malformed animation %u (%" PRIu64 " faces, %u facings)",
            anim_id,
            (uint64_t)num_animations,
            facings);
        return;
    }

    Animations *animation = &animations[anim_id];
    free(animation->faces);
    animation->faces = xmallocarray(num_animations, sizeof(*animation->faces));
    animation->flags = flags;
    animation->facings = facings;
    animation->num_animations = num_animations;
    animation->frame = num_animations / facings;

    for (size_t i = 0; i < num_animations; i++) {
        uint16_t face = packet_reader_read_uint16(&reader) & FACE_ID_MASK;
        if (!image_face_valid(face)) {
            LOG(ERROR, "Animation %u contains invalid face ID %u", anim_id, face);
            face = 0;
        }

        animation->faces[i] = face;
        image_prefetch_face(face);
    }

    animation->loaded = 1;
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_drawinfo(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    uint8_t type;
    char color[COLOR_BUF], *str;
    StringBuffer *sb;

    type = packet_reader_read_uint8(&reader);
    packet_reader_read_string(&reader, color, sizeof(color));

    sb = stringbuffer_new();
    packet_reader_read_stringbuffer(&reader, sb);
    str = stringbuffer_finish(sb);

    draw_info_tab(type, color, str);

    free(str);
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_target(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    cpl.target_code = packet_reader_read_uint8(&reader);
    packet_reader_read_string(&reader, cpl.target_color, sizeof(cpl.target_color));
    packet_reader_read_string(&reader, cpl.target_name, sizeof(cpl.target_name));
    cpl.target_level = packet_reader_read_uint8(&reader);
    cpl.combat = packet_reader_read_uint8(&reader);
    cpl.combat_force = packet_reader_read_uint8(&reader);
    WIDGET_REDRAW_ALL(TARGET_ID);

    map_redraw_request(MAP_REDRAW_REASON_UI);
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_stats(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    uint8_t type;
    int temp;

    while (packet_reader_error(&reader) == PACKET_ERROR_NONE && pos < len) {
        type = packet_reader_read_uint8(&reader);

        if (type >= CS_STAT_EQUIP_START && type <= CS_STAT_EQUIP_END) {
            cpl.equipment[type - CS_STAT_EQUIP_START] = packet_reader_read_uint32(&reader);
            WIDGET_REDRAW_ALL(PDOLL_ID);
        } else if (type >= CS_STAT_PROT_START && type <= CS_STAT_PROT_END) {
            cpl.stats.protection[type - CS_STAT_PROT_START] = packet_reader_read_int8(&reader);
            WIDGET_REDRAW_ALL(PROTECTIONS_ID);
        } else {
            switch (type) {
                case CS_STAT_TARGET_HP:
                    cpl.target_hp = packet_reader_read_uint8(&reader);
                    WIDGET_REDRAW_ALL(TARGET_ID);
                    break;

                case CS_STAT_REG_HP:
                    cpl.gen_hp = packet_reader_read_uint16(&reader) / 10.0f;
                    widget_redraw_type_id(STAT_ID, "health");
                    break;

                case CS_STAT_REG_MANA:
                    cpl.gen_sp = packet_reader_read_uint16(&reader) / 10.0f;
                    widget_redraw_type_id(STAT_ID, "mana");
                    break;

                case CS_STAT_HP:
                    temp = packet_reader_read_int32(&reader);

                    if (temp < cpl.stats.hp && cpl.stats.food) {
                        cpl.warn_hp = 1;

                        if (cpl.stats.maxhp / 12 <= cpl.stats.hp - temp) {
                            cpl.warn_hp = 2;
                        }
                    }

                    cpl.stats.hp = temp;
                    widget_redraw_type_id(STAT_ID, "health");
                    break;

                case CS_STAT_MAXHP:
                    cpl.stats.maxhp = packet_reader_read_int32(&reader);
                    widget_redraw_type_id(STAT_ID, "health");
                    break;

                case CS_STAT_SP:
                    cpl.stats.sp = packet_reader_read_int16(&reader);
                    widget_redraw_type_id(STAT_ID, "mana");
                    break;

                case CS_STAT_MAXSP:
                    cpl.stats.maxsp = packet_reader_read_int16(&reader);
                    widget_redraw_type_id(STAT_ID, "mana");
                    break;

                case CS_STAT_STR:
                case CS_STAT_INT:
                case CS_STAT_POW:
                case CS_STAT_DEX:
                case CS_STAT_CON: {
                    int8_t *stat_curr;
                    uint8_t stat_new;

                    stat_curr = &(cpl.stats.Str) + (sizeof(cpl.stats.Str) * (type - CS_STAT_STR));
                    stat_new = packet_reader_read_uint8(&reader);

                    if (*stat_curr != -1) {
                        if (stat_new > *stat_curr) {
                            cpl.warn_statup = 1;
                        } else if (stat_new < *stat_curr) {
                            cpl.warn_statdown = 1;
                        }
                    }

                    *stat_curr = stat_new;
                    WIDGET_REDRAW_ALL(PDOLL_ID);
                    break;
                }

                case CS_STAT_PATH_ATTUNED:
                    cpl.path_attuned = packet_reader_read_uint32(&reader);
                    WIDGET_REDRAW_ALL(SPELLS_ID);
                    break;

                case CS_STAT_PATH_REPELLED:
                    cpl.path_repelled = packet_reader_read_uint32(&reader);
                    WIDGET_REDRAW_ALL(SPELLS_ID);
                    break;

                case CS_STAT_PATH_DENIED:
                    cpl.path_denied = packet_reader_read_uint32(&reader);
                    WIDGET_REDRAW_ALL(SPELLS_ID);
                    break;

                case CS_STAT_EXP:
                    cpl.stats.exp = packet_reader_read_uint64(&reader);
                    telemetry_exp_update(cpl.stats.exp);
                    widget_redraw_type_id(STAT_ID, "exp");
                    break;

                case CS_STAT_LEVEL:
                    cpl.stats.level = packet_reader_read_uint8(&reader);
                    WIDGET_REDRAW_ALL(PLAYER_INFO_ID);
                    break;

                case CS_STAT_WC:
                    cpl.stats.wc = packet_reader_read_uint16(&reader);
                    WIDGET_REDRAW_ALL(PDOLL_ID);
                    break;

                case CS_STAT_AC:
                    cpl.stats.ac = packet_reader_read_uint16(&reader);
                    WIDGET_REDRAW_ALL(PDOLL_ID);
                    break;

                case CS_STAT_DAM:
                    cpl.stats.dam = packet_reader_read_uint16(&reader);
                    WIDGET_REDRAW_ALL(PDOLL_ID);
                    break;

                case CS_STAT_SPEED:
                    cpl.stats.speed = packet_reader_read_double(&reader);
                    WIDGET_REDRAW_ALL(PDOLL_ID);
                    break;

                case CS_STAT_FOOD:
                    cpl.stats.food = packet_reader_read_uint16(&reader);
                    widget_redraw_type_id(STAT_ID, "food");
                    break;

                case CS_STAT_WEAPON_SPEED:
                    cpl.stats.weapon_speed = packet_reader_read_double(&reader);
                    WIDGET_REDRAW_ALL(PDOLL_ID);
                    break;

                case CS_STAT_FLAGS:
                    cpl.stats.flags = packet_reader_read_uint16(&reader);
                    break;

                case CS_STAT_WEIGHT_LIM:
                    cpl.weight_limit = packet_reader_read_uint32(&reader) / 1000.0;
                    break;

                case CS_STAT_ACTION_TIME:
                    cpl.action_timer = packet_reader_read_float(&reader);
                    WIDGET_REDRAW_ALL(PLAYER_INFO_ID);
                    break;

                case CS_STAT_GENDER:
                    cpl.gender = packet_reader_read_uint8(&reader);
                    WIDGET_REDRAW_ALL(PDOLL_ID);
                    break;

                case CS_STAT_RANGED_DAM:
                    cpl.stats.ranged_dam = packet_reader_read_uint16(&reader);
                    WIDGET_REDRAW_ALL(PDOLL_ID);
                    break;

                case CS_STAT_RANGED_WC:
                    cpl.stats.ranged_wc = packet_reader_read_uint16(&reader);
                    WIDGET_REDRAW_ALL(PDOLL_ID);
                    break;

                case CS_STAT_RANGED_WS:
                    cpl.stats.ranged_ws = packet_reader_read_float(&reader);
                    WIDGET_REDRAW_ALL(PDOLL_ID);
                    break;

                default:
                    packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
                    break;
            }
        }
    }

    (void)packet_reader_finish(&reader);
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_player(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    int tag, weight;

    tag = packet_reader_read_uint32(&reader);
    weight = packet_reader_read_uint32(&reader);
    uint32_t raw_face = packet_reader_read_uint32(&reader);
    uint16_t face = raw_face & FACE_ID_MASK;
    if (!image_face_valid(face)) {
        LOG(ERROR, "Player %d received invalid face ID %" PRIu32, tag, raw_face);
        face = 0;
    }

    image_request_face(face);
    packet_reader_read_string(&reader, cpl.name, sizeof(cpl.name));

    new_player(tag, weight, face);
    map_redraw_request(MAP_REDRAW_REASON_UI);

    cur_widget[INPUT_ID]->show = 0;

    if (cur_widget[PARTY_ID]->show) {
        send_command("/party list");
    }

    rich_presence_session_start();
    cpl.state = ST_PLAY;
}

static void command_item_apply(const item_packet_update_t *update, uint32_t flags, object *tmp) {
    object parsed = update->item;
    bool force_anim = false;

    if (flags & UPD_FACE) {
        uint16_t raw_face = parsed.face;
        uint16_t face = raw_face & FACE_ID_MASK;
        if (!image_face_valid(face)) {
            LOG(ERROR,
                "Object %" PRIu32 " received invalid face ID %u "
                "(animation: %u, direction: %u)",
                parsed.tag,
                raw_face,
                parsed.animation_id,
                parsed.direction);
            face = 0;
        }
        parsed.face = face;
    }

    if (flags & UPD_ANIM) {
        uint16_t animation_id = parsed.animation_id;
        if (animation_id >= animations_num) {
            LOG(ERROR,
                "Object %" PRIu32 " received invalid animation ID %u "
                "(face: %u, direction: %u)",
                parsed.tag,
                animation_id,
                parsed.face,
                parsed.direction);
            animation_id = 0;
        }
        if (tmp->animation_id != animation_id) {
            force_anim = true;
            parsed.anim_state = 0;
        }
        parsed.animation_id = animation_id;
    }

    if (flags & UPD_ANIMSPEED) {
        if (tmp->anim_speed == 0 && parsed.anim_speed != 0) {
            force_anim = true;
        }
    }

    item_packet_update_t applied = *update;
    applied.item = parsed;
    item_packet_apply_update(&applied, tmp);
    if (flags & UPD_FACE) {
        image_request_face(tmp->face);
    }

    if (update->extra_type == ITEM_PACKET_EXTRA_SPELL) {
        spells_update(tmp,
                      update->spell_cost,
                      update->spell_path,
                      update->spell_flags,
                      update->extra_message);
    } else if (update->extra_type == ITEM_PACKET_EXTRA_SKILL) {
        skills_update(tmp, update->skill_level, update->skill_exp, update->extra_message);
    }

    if (tmp->itype == TYPE_REGION_MAP) {
        region_map_fow_update(MapData.region_map);
        minimap_redraw_flag = 1;
    }

    if (force_anim) {
        tmp->last_anim = tmp->anim_speed;
        object_animate(tmp);
    }

    object_redraw(tmp);
}

bool command_item_update(packet_reader_t *reader, uint32_t flags, object *tmp) {
    item_packet_update_t update;
    if (!item_packet_parse_update(reader, flags, tmp, &update)) {
        return false;
    }

    command_item_apply(&update, flags, tmp);
    return true;
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_item(uint8_t *data, size_t len, size_t pos) {
    if (!item_packet_validate_command(data, len, pos)) {
        return;
    }

    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    bool delete_env = packet_reader_read_uint8(&reader) == 1;
    object *delete_target = NULL;
    if (delete_env) {
        tag_t loc_delete = packet_reader_read_uint32(&reader);
        delete_target = object_find(loc_delete);
        if (delete_target == NULL) {
            packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
            return;
        }

        if (pos == len) {
            object_remove_inventory(delete_target);
            return;
        }
    }

    tag_t loc = packet_reader_read_uint32(&reader);
    object *env = object_find(loc);
    if (env == NULL) {
        LOG(ERROR, "Server sent invalid location: %" PRIu32, loc);
        packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
        return;
    }

    uint8_t bflag = packet_reader_read_uint8(&reader);

    if (delete_target != NULL) {
        object_remove_inventory(delete_target);
    }
    if (env != cpl.below && env != cpl.ob) {
        cpl.sack = env;
    }

    while (packet_reader_error(&reader) == PACKET_ERROR_NONE && pos < len) {
        size_t iteration_start = pos;
        tag_t tag = packet_reader_read_uint32(&reader);
        uint8_t apply_action = CMD_APPLY_ACTION_NORMAL;

        object *tmp = NULL;
        if (tag != 0) {
            tmp = object_find(tag);
        } else {
            apply_action = packet_reader_read_uint8(&reader);
        }

        object base = {0};
        if (tmp != NULL && tmp->env == env && !delete_env) {
            base = *tmp;
        } else {
            base.tag = tag;
            base.apply_action = apply_action;
        }

        uint32_t flags = UPD_FLAGS | UPD_WEIGHT | UPD_FACE | UPD_DIRECTION | UPD_NAME | UPD_ANIM |
                         UPD_ANIMSPEED | UPD_NROF | UPD_GLOW;
        if (loc > 0) {
            flags |= UPD_TYPE | UPD_EXTRA;
        }

        item_packet_update_t update;
        if (!item_packet_parse_update(&reader, flags, &base, &update)) {
            return;
        }

        if (tmp != NULL && tmp->env != env) {
            object_remove(tmp);
            tmp = NULL;
        }

        if (tmp == NULL || delete_env) {
            object *old_tmp = tmp;
            tmp = object_create(env, tag, bflag);
            tmp->apply_action = apply_action;

            if (old_tmp != NULL) {
                if (old_tmp == cpl.sack) {
                    cpl.sack = tmp;
                }

                object_transfer_inventory(old_tmp, tmp);
                object_remove(old_tmp);
            }
        }

        command_item_apply(&update, flags, tmp);
        if (pos == iteration_start) {
            packet_reader_set_error(&reader, PACKET_ERROR_INVALID_ENCODING);
            return;
        }
    }

    (void)packet_reader_finish(&reader);
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_item_update(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t validate_reader;
    packet_reader_init_at(&validate_reader, data, len, pos);
    uint32_t validate_flags = packet_reader_read_uint16(&validate_reader);
    tag_t validate_tag = packet_reader_read_uint32(&validate_reader);
    object *validate_tmp = object_find(validate_tag);
    item_packet_update_t validate_update;
    if (validate_tmp == NULL) {
        packet_reader_set_error(&validate_reader, PACKET_ERROR_UNSUPPORTED);
        return;
    }
    if (!item_packet_parse_update(&validate_reader,
                                  validate_flags,
                                  validate_tmp,
                                  &validate_update) ||
        !packet_reader_finish(&validate_reader)) {
        return;
    }

    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    uint32_t flags, tag;
    object *tmp;

    flags = packet_reader_read_uint16(&reader);
    tag = packet_reader_read_uint32(&reader);

    tmp = object_find(tag);

    if (!tmp) {
        return;
    }

    command_item_update(&reader, flags, tmp);
    (void)packet_reader_finish(&reader);
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_item_delete(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    tag_t tag;

    if (packet_reader_remaining(&reader) % sizeof(tag_t) != 0) {
        packet_reader_set_error(&reader, PACKET_ERROR_TRUNCATED);
        return;
    }

    while (packet_reader_error(&reader) == PACKET_ERROR_NONE && pos < len) {
        tag = packet_reader_read_uint32(&reader);
        delete_object(tag);
    }
    (void)packet_reader_finish(&reader);
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_player_status(uint8_t *data, size_t len, size_t pos) {
    if (!player_status_parse_command(&player_status_model, data, len, pos)) {
        return;
    }

    for (player_status_t *status = player_status_model.head; status != NULL;
         status = status->next) {
        image_request_face(status->face);
    }
    WIDGET_REDRAW_ALL(ACTIVE_EFFECTS_ID);
}

/**
 * Plays the footstep sounds when moving on the map.
 */
static void map_play_footstep(void) {
    static int step = 0;
    static uint32_t tick = 0;

    if (LastTick - tick > 125) {
        step++;

        if (step % 2) {
            sound_play_effect("step1.ogg", 100);
        } else {
            step = 0;
            sound_play_effect("step2.ogg", 100);
        }

        tick = LastTick;
    }
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_mapstats(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    uint8_t type;
    char buf[HUGE_BUF];

    while (packet_reader_error(&reader) == PACKET_ERROR_NONE && pos < len) {
        /* Get the type of this command... */
        type = packet_reader_read_uint8(&reader);

        if (type == CMD_MAPSTATS_NAME) {
            /* Change map name. */
            packet_reader_read_string(&reader, buf, sizeof(buf));
            update_map_name(buf);
        } else if (type == CMD_MAPSTATS_MUSIC) {
            /* Change map music. */
            packet_reader_read_string(&reader, buf, sizeof(buf));
            update_map_bg_music(buf);
        } else if (type == CMD_MAPSTATS_WEATHER) {
            /* Change map weather. */
            packet_reader_read_string(&reader, buf, sizeof(buf));
            update_map_weather(buf);
        } else if (type == CMD_MAPSTATS_TEXT_ANIM) {
            packet_reader_read_string(&reader, msg_anim.color, sizeof(msg_anim.color));
            packet_reader_read_string(&reader, msg_anim.message, sizeof(msg_anim.message));
            msg_anim.tick = LastTick;
        } else if (type == CMD_MAPSTATS_TIME) {
            uint64_t game_seconds = packet_reader_read_uint64(&reader);
            uint32_t millis_per_game_minute = packet_reader_read_uint32(&reader);
            telemetry_game_time_sync(game_seconds, millis_per_game_minute);
            WIDGET_REDRAW_ALL(GAME_TIME_ID);
        } else {
            packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
        }
    }
    (void)packet_reader_finish(&reader);
}

static bool map_continuation_visible_change;
static bool map_continuation_region_fow_update;
static uint64_t map_publication_generation;

typedef struct map_pending_packet {
    uint8_t *data;
    size_t len;
    size_t pos;
    struct map_pending_packet *next;
} map_pending_packet_t;

/** Validated continuation envelopes retained until their complete generation arrives. */
static struct {
    map_protocol_continuation_state_t continuation;
    map_pending_packet_t *head;
    map_pending_packet_t *tail;
    bool replaying;
} map_pending_batch;

/** Observable effects that must publish with the same generation as its map cells. */
static struct {
    bool active;
    bool rich_presence;
    bool music;
    bool weather;
    bool footstep;
    char bg_music[HUGE_BUF];
    char weather_name[MAX_BUF];
} map_pending_effects;

static void map_pending_batch_release(map_pending_packet_t *packet) {
    while (packet != NULL) {
        map_pending_packet_t *next = packet->next;
        free(packet->data);
        free(packet);
        packet = next;
    }
}

static void map_pending_batch_reset(void) {
    map_pending_batch_release(map_pending_batch.head);
    map_pending_batch.head = NULL;
    map_pending_batch.tail = NULL;
    map_protocol_continuation_reset(&map_pending_batch.continuation);
}

static void map_pending_batch_append(uint8_t *data, size_t len, size_t pos) {
    map_pending_packet_t *packet = xcalloc(1, sizeof(*packet));
    packet->data = xmalloc(len);
    memcpy(packet->data, data, len);
    packet->len = len;
    packet->pos = pos;
    if (map_pending_batch.tail == NULL) {
        map_pending_batch.head = packet;
    } else {
        map_pending_batch.tail->next = packet;
    }
    map_pending_batch.tail = packet;
}

static void map_pending_effects_begin(void) {
    memset(&map_pending_effects, 0, sizeof(map_pending_effects));
    map_pending_effects.active = true;
}

static void map_pending_effects_abort(void) {
    memset(&map_pending_effects, 0, sizeof(map_pending_effects));
}

static void map_pending_effects_commit(void) {
    if (!map_pending_effects.active) {
        return;
    }
    bool rich_presence = map_pending_effects.rich_presence;
    bool music = map_pending_effects.music;
    bool weather = map_pending_effects.weather;
    bool footstep = map_pending_effects.footstep;
    char bg_music[HUGE_BUF];
    char weather_name[MAX_BUF];
    snprintf(VS(bg_music), "%s", map_pending_effects.bg_music);
    snprintf(VS(weather_name), "%s", map_pending_effects.weather_name);
    map_pending_effects_abort();

    if (rich_presence) {
        rich_presence_zone_changed();
    }
    if (music) {
        update_map_bg_music(bg_music);
    }
    if (weather) {
        update_map_weather(weather_name);
    }
    if (footstep) {
        map_play_footstep();
    }
}

void socket_command_map_abort_pending(void) {
    map_pending_batch_reset();
    map_light_keyframe_transaction_abort();
    map_state_transaction_abort();
    map_pending_effects_abort();
    map_continuation_visible_change = false;
    map_continuation_region_fow_update = false;
}

static void socket_command_map_abort_timed_light(void) {
    socket_command_map_abort_pending();
}

static bool socket_command_map_movement_delta(int mapstat,
                                              int xpos,
                                              int ypos,
                                              int *dx,
                                              int *dy) {
    *dx = xpos - MapData.posx;
    *dy = ypos - MapData.posy;
    return mapstat == MAP_UPDATE_CMD_SAME && (*dx != 0 || *dy != 0);
}

static void socket_command_map_descriptor_absent(int mapstat, bool transaction_pending) {
    if (mapstat == MAP_UPDATE_CMD_SAME || mapstat == MAP_UPDATE_CMD_PARTIAL ||
        transaction_pending) {
        return;
    }
    MapData.light_keyframe_generation = 0;
    MapData.light_keyframe_start_seconds = 0;
    MapData.light_keyframe_end_seconds = 0;
    MapData.light_keyframe_flags = 0;
    MapData.light_keyframe_valid = false;
}

#ifdef ATRINIK_WIDGET_TESTS
bool socket_command_map_timed_light_same_test(void) {
    uint64_t saved_generation = MapData.light_keyframe_generation;
    uint64_t saved_start = MapData.light_keyframe_start_seconds;
    uint64_t saved_end = MapData.light_keyframe_end_seconds;
    uint8_t saved_flags = MapData.light_keyframe_flags;
    bool saved_valid = MapData.light_keyframe_valid;

    MapData.light_keyframe_generation = 7;
    MapData.light_keyframe_start_seconds = 120;
    MapData.light_keyframe_end_seconds = 180;
    MapData.light_keyframe_flags = MAP2_LIGHT_KEYFRAME_SNAP;
    MapData.light_keyframe_valid = true;
    socket_command_map_descriptor_absent(MAP_UPDATE_CMD_SAME, false);
    bool success = MapData.light_keyframe_valid && MapData.light_keyframe_generation == 7 &&
                   MapData.light_keyframe_start_seconds == 120 &&
                   MapData.light_keyframe_end_seconds == 180;
    socket_command_map_descriptor_absent(MAP_UPDATE_CMD_PARTIAL, false);
    success = success && MapData.light_keyframe_valid;
    socket_command_map_descriptor_absent(MAP_UPDATE_CMD_NEW, false);
    success = success && !MapData.light_keyframe_valid && MapData.light_keyframe_generation == 0;

    MapData.light_keyframe_generation = saved_generation;
    MapData.light_keyframe_start_seconds = saved_start;
    MapData.light_keyframe_end_seconds = saved_end;
    MapData.light_keyframe_flags = saved_flags;
    MapData.light_keyframe_valid = saved_valid;
    return success;
}
#endif

static void socket_command_map_apply(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    int mask, x, y, rx, ry;
    int mapstat;
    int xpos, ypos;
    int layer, ext_flags;
    uint64_t light_keyframe_generation = 0;
    uint64_t light_keyframe_start_seconds = 0;
    uint64_t light_keyframe_end_seconds = 0;
    uint8_t light_keyframe_flags = 0;
    bool timed_light_header = false;
    uint8_t num_layers;
    region_map_def_map_t *def_map;
    bool region_map_fow_need_update;
    bool map_visible_change;

    if (!map_protocol_validate(
            data,
            len,
            pos,
            MAP_LOOK_TO_WIRE_SIZE(setting_get_int(OPT_CAT_MAP, OPT_MAP_WIDTH)),
            MAP_LOOK_TO_WIRE_SIZE(setting_get_int(OPT_CAT_MAP, OPT_MAP_HEIGHT)))) {
        LOG(PACKET, "Rejected malformed map packet.");
        socket_command_map_abort_pending();
        return;
    }

    mapstat = packet_reader_read_uint8(&reader);
    if (mapstat != MAP_UPDATE_CMD_PARTIAL) {
        if (MapData.continuation.pending) {
            /* A different envelope interrupts the declared continuation. Roll
             * the entire unpublished sequence back before accepting it. */
            socket_command_map_abort_pending();
        }
        map_state_transaction_begin(mapstat != MAP_UPDATE_CMD_SAME);
        map_pending_effects_begin();
        map_continuation_visible_change = false;
        map_continuation_region_fow_update = false;
    }
    map_visible_change = mapstat != MAP_UPDATE_CMD_SAME && mapstat != MAP_UPDATE_CMD_PARTIAL;

    if (mapstat != MAP_UPDATE_CMD_SAME && mapstat != MAP_UPDATE_CMD_PARTIAL) {
        char mapname[HUGE_BUF], bg_music[HUGE_BUF], weather[MAX_BUF], region_name[MAX_BUF],
            region_longname[MAX_BUF], mappath[HUGE_BUF];
        uint8_t height_diff;

        packet_reader_read_string(&reader, mapname, sizeof(mapname));
        packet_reader_read_string(&reader, bg_music, sizeof(bg_music));
        packet_reader_read_string(&reader, weather, sizeof(weather));
        height_diff = packet_reader_read_uint8(&reader);
        MapData.region_has_map = packet_reader_read_uint8(&reader);
        packet_reader_read_string(&reader, VS(region_name));
        packet_reader_read_string(&reader, VS(region_longname));
        packet_reader_read_string(&reader, VS(mappath));

        if (mapstat == MAP_UPDATE_CMD_NEW) {
            int map_w, map_h;

            map_w = packet_reader_read_uint8(&reader);
            map_h = packet_reader_read_uint8(&reader);
            xpos = packet_reader_read_uint8(&reader);
            ypos = packet_reader_read_uint8(&reader);
            init_map_data(map_w, map_h, xpos, ypos);
        } else {
            int xoff, yoff, zoff;

            packet_reader_read_uint8(&reader);
            xoff = packet_reader_read_int8(&reader);
            yoff = packet_reader_read_int8(&reader);
            zoff = packet_reader_read_int8(&reader);
            xpos = packet_reader_read_uint8(&reader);
            ypos = packet_reader_read_uint8(&reader);
            display_mapscroll(xoff, yoff, 0, 0);
            map_level_scroll(zoff);

            map_pending_effects.footstep = true;
        }

        snprintf(MapData.name_new, sizeof(MapData.name_new), "%s", mapname);
        map_pending_effects.rich_presence = true;
        snprintf(VS(map_pending_effects.bg_music), "%s", bg_music);
        map_pending_effects.music = true;
        snprintf(VS(map_pending_effects.weather_name), "%s", weather);
        map_pending_effects.weather = true;
        update_map_height_diff(height_diff);
        update_map_region_name(region_name);
        update_map_region_longname(region_longname);
        update_map_path(mappath);
    } else {
        xpos = packet_reader_read_uint8(&reader);
        ypos = packet_reader_read_uint8(&reader);

        /* Have we moved? */
        int dx, dy;
        if (socket_command_map_movement_delta(mapstat, xpos, ypos, &dx, &dy)) {
            display_mapscroll(dx, dy, 0, 0);
            map_pending_effects.footstep = true;
            map_visible_change = true;
        }

    }

    uint8_t player_sub_layer = packet_reader_read_uint8(&reader);
    uint16_t continuation_marker = packet_reader_read_uint16(&reader);

    timed_light_header = (continuation_marker & MAP2_CONTINUATION_TIMED_LIGHT) != 0;
    continuation_marker &= (uint16_t)~MAP2_CONTINUATION_TIMED_LIGHT;
    if (timed_light_header) {
        light_keyframe_generation = packet_reader_read_uint64(&reader);
        light_keyframe_start_seconds = packet_reader_read_uint64(&reader);
        light_keyframe_end_seconds = packet_reader_read_uint64(&reader);
        light_keyframe_flags = packet_reader_read_uint8(&reader);
        if (!map_light_keyframe_transaction_begin(light_keyframe_generation,
                                                  light_keyframe_start_seconds,
                                                  light_keyframe_end_seconds,
                                                  light_keyframe_flags)) {
            LOG(PACKET, "Rejected invalid timed-light generation descriptor.");
            socket_command_map_abort_timed_light();
            return;
        }
        MapData.light_keyframe_generation = light_keyframe_generation;
        MapData.light_keyframe_start_seconds = light_keyframe_start_seconds;
        MapData.light_keyframe_end_seconds = light_keyframe_end_seconds;
        MapData.light_keyframe_flags = light_keyframe_flags;
        MapData.light_keyframe_valid = true;
    } else {
        socket_command_map_descriptor_absent(mapstat, map_light_keyframe_transaction_pending());
    }

    if (pos >= len) {
        LOG(PACKET, "Map packet has no level count.");
        socket_command_map_abort_timed_light();
        return;
    }

    uint8_t level_count = packet_reader_read_uint8(&reader);
    if (level_count > MAP2_LEVELS) {
        LOG(PACKET, "Map packet contains too many levels: %" PRIu8 ".", level_count);
        socket_command_map_abort_timed_light();
        return;
    }

    uint16_t incoming_level_mask = 0;
    size_t scan_pos = pos;
    packet_reader_t scan;
    packet_reader_init_cursor(&scan, data, len, &scan_pos);
    for (uint8_t level_num = 0; level_num < level_count; level_num++) {
        int depth = packet_reader_read_int8(&scan);
        uint32_t level_size = packet_reader_read_uint32(&scan);
        incoming_level_mask |= UINT16_C(1) << MAP2_DEPTH_INDEX(depth);
        packet_reader_skip(&scan, level_size);
    }
    if (!packet_reader_finish(&scan)) {
        LOG(PACKET, "Could not inspect validated map level blocks.");
        socket_command_map_abort_timed_light();
        return;
    }
    if (mapstat == MAP_UPDATE_CMD_PARTIAL &&
        !map_protocol_continuation_matches(&MapData.continuation,
                                           continuation_marker,
                                           xpos,
                                           ypos,
                                           player_sub_layer,
                                           incoming_level_mask)) {
        LOG(PACKET, "Rejected unsolicited, mismatched, or out-of-sequence map continuation.");
        socket_command_map_abort_timed_light();
        return;
    }

    MapData.posx = xpos;
    MapData.posy = ypos;
    MapData.player_sub_layer = player_sub_layer;
    def_map = region_map_find_map(MapData.region_map, MapData.map_path);
    map_get_real_coords(&rx, &ry);
    region_map_fow_need_update = false;

    uint16_t level_mask = 0;
    size_t packet_end = len;

    for (uint8_t level_num = 0; level_num < level_count; level_num++) {
        if (len - pos < sizeof(int8_t) + sizeof(uint32_t)) {
            LOG(PACKET, "Truncated map level header.");
            socket_command_map_abort_timed_light();
            return;
        }

        int depth = packet_reader_read_int8(&reader);
        uint32_t level_size = packet_reader_read_uint32(&reader);
        if (depth < -MAP2_MAX_DEPTH || depth > MAP2_MAX_DEPTH || level_size > len - pos) {
            LOG(PACKET, "Invalid map level depth or payload size.");
            socket_command_map_abort_timed_light();
            return;
        }

        size_t level_end = pos + level_size;
        len = level_end;
        if (!map_select_level(depth, true)) {
            LOG(PACKET, "Could not select map level %d.", depth);
            socket_command_map_abort_timed_light();
            return;
        }
        uint16_t level_bit = UINT16_C(1) << MAP2_DEPTH_INDEX(depth);
        if (level_mask & level_bit) {
            LOG(PACKET, "Map packet contains duplicate depth %d.", depth);
            socket_command_map_abort_timed_light();
            return;
        }
        level_mask |= level_bit;

        while (pos < level_end) {
            if (len - pos < sizeof(uint16_t)) {
                LOG(PACKET, "Truncated map tile mask.");
                socket_command_map_abort_timed_light();
                return;
            }

            mask = packet_reader_read_uint16(&reader);
            x = (mask >> 11) & 0x1f;
            y = (mask >> 6) & 0x1f;

            /* Clear the whole cell? */
            if (mask & MAP2_MASK_CLEAR) {
                map_cell_snapshot_t before;

                map_cell_snapshot(x, y, &before);
                map_clear_cell(x, y, (mask & MAP2_MASK_HARD_CLEAR) != 0);
                map_visible_change |= map_cell_changed(x, y, &before);
                continue;
            }

            map_cell_snapshot_t before;
            map_cell_snapshot(x, y, &before);

            size_t tile_values = 0;
            if (mask & MAP2_MASK_SUPPORT_HEIGHT) {
                tile_values += sizeof(int16_t);
            }
            if (mask & MAP2_MASK_FOW) {
                tile_values++;
            }
            if (mask & MAP2_MASK_LIGHT_LEVEL) {
                tile_values += sizeof(uint16_t);
            }
            if (mask & MAP2_MASK_LIGHT_LEVEL_MORE) {
                tile_values += sizeof(uint16_t) * (NUM_SUB_LAYERS - 1);
            }
            if (len - pos < tile_values + sizeof(num_layers)) {
                LOG(PACKET, "Truncated map tile metadata.");
                socket_command_map_abort_timed_light();
                return;
            }

            if (mask & MAP2_MASK_SUPPORT_HEIGHT) {
                map_set_structural_support_height(x, y, packet_reader_read_int16(&reader));
            }

            bool fow_updated = (mask & MAP2_MASK_FOW) != 0;
            bool tile_fow =
                fow_updated ? packet_reader_read_uint8(&reader) != 0 : map_get_fow(x, y);

            /* Do we have light-level information? */
            if (mask & MAP2_MASK_LIGHT_LEVEL) {
                map_set_light_radiance(x, y, 0, packet_reader_read_uint16(&reader));
            }

            if (mask & MAP2_MASK_LIGHT_LEVEL_MORE) {
                int sub_layer;

                for (sub_layer = 1; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
                    map_set_light_radiance(x, y, sub_layer, packet_reader_read_uint16(&reader));
                }
            }

            num_layers = packet_reader_read_uint8(&reader);

            /* Go through all the layers on this tile. */
            for (layer = 0; layer < num_layers; layer++) {
                uint8_t type;

                type = packet_reader_read_uint8(&reader);

                /* Clear this layer. */
                if (type == MAP2_LAYER_CLEAR) {
                    map_set_data(x,
                                 y,
                                 packet_reader_read_uint8(&reader),
                                 0,
                                 0,
                                 0,
                                 "",
                                 "",
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 "",
                                 0);
                } else { /* We have some data. */
                    int16_t face, height = 0, zoom_x = 0, zoom_y = 0, align = 0, rotate = 0;
                    uint8_t flags, obj_flags, quick_pos = 0, probe = 0, draw_double = 0, alpha = 0,
                                              infravision = 0, target_is_friend = 0;
                    uint8_t anim_speed, anim_facing, anim_flags, anim_state, priority, secondpass,
                        roof, door, exit, glow_speed;
                    char player_name[64], player_color[COLOR_BUF], glow[COLOR_BUF];
                    uint32_t target_object_count = 0;

                    anim_speed = anim_facing = anim_flags = anim_state = 0;
                    priority = secondpass = roof = door = exit = glow_speed = 0;

                    player_name[0] = '\0';
                    player_color[0] = '\0';
                    glow[0] = '\0';

                    face = packet_reader_read_uint16(&reader);
                    /* Object flags. */
                    obj_flags = packet_reader_read_uint8(&reader);
                    /* Flags of this layer. */
                    flags = packet_reader_read_uint8(&reader);

                    /* Multi-arch? */
                    if (flags & MAP2_FLAG_MULTI) {
                        quick_pos = packet_reader_read_uint8(&reader);
                    }

                    /* Player name? */
                    if (flags & MAP2_FLAG_NAME) {
                        packet_reader_read_string(&reader, VS(player_name));
                        packet_reader_read_string(&reader, VS(player_color));
                    }

                    /* Animation? */
                    if (flags & MAP2_FLAG_ANIMATION) {
                        anim_speed = packet_reader_read_uint8(&reader);
                        anim_facing = packet_reader_read_uint8(&reader);
                        anim_flags = packet_reader_read_uint8(&reader);

                        if (anim_flags & ANIM_FLAG_MOVING) {
                            anim_state = packet_reader_read_uint8(&reader);
                        }
                    }

                    /* Z position? */
                    if (flags & MAP2_FLAG_HEIGHT) {
                        height = packet_reader_read_int16(&reader);
                    }

                    /* Align? */
                    if (flags & MAP2_FLAG_ALIGN) {
                        align = packet_reader_read_int16(&reader);
                    }

                    if (flags & MAP2_FLAG_INFRAVISION) {
                        infravision = 1;
                    }

                    /* Double? */
                    if (flags & MAP2_FLAG_DOUBLE) {
                        draw_double = 1;
                    }

                    if (flags & MAP2_FLAG_MORE) {
                        uint32_t flags2;

                        flags2 = packet_reader_read_uint32(&reader);

                        if (flags2 & MAP2_FLAG2_ALPHA) {
                            alpha = packet_reader_read_uint8(&reader);
                        }

                        if (flags2 & MAP2_FLAG2_ROTATE) {
                            rotate = packet_reader_read_int16(&reader);
                        }

                        /* Zoom? */
                        if (flags2 & MAP2_FLAG2_ZOOM) {
                            zoom_x = packet_reader_read_uint16(&reader);
                            zoom_y = packet_reader_read_uint16(&reader);
                        }

                        if (flags2 & MAP2_FLAG2_TARGET) {
                            target_object_count = packet_reader_read_uint32(&reader);
                            target_is_friend = packet_reader_read_uint8(&reader);
                        }

                        /* Target's HP? */
                        if (flags2 & MAP2_FLAG2_PROBE) {
                            probe = packet_reader_read_uint8(&reader);
                        }

                        if (flags2 & MAP2_FLAG2_PRIORITY) {
                            priority = 1;
                        }

                        if (flags2 & MAP2_FLAG2_SECONDPASS) {
                            secondpass = 1;
                        }

                        if (flags2 & MAP2_FLAG2_GLOW) {
                            packet_reader_read_string(&reader, VS(glow));
                            glow_speed = packet_reader_read_uint8(&reader);
                        }

                        if (flags2 & MAP2_FLAG2_ROOF) {
                            roof = 1;
                        }

                        if (flags2 & MAP2_FLAG2_DOOR) {
                            door = 1;
                        }

                        if (flags2 & MAP2_FLAG2_EXIT) {
                            exit = 1;
                        }
                    }

                    /* Set the data we figured out. */
                    map_set_data(x,
                                 y,
                                 type,
                                 face,
                                 quick_pos,
                                 obj_flags,
                                 player_name,
                                 player_color,
                                 height,
                                 probe,
                                 zoom_x,
                                 zoom_y,
                                 align,
                                 draw_double,
                                 alpha,
                                 rotate,
                                 infravision,
                                 target_object_count,
                                 target_is_friend,
                                 anim_speed,
                                 anim_facing,
                                 anim_flags,
                                 anim_state,
                                 priority,
                                 secondpass,
                                 roof,
                                 door,
                                 exit,
                                 glow,
                                 glow_speed);
                }
            }

            /* Get tile flags. */
            ext_flags = packet_reader_read_uint8(&reader);

            if (ext_flags & MAP2_FLAG_EXT_LIGHT_RADIANCE_RGB16) {
                uint8_t bitmap = packet_reader_read_uint8(&reader);
                uint16_t rgb[NUM_SUB_LAYERS][3] = {{0}};

                for (uint8_t sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
                    if (!(bitmap & (UINT8_C(1) << sub_layer))) {
                        continue;
                    }
                    for (size_t channel = 0; channel < 3; channel++) {
                        rgb[sub_layer][channel] = packet_reader_read_uint16(&reader);
                    }
                }
                map_set_light_rgb_radiance(x, y, bitmap, rgb);
            }

            if (ext_flags & MAP2_FLAG_EXT_LIGHT_KEYFRAME) {
                uint8_t scalar_bitmap = packet_reader_read_uint8(&reader);
                uint16_t scalar[NUM_SUB_LAYERS] = {0};
                for (uint8_t sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
                    if (scalar_bitmap & (UINT8_C(1) << sub_layer)) {
                        scalar[sub_layer] = packet_reader_read_uint16(&reader);
                    }
                }
                uint8_t rgb_bitmap = packet_reader_read_uint8(&reader);
                uint16_t rgb[NUM_SUB_LAYERS][3] = {{0}};
                for (uint8_t sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
                    if (!(rgb_bitmap & (UINT8_C(1) << sub_layer))) {
                        continue;
                    }
                    for (size_t channel = 0; channel < 3; channel++) {
                        rgb[sub_layer][channel] = packet_reader_read_uint16(&reader);
                    }
                }
                if (MapData.light_keyframe_valid &&
                    !map_light_keyframe_transaction_stage(depth,
                                                          x,
                                                          y,
                                                          scalar_bitmap,
                                                          scalar,
                                                          rgb_bitmap,
                                                          rgb)) {
                    LOG(PACKET, "Timed-light generation exceeded its staging bound.");
                    socket_command_map_abort_timed_light();
                    return;
                }
            }

            /* Animation? */
            if (ext_flags & MAP2_FLAG_EXT_ANIM) {
                uint8_t anim_num = packet_reader_read_uint8(&reader);

                map_visible_change |= anim_num != 0;

                for (uint8_t i = 0; i < anim_num; i++) {
                    uint8_t sub_layer = packet_reader_read_uint8(&reader);
                    uint8_t anim_type = packet_reader_read_uint8(&reader);
                    int16_t anim_value = packet_reader_read_int16(&reader);

                    map_anims_add(anim_type, x, y, sub_layer, depth, anim_value);
                }
            }

            if (fow_updated) {
                map_set_fow(x, y, tile_fow);
            }

            if (depth == 0 && !tile_fow && MapData.region_name[0] != '\0') {
                if (region_map_fow_set_visited(MapData.region_map,
                                               def_map,
                                               MapData.map_path,
                                               rx + x,
                                               ry + y)) {
                    region_map_fow_need_update = true;
                }
            }

            map_visible_change |= map_cell_changed(x, y, &before);
        }

        if (pos != level_end) {
            LOG(PACKET, "Map level payload was not consumed exactly.");
            socket_command_map_abort_timed_light();
            return;
        }

        len = packet_end;
    }

    if (pos != packet_end) {
        LOG(PACKET, "Map packet has trailing data after its level blocks.");
        socket_command_map_abort_timed_light();
        return;
    }

    if (mapstat == MAP_UPDATE_CMD_PARTIAL) {
        map_protocol_continuation_advance(&MapData.continuation);
    } else {
        map_set_level_mask(level_mask);
        map_protocol_continuation_begin(&MapData.continuation,
                                        continuation_marker,
                                        xpos,
                                        ypos,
                                        player_sub_layer,
                                        level_mask);
    }

    map_continuation_visible_change |= map_visible_change;
    map_continuation_region_fow_update |= region_map_fow_need_update;

    if (MapData.continuation.pending) {
        /* Atomic batch replay continues with the next buffered envelope before
         * returning to input, widgets, or presentation. */
        return;
    }

    if (map_light_keyframe_transaction_pending()) {
        map_light_keyframe_transaction_commit();
    }

    uint16_t active_level_mask = map_get_level_mask();
    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        if ((active_level_mask & (UINT16_C(1) << MAP2_DEPTH_INDEX(depth))) &&
            map_select_level(depth, false)) {
            adjust_tile_stretch();
        }
    }
    map_select_level(0, true);
    map_continuation_visible_change |= map_continuation_region_fow_update;
    if (map_continuation_visible_change) {
        map_redraw_request(MAP_REDRAW_REASON_MAP_PACKET);
        minimap_redraw_flag = 1;
    }

    if (map_continuation_region_fow_update) {
        region_map_fow_update(MapData.region_map);
    }

    map_state_transaction_commit();
    map_pending_effects_commit();
    map_publication_generation++;
    map_continuation_visible_change = false;
    map_continuation_region_fow_update = false;
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_map(uint8_t *data, size_t len, size_t pos) {
    int wire_width = MAP_LOOK_TO_WIRE_SIZE(setting_get_int(OPT_CAT_MAP, OPT_MAP_WIDTH));
    int wire_height = MAP_LOOK_TO_WIRE_SIZE(setting_get_int(OPT_CAT_MAP, OPT_MAP_HEIGHT));
    map_protocol_packet_info_t info;
    if (!map_protocol_inspect(data, len, pos, wire_width, wire_height, &info)) {
        LOG(PACKET, "Rejected malformed map packet.");
        socket_command_map_abort_pending();
        return;
    }

    if (map_pending_batch.replaying) {
        socket_command_map_apply(data, len, pos);
        return;
    }

    if (info.mapstat != MAP_UPDATE_CMD_PARTIAL) {
        if (map_pending_batch.continuation.pending) {
            LOG(PACKET, "Discarded interrupted unpublished map generation.");
            map_pending_batch_reset();
        }
        if (info.continuation == 0) {
            socket_command_map_apply(data, len, pos);
            return;
        }

        map_pending_batch_append(data, len, pos);
        map_protocol_continuation_begin(&map_pending_batch.continuation,
                                        info.continuation,
                                        info.x,
                                        info.y,
                                        info.sub_layer,
                                        info.depths);
        return;
    }

    if (!map_protocol_continuation_matches(&map_pending_batch.continuation,
                                           info.continuation,
                                           info.x,
                                           info.y,
                                           info.sub_layer,
                                           info.depths)) {
        LOG(PACKET, "Rejected unsolicited, mismatched, or out-of-sequence map continuation.");
        map_pending_batch_reset();
        return;
    }

    map_pending_batch_append(data, len, pos);
    map_protocol_continuation_advance(&map_pending_batch.continuation);
    if (map_pending_batch.continuation.pending) {
        return;
    }

    map_pending_packet_t *batch = map_pending_batch.head;
    map_pending_batch.head = NULL;
    map_pending_batch.tail = NULL;
    map_pending_batch.replaying = true;
    uint64_t publication_before = map_publication_generation;
    bool replay_complete = true;
    for (map_pending_packet_t *packet = batch; packet != NULL; packet = packet->next) {
        socket_command_map_apply(packet->data, packet->len, packet->pos);
        if (packet->next != NULL &&
            (!map_state_transaction_active() || !MapData.continuation.pending)) {
            replay_complete = false;
            break;
        }
    }
    map_pending_batch.replaying = false;
    map_pending_batch_release(batch);

    if (!replay_complete || map_state_transaction_active() || MapData.continuation.pending ||
        map_publication_generation != publication_before + 1) {
        LOG(PACKET, "Rejected incomplete map generation during atomic publication.");
        socket_command_map_abort_pending();
    }
}

#ifdef ATRINIK_WIDGET_TESTS
bool socket_command_map_buffered_generation_test_begin(void) {
    if (map_pending_batch.continuation.pending || map_pending_batch.head != NULL) {
        return false;
    }
    uint8_t staged_envelope[] = {CLIENT_CMD_MAP, MAP_UPDATE_CMD_NEW};
    map_pending_batch_append(staged_envelope, sizeof(staged_envelope), 1);
    map_protocol_continuation_begin(&map_pending_batch.continuation, 1, 18, 24, 0, 1);
    return map_pending_batch.continuation.pending && map_pending_batch.head != NULL;
}

bool socket_command_map_buffered_generation_test_pending(void) {
    return map_pending_batch.continuation.pending || map_pending_batch.head != NULL;
}

bool socket_command_map_continuation_transaction_test(void) {
    if (map_state_transaction_active() || MapData.continuation.pending ||
        map_pending_batch.continuation.pending) {
        return false;
    }
    int saved_posx = MapData.posx;
    int saved_posy = MapData.posy;
    uint32_t saved_target = cpl.target_object_index;
    MapData.posx = 17;
    MapData.posy = 23;

    /* Model a command-budget yield after the first envelope. The validated
     * generation is retained as wire data, so every live read/action still
     * sees the previously published coordinates and target. */
    bool success = socket_command_map_buffered_generation_test_begin() &&
                   !map_state_transaction_active() && MapData.posx == 17 && MapData.posy == 23 &&
                   cpl.target_object_index == saved_target && !map_pending_effects.active;
    socket_command_map_abort_pending();
    success = success && !socket_command_map_buffered_generation_test_pending() &&
              MapData.posx == 17 && MapData.posy == 23 &&
              cpl.target_object_index == saved_target;

    map_state_transaction_begin(false);
    MapData.posx = 18;
    MapData.posy = 24;
    map_protocol_continuation_begin(&MapData.continuation, 1, 0, 0, 0, 0);
    success = success && map_state_transaction_active() && MapData.continuation.pending &&
              MapData.continuation.next == 1;
    map_protocol_continuation_advance(&MapData.continuation);
    map_state_transaction_commit();
    success = success && !map_state_transaction_active() && !MapData.continuation.pending &&
              MapData.posx == 18 && MapData.posy == 24;

    map_state_transaction_begin(false);
    map_pending_effects_begin();
    map_pending_effects.rich_presence = true;
    map_pending_effects.music = true;
    map_pending_effects.weather = true;
    map_pending_effects.footstep = true;
    MapData.posx = 19;
    MapData.posy = 25;
    map_protocol_continuation_begin(&MapData.continuation, 1, 0, 0, 0, 0);
    success = success && map_state_transaction_active() && MapData.continuation.pending;
    socket_command_map_abort_pending();
    int retry_dx, retry_dy;
    success = success && !map_state_transaction_active() && !MapData.continuation.pending &&
              !map_pending_effects.active &&
              MapData.posx == 18 && MapData.posy == 24 &&
              socket_command_map_movement_delta(
                  MAP_UPDATE_CMD_SAME, 19, 25, &retry_dx, &retry_dy) &&
              retry_dx == 1 && retry_dy == 1;
    MapData.posx = saved_posx;
    MapData.posy = saved_posy;
    cpl.target_object_index = saved_target;
    return success;
}
#endif

/** @copydoc socket_command_struct::handle_func */
void socket_command_version(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    if (cpl.state != ST_WAITVERSION) {
        LOG(BUG,
            "Received version command when not in proper "
            "state: %d, should be: %d.",
            cpl.state,
            ST_WAITVERSION);
        return;
    }

    cpl.server_socket_version = packet_reader_read_uint32(&reader);
    if (cpl.server_socket_version != SOCKET_VERSION) {
        draw_info(COLOR_RED, "The client and server use incompatible gameplay protocol versions.");
        client_attempt_secrets_clear(
            selected_server != NULL ? &selected_server->join_password : NULL,
            &clioption_settings.join_password,
            selected_server != NULL ? &selected_server->rendezvous_invite : NULL);
        cpl.state = ST_START;
        return;
    }

    cpl.state = ST_VERSION;
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_compressed(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    uint32_t declared_len;
    uint8_t type, *dest;
    size_t dest_size;

    type = packet_reader_read_uint8(&reader);
    declared_len = packet_reader_read_uint32(&reader);
    packet_view_t compressed = packet_reader_read_view(&reader, packet_reader_remaining(&reader));
    if (packet_reader_error(&reader) != PACKET_ERROR_NONE ||
        declared_len > PACKET_PAYLOAD_MAX - 1 || type >= CLIENT_CMD_NROF ||
        type == CLIENT_CMD_REGION_MAP) {
        packet_reader_set_error(&reader,
                                declared_len > PACKET_PAYLOAD_MAX - 1 ? PACKET_ERROR_LIMIT_EXCEEDED
                                                                      : PACKET_ERROR_UNSUPPORTED);
        return;
    }

    dest_size = (size_t)declared_len + 1;
    dest = xmalloc(dest_size);
    dest[0] = type;

    uLongf actual_len = declared_len;
    if (uncompress((Bytef *)dest + 1, &actual_len, compressed.data, compressed.len) == Z_OK &&
        actual_len == declared_len) {
        command_buffer *buf;

        buf = command_buffer_new((size_t)actual_len + 1, dest);
        add_input_command(buf);
    } else {
        packet_reader_set_error(&reader, PACKET_ERROR_INVALID_ENCODING);
    }

    free(dest);
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_control(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    char app_name[MAX_BUF];
    uint8_t type, sub_type;

    packet_reader_read_string(&reader, app_name, sizeof(app_name));
    type = packet_reader_read_uint8(&reader);
    sub_type = packet_reader_read_uint8(&reader);

    if (type == CMD_CONTROL_PLAYER && sub_type == CMD_CONTROL_PLAYER_TELEPORT) {
        SDL_RaiseWindow(ScreenWindow);
    }
}
