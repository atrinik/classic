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
 * Handles code related to @ref LIGHT_REFILL "light refills".
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <server_main.h>
#include <server_item.h>
#include <server.h>
#include <object.h>
#include <player.h>
#include <object_methods.h>
#include <book_edit.h>

/** @copydoc object_methods_t::apply_func */
static int apply_func(object *op, object *applier, int aflags) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(applier != NULL);

    if (applier->type != PLAYER) {
        return OBJECT_METHOD_UNHANDLED;
    }

    /* Ink uses the familiar mark-tool/apply-refill interaction. */
    if (op->race != NULL && strcmp(op->race, "writing_ink") == 0) {
        object *pen = book_edit_marked_inventory(applier);
        if (!book_edit_is_pen(pen) || pen->env != applier ||
            pen->race == NULL || strcmp(pen->race, "writing_ink") != 0 ||
            !book_edit_inventory_contains(applier, op)) {
            draw_info(COLOR_WHITE, applier,
                      "Mark a writing pen in your main inventory, then apply a carried ink bottle.");
            return OBJECT_METHOD_OK;
        }
        if (QUERY_FLAG(pen, FLAG_UNPAID) || QUERY_FLAG(op, FLAG_UNPAID)) {
            draw_info(COLOR_WHITE, applier, "You should pay for the pen and ink first.");
            return OBJECT_METHOD_OK;
        }
        if (pen->nrof > 1) {
            draw_info(COLOR_WHITE, applier, "Split off one writing pen before refilling it.");
            return OBJECT_METHOD_OK;
        }
        if (pen->stats.maxhp <= 0 || pen->stats.food < 0 ||
            pen->stats.food >= pen->stats.maxhp || op->stats.food <= 0) {
            draw_info(COLOR_WHITE, applier, "The pen is full or this bottle has no usable ink.");
            return OBJECT_METHOD_OK;
        }
        int amount = MIN(pen->stats.maxhp - pen->stats.food, op->stats.food);
        if (amount < op->stats.food && op->nrof > 1) {
            op = object_stack_get_reinsert(op, 1);
            if (op->nrof > 1) {
                draw_info(COLOR_WHITE, applier, "Split off one ink bottle before refilling.");
                return OBJECT_METHOD_OK;
            }
        }
        pen->stats.food += amount;
        if (amount == op->stats.food) {
            decrease_ob(op);
        } else {
            op->stats.food -= amount;
            esrv_update_item(UPD_NAME, op);
        }
        esrv_update_item(UPD_NAME, pen);
        draw_info_format(COLOR_WHITE, applier,
                         "Your pen now has %d/%d ink. Writing costs 1 ink per new or replaced "
                         "UTF-8 byte; deletions, renaming and signing are free.",
                         pen->stats.food, pen->stats.maxhp);
        return OBJECT_METHOD_OK;
    }

    object *light = find_marked_object(applier);
    if (light == NULL) {
        draw_info_format(COLOR_WHITE,
                         applier,
                         "You need to mark a light source that you "
                         "want to refill.");
        return OBJECT_METHOD_OK;
    }

    char *light_name = object_get_name_s(light, applier);

    if (light->type != LIGHT_APPLY || light->race == NULL || light->race != op->race) {
        char *name = object_get_name_s(op, applier);
        draw_info_format(COLOR_WHITE,
                         applier,
                         "You can't refill the %s with the %s.",
                         light_name,
                         name);
        free(name);
        goto out;
    }

    int capacity_missing = light->stats.maxhp - light->stats.food;
    if (capacity_missing == 0) {
        draw_info_format(COLOR_WHITE, applier, "The %s is full and can't be refilled.", light_name);
        goto out;
    }

    int capacity_received = MIN(capacity_missing, op->stats.food);
    light->stats.food += capacity_received;

    int percent = (double)light->stats.food / light->stats.maxhp * 100.0;
    draw_info_format(COLOR_WHITE,
                     applier,
                     "You refill the %s and it's now at %d%% of its capacity.",
                     light_name,
                     percent);

    /* Check whether the refilling object was all used up. If so,
     * decrease it, otherwise split it from the stack (if any) and
     * decrease the amount of units it gives back. */
    if (op->stats.food - capacity_received <= 0) {
        decrease_ob(op);
    } else {
        op = object_stack_get_reinsert(op, 1);
        op->stats.food -= capacity_received;
    }

out:
    free(light_name);
    return OBJECT_METHOD_OK;
}

/**
 * Initialize the light refill type object methods.
 */
OBJECT_TYPE_INIT_DEFINE(light_refill) {
    OBJECT_METHODS(LIGHT_REFILL)->apply_func = apply_func;
}
