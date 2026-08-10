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

#include <global.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

static void link_siblings(widgetdata *nodes[], size_t count, widgetdata *parent) {
    for (size_t i = 0; i < count; i++) {
        nodes[i]->prev = i == 0 ? NULL : nodes[i - 1];
        nodes[i]->next = i + 1 == count ? NULL : nodes[i + 1];
        nodes[i]->env = parent;
    }

    if (parent != NULL) {
        parent->inv = nodes[0];
        parent->inv_rev = nodes[count - 1];
    }
}

static void test_top_level_map_is_backmost(void) {
    widgetdata map = {.type = MAP_ID};
    widgetdata stats = {.type = STAT_ID};
    widgetdata chat = {.type = CHATWIN_ID};
    widgetdata *nodes[] = {&map, &stats, &chat};
    widgetdata *head = &map;
    widgetdata *foot = &chat;

    link_siblings(nodes, arraysize(nodes), NULL);
    widget_priority_map_to_back(&map, &head, &foot);

    TEST_CHECK(head == &stats);
    TEST_CHECK(foot == &map);
    TEST_CHECK(stats.prev == NULL && stats.next == &chat);
    TEST_CHECK(chat.prev == &stats && chat.next == &map);
    TEST_CHECK(map.prev == &chat && map.next == NULL);

    widget_priority_map_to_back(&map, &head, &foot);
    TEST_CHECK(head == &stats && foot == &map);
}

static void test_middle_map_is_relinked_to_back(void) {
    widgetdata stats = {.type = STAT_ID};
    widgetdata map = {.type = MAP_ID};
    widgetdata chat = {.type = CHATWIN_ID};
    widgetdata inventory = {.type = INVENTORY_ID};
    widgetdata *nodes[] = {&stats, &map, &chat, &inventory};
    widgetdata *head = &stats;
    widgetdata *foot = &inventory;

    link_siblings(nodes, arraysize(nodes), NULL);
    widget_priority_map_to_back(&map, &head, &foot);

    TEST_CHECK(head == &stats && foot == &map);
    TEST_CHECK(stats.next == &chat && chat.prev == &stats);
    TEST_CHECK(chat.next == &inventory && inventory.prev == &chat);
    TEST_CHECK(inventory.next == &map && map.prev == &inventory);
}

static void test_nested_map_lowers_complete_ancestor_chain(void) {
    widgetdata map = {.type = MAP_ID};
    widgetdata inventory = {.type = INVENTORY_ID};
    widgetdata inner = {.type = CONTAINER_ID};
    widgetdata inner_hud = {.type = STAT_ID};
    widgetdata outer = {.type = CONTAINER_ID};
    widgetdata outer_hud = {.type = CHATWIN_ID};
    widgetdata menu = {.type = MENU_B_ID};
    widgetdata *inner_nodes[] = {&map, &inner_hud};
    widgetdata *outer_nodes[] = {&inner, &outer_hud};
    widgetdata *root_nodes[] = {&outer, &menu, &inventory};
    widgetdata *head = &outer;
    widgetdata *foot = &inventory;

    link_siblings(inner_nodes, arraysize(inner_nodes), &inner);
    link_siblings(outer_nodes, arraysize(outer_nodes), &outer);
    link_siblings(root_nodes, arraysize(root_nodes), NULL);
    widget_priority_map_to_back(&map, &head, &foot);

    TEST_CHECK(inner.inv == &inner_hud && inner.inv_rev == &map);
    TEST_CHECK(outer.inv == &outer_hud && outer.inv_rev == &inner);
    TEST_CHECK(head == &menu && foot == &outer);
    TEST_CHECK(widget_priority_is_ancestor(&inner, &map));
    TEST_CHECK(widget_priority_is_ancestor(&outer, &map));
    TEST_CHECK(!widget_priority_is_ancestor(&outer_hud, &map));
}

static void test_move_and_detach_reattach_preserve_background(void) {
    widgetdata map = {.type = MAP_ID};
    widgetdata container = {.type = CONTAINER_ID};
    widgetdata reattached_container = {.type = CONTAINER_ID};
    widgetdata hud = {.type = MENU_B_ID};
    widgetdata *children[] = {&map};
    widgetdata *roots[] = {&container, &hud};
    widgetdata *head = &container;
    widgetdata *foot = &hud;

    link_siblings(children, arraysize(children), &container);
    link_siblings(roots, arraysize(roots), NULL);
    map.x = 320;
    container.x = 320;
    widget_priority_map_to_back(&map, &head, &foot);
    TEST_CHECK(head == &hud && foot == &container);
    TEST_CHECK(map.x == 320 && container.x == 320);

    container.inv = container.inv_rev = NULL;
    map.env = NULL;
    map.prev = NULL;
    map.next = head;
    head->prev = &map;
    head = &map;
    widget_priority_map_to_back(&map, &head, &foot);
    TEST_CHECK(head == &hud && foot == &map);

    map.prev->next = NULL;
    foot = map.prev;
    map.prev = map.next = NULL;
    widgetdata *reattached[] = {&map};
    link_siblings(reattached, arraysize(reattached), &reattached_container);
    reattached_container.next = head;
    head->prev = &reattached_container;
    head = &reattached_container;
    widget_priority_map_to_back(&map, &head, &foot);
    TEST_CHECK(head == &hud && foot == &reattached_container);
}

static void test_saved_nested_layout_is_repaired_after_load(void) {
    widgetdata map = {.type = MAP_ID, .x = 47, .y = 83};
    widgetdata nested = {.type = CONTAINER_ID, .x = 45, .y = 81};
    widgetdata outer = {.type = CONTAINER_ID, .x = 43, .y = 79};
    widgetdata stats = {.type = STAT_ID};
    widgetdata chat = {.type = CHATWIN_ID};
    widgetdata *nested_children[] = {&map};
    widgetdata *outer_children[] = {&nested};
    widgetdata *saved_roots[] = {&outer, &stats, &chat};
    widgetdata *head = &outer;
    widgetdata *foot = &chat;

    link_siblings(nested_children, arraysize(nested_children), &nested);
    link_siblings(outer_children, arraysize(outer_children), &outer);
    link_siblings(saved_roots, arraysize(saved_roots), NULL);

    widget_priority_map_to_back(&map, &head, &foot);

    TEST_CHECK(head == &stats && foot == &outer);
    TEST_CHECK(map.env == &nested && nested.env == &outer);
    TEST_CHECK(map.x == 47 && map.y == 83);
    TEST_CHECK(nested.x == 45 && nested.y == 81);
    TEST_CHECK(outer.x == 43 && outer.y == 79);
}

int main(void) {
    test_top_level_map_is_backmost();
    test_middle_map_is_relinked_to_back();
    test_nested_map_lowers_complete_ancestor_chain();
    test_move_and_detach_reattach_preserve_background();
    test_saved_nested_layout_is_repaired_after_load();
    return 0;
}
