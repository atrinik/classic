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
 * Handles code used for @ref SAVEBED "savebeds".
 */

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <hiscore.h>
#include <player.h>
#include <object.h>
#include <object_methods.h>
#include <region.h>
#include <gameplay_journal.h>

/** @copydoc object_methods_t::apply_func */
static int apply_func(object *op, object *applier, int aflags) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(applier != NULL);

    if (applier->type != PLAYER) {
        return OBJECT_METHOD_OK;
    }

    char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE] = "";
    bool changed = strcmp(CONTR(applier)->savebed_map, applier->map->path) != 0 ||
                   CONTR(applier)->bed_x != applier->x || CONTR(applier)->bed_y != applier->y;
    if (changed) {
        char map_identity[GAMEPLAY_JOURNAL_ID_MAX + 1];
        char previous_identity[GAMEPLAY_JOURNAL_ID_MAX + 1];
        char subject[GAMEPLAY_JOURNAL_ID_MAX + 1];
        char lineage[GAMEPLAY_JOURNAL_ID_MAX + 1];
        int subject_length;
        int lineage_length;
        if (!gameplay_journal_map_identity(applier->map, map_identity) ||
            !gameplay_journal_map_path_identity(CONTR(applier)->savebed_map, previous_identity) ||
            (subject_length = snprintf(VS(subject), "map:%s", map_identity)) < 0 ||
            (size_t)subject_length >= sizeof(subject) ||
            (lineage_length = snprintf(VS(lineage),
                                       "previous-map:%s@%d+%d",
                                       previous_identity[0] != '\0' ? previous_identity : "unset",
                                       CONTR(applier)->bed_x,
                                       CONTR(applier)->bed_y)) < 0 ||
            (size_t)lineage_length >= sizeof(lineage) ||
            !gameplay_journal_milestone_begin(applier,
                                              GAMEPLAY_JOURNAL_PROGRESSION,
                                              "survival.savebed-changed",
                                              subject,
                                              lineage,
                                              0,
                                              1,
                                              transaction)) {
            return OBJECT_METHOD_OK;
        }
    }

    /* Update respawn position. */
    snprintf(VS(CONTR(applier)->savebed_map), "%s", applier->map->path);
    CONTR(applier)->bed_x = applier->x;
    CONTR(applier)->bed_y = applier->y;
    metrics_add(&CONTR(applier)->metrics, METRIC_CHARACTER_SAVEBEDS_BOUND, 1);
    if (applier->map->region != NULL && applier->map->region->name != NULL) {
        char id[METRICS_UNIQUE_ID_MAX + 1];
        if (metrics_format_content_id(VS(id), "region", applier->map->region->name)) {
            metrics_mark_unique(&CONTR(applier)->metrics,
                                METRIC_COLLECTION_CHARACTER_SAVEBED_REGIONS,
                                id);
        }
    }

    if (!gameplay_journal_semantic_commit(transaction)) {
        return OBJECT_METHOD_OK;
    }

    draw_info(COLOR_WHITE, applier, "You save and your save bed location is updated.");
    hiscore_check(applier, 0);
    player_save(applier);

    return OBJECT_METHOD_OK;
}

/**
 * Initialize the savebed type object methods.
 */
OBJECT_TYPE_INIT_DEFINE(savebed) {
    OBJECT_METHODS(SAVEBED)->apply_func = apply_func;
}
