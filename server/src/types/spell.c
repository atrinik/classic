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
 * Handles code for @ref SPELL "spells".
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <server_main.h>
#include <object.h>
#include <object_methods.h>
#include <metrics.h>
#include <player.h>

/** @copydoc object_methods_t::ranged_fire_func */
static int ranged_fire_func(object *op, object *shooter, int dir, double *delay) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(shooter != NULL);

    int cost = cast_spell(shooter, shooter, dir, op->stats.sp, 0, CAST_NORMAL, NULL);
    if (cost != 0) {
        if (shooter->type == PLAYER) {
            metrics_add(&CONTR(shooter)->metrics, METRIC_CHARACTER_SPELLS_CAST, 1);
            metrics_add(&CONTR(shooter)->metrics, METRIC_CHARACTER_MANA_SPENT, (uint64_t)cost);
            const char *spell_id = spell_id_from_index(op->stats.sp);
            char id[METRICS_UNIQUE_ID_MAX + 1];
            if (spell_id != NULL && metrics_format_content_id(VS(id), "spell", spell_id)) {
                metrics_keyed_add(&CONTR(shooter)->metrics,
                                  METRIC_KEYED_CHARACTER_SPELLS_CAST,
                                  id,
                                  1);
                metrics_keyed_add(&CONTR(shooter)->metrics,
                                  METRIC_KEYED_CHARACTER_MANA_SPENT_BY_SPELL,
                                  id,
                                  (uint64_t)cost);
            }
        }
        shooter->stats.sp -= cost;

        if (delay != NULL) {
            *delay = spells[op->stats.sp].time;
        }

        return OBJECT_METHOD_OK;
    }

    if (shooter->type == PLAYER) {
        metrics_add(&CONTR(shooter)->metrics, METRIC_CHARACTER_SPELLS_FAILED, 1);
        const char *spell_id = spell_id_from_index(op->stats.sp);
        char id[METRICS_UNIQUE_ID_MAX + 1];
        if (spell_id != NULL && metrics_format_content_id(VS(id), "spell", spell_id)) {
            metrics_keyed_add(&CONTR(shooter)->metrics,
                              METRIC_KEYED_CHARACTER_SPELLS_FAILED,
                              id,
                              1);
        }
    }

    return OBJECT_METHOD_UNHANDLED;
}

/**
 * Initialize the spell type object methods.
 */
OBJECT_TYPE_INIT_DEFINE(spell) {
    OBJECT_METHODS(SPELL)->apply_func = object_apply_item;
    OBJECT_METHODS(SPELL)->ranged_fire_func = ranged_fire_func;
}
