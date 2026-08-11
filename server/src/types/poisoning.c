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
 * Handles code for @ref POISONING "poisoning" objects.
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <server_main.h>
#include <object.h>
#include <object_methods.h>
#include <poisoning.h>

/**
 * Calculate the per-stat poison depletion bound for a protection value.
 *
 * Protection is clamped to the normal 0-100 range. The ceiling division keeps
 * the result monotonic while ensuring that partial protection never increases
 * depletion.
 */
int poisoning_stat_depletion_limit(int protection) {
    protection = MAX(0, MIN(100, protection));
    return (POISON_MAX_STAT_DEPLETION * (100 - protection) + 99) / 100;
}

/**
 * Apply one pulse of player-only poison stat depletion.
 */
void poisoning_apply_stat_depletion(object *poison, object *target) {
    HARD_ASSERT(poison != NULL);
    HARD_ASSERT(target != NULL);

    if (target->type != PLAYER) {
        return;
    }

    int protection = MAX(0, MIN(100, target->protection[ATNR_POISON]));
    int limit = poisoning_stat_depletion_limit(protection);

    for (int i = 0; i < NUM_STATS; i++) {
        int8_t depletion = get_attr_value(&poison->stats, i);

        /* Protection gained while poisoned takes effect on the next pulse. */
        if (depletion < -limit) {
            set_attr_value(&poison->stats, i, -limit);
            depletion = -limit;
        }

        if (limit > 0 && depletion > -limit && rndm_chance(2) && rndm(1, 100) > protection) {
            int amount = rndm_chance(6) ? 2 : 1;
            set_attr_value(&poison->stats, i, MAX(-limit, depletion - amount));
        }
    }
}

/** @copydoc object_methods_t::process_func */
static void process_func(object *op) {
    HARD_ASSERT(op != NULL);

    if (op->env == NULL || !IS_LIVE(op->env) || op->env->stats.hp < 0) {
        object_remove(op, 0);
        object_destroy(op);
        return;
    }

    object *target = op->env;

    if (op->owner != NULL && !object_owner(op)) {
        object_owner_clear(op);
    }

    OBJECTS_DESTROYED_BEGIN(target) {
        if (!attack_hit(target, op, op->stats.dam)) {
            return;
        }

        if (OBJECTS_DESTROYED(target)) {
            return;
        }
    }
    OBJECTS_DESTROYED_END();

    if (target->type != PLAYER) {
        return;
    }

    poisoning_apply_stat_depletion(op, target);

    living_update(target);
}

/**
 * Initialize the poisoning type object methods.
 */
OBJECT_TYPE_INIT_DEFINE(poisoning) {
    OBJECT_METHODS(POISONING)->process_func = process_func;
}
