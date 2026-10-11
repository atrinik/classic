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
 * Handles code for @ref CONE "cones".
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <propagation.h>
#include <server_main.h>
#include <arch.h>
#include <object_methods.h>

/** @copydoc object_methods_t::process_func */
static void process_func(object *op) {
    HARD_ASSERT(op != NULL);

    if (op->map == NULL) {
        return;
    }

    if (QUERY_FLAG(op, FLAG_LIFESAVE)) {
        attack_hit_map(op, 0, false);
        return;
    }

    attack_hit_map(op, 0, true);

    op->stats.hp -= 2;
    if (op->stats.hp < 0) {
        object_remove(op, 0);
        object_destroy(op);
        return;
    }

    if (op->stats.food != 0) {
        return;
    }

    op->stats.food = 1;

    propagation_expand(op);
}

/** @copydoc object_methods_t::move_on_func */
static int move_on_func(object *op, object *victim, object *originator, int state) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(victim != NULL);

    if (state == 0) {
        return OBJECT_METHOD_OK;
    }

    if (QUERY_FLAG(op, FLAG_SLOW_MOVE)) {
        return OBJECT_METHOD_OK;
    }

    if (IS_LIVE(victim)) {
        attack_hit_situational(victim, op, op->stats.dam);
    }

    return OBJECT_METHOD_OK;
}

/**
 * Initialize the cone type object methods.
 */
OBJECT_TYPE_INIT_DEFINE(cone) {
    OBJECT_METHODS(CONE)->move_on_func = move_on_func;
    OBJECT_METHODS(CONE)->process_func = process_func;
}
