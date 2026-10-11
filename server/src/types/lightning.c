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
 * Handles code related to @ref LIGHTNING "lightning".
 */

#include <global.h>
#include <server_main.h>
#include <object.h>
#include <object_methods.h>

/**
 * Causes lightning to fork.
 *
 * @param op
 * Original bolt.
 * @param tmp
 * First piece of the fork.
 */
static void lightning_fork(object *op, object *tmp) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(tmp != NULL);

    if (tmp->stats.Dex == 0 || rndm(1, 100) > tmp->stats.Dex) {
        return;
    }

    int dir_adjust;
    if (rndm(0, 99) < tmp->stats.Con) {
        /* Fork left. */
        dir_adjust = -1;
    } else {
        /* Fork right. */
        dir_adjust = 1;
    }

    int dir = absdir(tmp->direction + dir_adjust);

    object *bolt = object_get();
    object_copy(bolt, tmp, false);

    projectile3d_turn(bolt, dir);
    bolt->map = tmp->map;
    if (!projectile3d_can_move(bolt)) {
        bolt->map = NULL;
        object_destroy(bolt);
        return;
    }
    bolt->map = NULL;
    bolt->stats.food = 0;
    /* Retain the existing random decisions and damage split after a valid
     * fork path has been established. Each copy owns its fractional state. */
    bolt->stats.Dex -= 10;
    tmp->stats.Dex -= 10;
    bolt->stats.Con += 25 * dir;
    bolt->speed_left = -0.1f;
    bolt->stats.hp++;
    bolt->stats.dam /= 2;
    bolt->stats.dam++;
    tmp->stats.dam /= 2;
    tmp->stats.dam++;
    bolt = object_insert_map(bolt, tmp->map, op, INS_NO_MERGE | INS_NO_WALK_ON);
    if (bolt != NULL) {
        common_object_projectile_move(bolt);
    }
}

/** @copydoc object_methods_t::projectile_move_func */
static object *projectile_move_func(object *op) {
    HARD_ASSERT(op != NULL);

    if (op->stats.food == 0) {
        op = common_object_projectile_move(op);
        if (op == NULL) {
            return NULL;
        }

        op->stats.food = 1;
    } else if (op->stats.food == 1) {
        object *tmp = object_get();
        object_copy(tmp, op, false);
        tmp->speed_left = -0.1f;
        tmp->x = op->x;
        tmp->y = op->y;
        tmp = object_insert_map(tmp, op->map, op, INS_NO_MERGE | INS_NO_WALK_ON);
        if (tmp == NULL) {
            return NULL;
        }

        tmp->stats.food = 0;
        tmp->last_sp++;
        tag_t tmp_count = tmp->count;
        object_process(tmp);
        if (OBJECT_VALID(tmp, tmp_count) && tmp->map != NULL) {
            lightning_fork(op, tmp);
        }
        op->stats.food = 2;
        if (!projectile3d_hit_current(op)) {
            return NULL;
        }
    } else if (!projectile3d_hit_current(op)) {
        return NULL;
    }

    return op;
}

/** @copydoc object_methods_t::projectile_stop_func */
static object *projectile_stop_func(object *op, int reason) {
    HARD_ASSERT(op != NULL);

    if (reason == OBJECT_PROJECTILE_STOP_EOL) {
        return common_object_projectile_stop_spell(op, reason);
    }

    return op;
}

/**
 * Initialize the lightning type object methods.
 */
OBJECT_TYPE_INIT_DEFINE(lightning) {
    OBJECT_METHODS(LIGHTNING)->projectile_move_func = projectile_move_func;
    OBJECT_METHODS(LIGHTNING)->projectile_stop_func = projectile_stop_func;
    OBJECT_METHODS(LIGHTNING)->process_func = common_object_projectile_process;
    OBJECT_METHODS(LIGHTNING)->projectile_hit_func = common_object_projectile_hit;
    OBJECT_METHODS(LIGHTNING)->move_on_func = common_object_projectile_move_on;
}
