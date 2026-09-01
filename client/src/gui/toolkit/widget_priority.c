/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Atrinik Development Team                         *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Priority-list operations shared by the widget toolkit and its tests.
 */

#include <stddef.h>
#include <toolkit/toolkit.h>
#include <widget.h>

bool widget_priority_is_ancestor(const widgetdata *ancestor, const widgetdata *widget) {
    for (const widgetdata *node = widget; node != NULL; node = node->env) {
        if (node == ancestor) {
            return true;
        }
    }

    return false;
}

void widget_priority_to_back(widgetdata *node, widgetdata **root_head, widgetdata **root_foot) {
    if (node == NULL || node->next == NULL) {
        return;
    }

    if (node->prev == NULL) {
        if (node->env != NULL) {
            node->env->inv = node->next;
        } else {
            *root_head = node->next;
        }

        node->next->prev = NULL;
    } else {
        node->next->prev = node->prev;
        node->prev->next = node->next;
    }

    if (node->env != NULL) {
        node->prev = node->env->inv_rev;
        node->env->inv_rev = node;
    } else {
        node->prev = *root_foot;
        *root_foot = node;
    }

    node->prev->next = node;
    node->next = NULL;
}

void widget_priority_map_to_back(widgetdata *map, widgetdata **root_head, widgetdata **root_foot) {
    for (widgetdata *node = map; node != NULL; node = node->env) {
        widget_priority_to_back(node, root_head, root_foot);
    }
}
