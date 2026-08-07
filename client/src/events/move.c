/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
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

#include <global.h>
#include <session_client.h>

/**
 * Number of the possible directions.
 */
#define DIRECTIONS_NUM 9

/**
 * Directions to fire into.
 */
static const int directions_fire[DIRECTIONS_NUM] = {6, 5, 4, 7, 0, 3, 8, 1, 2};

void client_send_fire(int num, tag_t tag) {
    if (num < 1 || num > DIRECTIONS_NUM) {
        LOG(BUG, "Invalid fire direction index: %d", num);
        return;
    }

    session_intent_t intent = {0};
    HARD_ASSERT(session_intent_view(client_session_get(), &intent));
    session_action_result_t result =
        tag != 0 ? client_session_cast(tag, directions_fire[num - 1])
                 : client_session_move(directions_fire[num - 1], intent.run, true);
    if (result != SESSION_ACTION_ACCEPTED) {
        LOG(INFO, "Rejected fire action: %s", session_action_result_string(result));
    }
}

void move_keys(int num) {
    session_intent_t intent = {0};
    HARD_ASSERT(session_intent_view(client_session_get(), &intent));
    if (intent.fire) {
        client_send_fire(num, 0);
    } else {
        session_action_result_t result;
        if (num == 5) {
            result = client_session_stop();
        } else {
            uint8_t direction = 0;
            if (num != 0) {
                if (num < 1 || num > DIRECTIONS_NUM) {
                    LOG(BUG, "Invalid movement direction index: %d", num);
                    return;
                }
                direction = directions_fire[num - 1];
            }
            result = client_session_move(direction, intent.run, false);
        }

        if (result != SESSION_ACTION_ACCEPTED) {
            LOG(INFO, "Rejected movement action: %s", session_action_result_string(result));
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
