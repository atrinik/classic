/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Zoey Rose and Atrinik Development Team           *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/** @file Implements the operator-only /custody command. */

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <object.h>

/** @copydoc command_func */
void command_custody(object *op, const char *command, char *params) {
    if (params == NULL || *params == '\0') {
        draw_info(COLOR_WHITE, op, "Usage: /custody <inventory item>");
        return;
    }

    object *match = NULL;
    for (object *item = op->inv; item != NULL; item = item->below) {
        if (object_matches_string(item, op, params) != 0) {
            match = item;
            break;
        }
    }

    if (match == NULL) {
        draw_info(COLOR_WHITE, op, "No matching item in your inventory.");
        return;
    }

    draw_info_format(COLOR_NAVY, op, "[b]Custody provenance: %s[/b]", object_get_str(match));
    draw_info_format(COLOR_WHITE,
                     op,
                     "  lineage: %s",
                     match->custody_lineage != NULL ? match->custody_lineage : "legacy/unknown");
    draw_info_format(COLOR_WHITE,
                     op,
                     "  quantity segments: %s",
                     match->custody_provenance != NULL ? match->custody_provenance : "legacy/unknown");
    draw_info_format(COLOR_WHITE,
                     op,
                     "  first acquirer: %s",
                     match->custody_first != NULL ? match->custody_first : "legacy/unknown");
    draw_info_format(COLOR_WHITE,
                     op,
                     "  last relinquished by: %s",
                     match->custody_last != NULL ? match->custody_last : "none");
}
