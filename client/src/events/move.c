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
 * Handles movement events.
 */

#include <client.h>
#include <event.h>
#include <keybind.h>
#include <main.h>
#include <map.h>
#include <player.h>
#include <settings.h>
#include <toolkit/toolkit.h>
#include <client_socket.h>
#include <toolkit/packet.h>

/** Whether a running movement producer still needs a direction-zero stop. */
static bool run_stream_active;

/**
 * Number of the possible directions.
 */
#define DIRECTIONS_NUM 9

/**
 * Directions to fire into.
 */
static const int directions_fire[DIRECTIONS_NUM] = {6, 5, 4, 7, 0, 3, 8, 1, 2};

static void client_send_fire_stream(int num, tag_t tag, uint32_t epoch) {
    packet_struct *packet;

    packet = packet_new(SERVER_CMD_FIRE, 64, 64);
    packet_writer_write_uint8(packet, directions_fire[num - 1]);
    packet_writer_write_uint32(packet, tag);
    packet_writer_write_uint32(packet, epoch);
    socket_send_packet(packet);
}

void client_send_fire(int num, tag_t tag) {
    client_send_fire_stream(num, tag, 0);
}

/** Stop a repeated/running movement stream without applying fire modifiers. */
void move_keys_clear(void) {
    packet_struct *packet = packet_new(SERVER_CMD_CLEAR, 0, 0);

    socket_send_packet(packet);
    run_stream_active = false;
}

/** Remove queued commands for one movement stream without clearing other input. */
static void move_keys_clear_stream(uint8_t command, uint32_t epoch) {
    packet_struct *packet = packet_new(SERVER_CMD_CLEAR, 5, 5);

    packet_writer_write_uint8(packet, command);
    packet_writer_write_uint32(packet, epoch);
    socket_send_packet(packet);
    if (command == SERVER_CMD_MOVE) {
        run_stream_active = false;
    }
}

static void move_keys_send_move(int num, bool run_on, uint32_t epoch) {
    packet_struct *packet = packet_new(SERVER_CMD_MOVE, 10, 0);

    packet_writer_write_uint8(packet, num == 0 ? 0 : directions_fire[num - 1]);
    packet_writer_write_uint8(packet, run_on);
    packet_writer_write_uint32(packet, epoch);
    socket_send_packet(packet);
}

/** Return whether a running movement producer still needs a stop. */
bool move_keys_run_stream_active(void) {
    return run_stream_active;
}

/** Stop a running movement stream without applying fire modifiers. */
void move_keys_run_stop(void) {
    move_keys_send_move(0, false, 0);
    run_stream_active = false;
}

/** Replace the queued direction in the active movement or directional-fire stream. */
void move_keys_replace(int num, uint32_t epoch) {
    move_keys_clear_stream(cpl.fire_on ? SERVER_CMD_FIRE : SERVER_CMD_MOVE, epoch);
    move_keys_stream(num, epoch);
}

/** Stop one keyboard movement epoch without clearing unrelated queued input. */
void move_keys_stream_stop(uint32_t epoch) {
    move_keys_clear_stream(SERVER_CMD_MOVE, epoch);
}

/** Emit a direction associated with one replaceable keyboard movement epoch. */
void move_keys_stream(int num, uint32_t epoch) {
    if (cpl.fire_on) {
        client_send_fire_stream(num, 0, epoch);
    } else {
        run_stream_active = cpl.run_on;
        move_keys_send_move(num, cpl.run_on, epoch);
    }
}

/** Drain a logical movement state through the production packet adapter. */
void keybind_movement_state_emit(keybind_movement_state *state) {
    uint8_t direction;
    uint32_t epoch;
    keybind_movement_action action;

    while ((action = keybind_movement_state_flush(state, &direction, &epoch)) !=
           KEYBIND_MOVEMENT_ACTION_NONE) {
        if (action == KEYBIND_MOVEMENT_ACTION_MOVE) {
            move_keys_stream(direction, epoch);
        } else if (action == KEYBIND_MOVEMENT_ACTION_REPLACE) {
            move_keys_replace(direction, epoch);
        } else if (action == KEYBIND_MOVEMENT_ACTION_STOP) {
            move_keys_stream_stop(epoch);
        } else if (action == KEYBIND_MOVEMENT_ACTION_RUN_STOP) {
            if (epoch != 0) {
                move_keys_stream_stop(epoch);
            } else {
                move_keys_run_stop();
            }
        } else if (action == KEYBIND_MOVEMENT_ACTION_RUN_TAP_STOP) {
            move_keys_run_stop();
        }
    }
}

void move_keys(int num) {
    if (cpl.fire_on) {
        client_send_fire(num, 0);
    } else {
        if (num == 5) {
            move_keys_clear();
        } else if (num == 0) {
            move_keys_run_stop();
        } else {
            run_stream_active = cpl.run_on;
            move_keys_send_move(num, cpl.run_on, 0);
        }
    }
}

/**
 * Transform tile coordinates into direction, which can be used as a
 * result for functions like move_keys() or ::directions_move (return
 * value - 1).
 * @param tx
 * Tile X.
 * @param ty
 * Tile Y.
 * @return
 * The direction, 1-9.
 */
int dir_from_tile_coords(int tx, int ty) {
    int player_tile_x = MAP_LOOK_TO_WIRE_SIZE(setting_get_int(OPT_CAT_MAP, OPT_MAP_WIDTH)) / 2,
        player_tile_y = MAP_LOOK_TO_WIRE_SIZE(setting_get_int(OPT_CAT_MAP, OPT_MAP_HEIGHT)) / 2;
    int q, x, y;

    if (tx == player_tile_x && ty == player_tile_y) {
        return 5;
    }

    x = -(tx - player_tile_x);
    y = -(ty - player_tile_y);

    if (!y) {
        q = -300 * x;
    } else {
        q = x * 100 / y;
    }

    if (y > 0) {
        /* East */
        if (q < -242) {
            return 6;
        }

        /* Northeast */
        if (q < -41) {
            return 9;
        }

        /* North */
        if (q < 41) {
            return 8;
        }

        /* Northwest */
        if (q < 242) {
            return 7;
        }

        /* West */
        return 4;
    }

    /* West */
    if (q < -242) {
        return 4;
    }

    /* Southwest */
    if (q < -41) {
        return 1;
    }

    /* South */
    if (q < 41) {
        return 2;
    }

    /* Southeast */
    if (q < 242) {
        return 3;
    }

    /* East */
    return 6;
}
