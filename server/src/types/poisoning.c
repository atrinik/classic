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
 * Calculate one stat's depletion after a poison pulse.
 *
 * Keeping protection scaling in this pure calculation makes exact boundary
 * behavior deterministic even though the decision to drain is random.
 */
int poisoning_stat_depletion_after_pulse(int depletion, int protection, bool drains, int amount) {
    int limit = poisoning_stat_depletion_limit(protection);

    depletion = MAX(-limit, MIN(0, depletion));
    if (!drains || limit == 0 || depletion == -limit) {
        return depletion;
    }

    return MAX(-limit, depletion - MAX(1, amount));
}

/**
 * Clamp existing player depletion to the target's current protection.
 *
 * This is separate from adding depletion so protection changes are honored
 * even when the next poison pulse deals no damage.
 */
static bool poisoning_reconcile_stat_depletion(object *poison, object *target) {
    int limit = poisoning_stat_depletion_limit(target->protection[ATNR_POISON]);
    bool changed = false;

    for (int i = 0; i < NUM_STATS; i++) {
        int8_t depletion = get_attr_value(&poison->stats, i);

        if (depletion < -limit) {
            set_attr_value(&poison->stats, i, -limit);
            changed = true;
        }
    }

    return changed;
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

    poisoning_reconcile_stat_depletion(poison, target);

    for (int i = 0; i < NUM_STATS; i++) {
        int8_t depletion = get_attr_value(&poison->stats, i);
        bool drains = rndm_chance(2) && rndm(1, 100) > protection;
        int amount = drains && rndm_chance(6) ? 2 : 1;

        set_attr_value(&poison->stats,
                       i,
                       poisoning_stat_depletion_after_pulse(depletion, protection, drains, amount));
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
    bool depletion_reconciled =
        target->type == PLAYER && poisoning_reconcile_stat_depletion(op, target);

    if (op->owner != NULL && !object_owner(op)) {
        object_owner_clear(op);
    }

    OBJECTS_DESTROYED_BEGIN(target) {
        if (!attack_hit(target, op, op->stats.dam)) {
            if (depletion_reconciled && !OBJECTS_DESTROYED(target)) {
                living_update(target);
            }
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
