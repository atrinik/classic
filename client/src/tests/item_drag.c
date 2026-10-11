/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Headless tests of the actual drag lifecycle; object lookup and mouse are stubbed. */
#include "../events/event.c"
#include <stdio.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

Client_Player cpl;

static object dragged_item;
static bool item_present;
static float mouse_x, mouse_y;
static unsigned int drops;

object *object_find(tag_t tag) {
    return item_present && tag == dragged_item.tag ? &dragged_item : NULL;
}

SDL_MouseButtonFlags SDLCALL SDL_GetMouseState(float *x, float *y) {
    if (x != NULL) {
        *x = mouse_x;
    }
    if (y != NULL) {
        *y = mouse_y;
    }
    return 0;
}

static void drop_item(void) {
    TEST_CHECK(object_find(cpl.dragging_tag) == &dragged_item);
    drops++;
}

static void start_drag(void) {
    event_dragging_stop();
    dragged_item.tag = 42;
    item_present = true;
    mouse_x = 10;
    mouse_y = 20;
    drops = 0;
    event_dragging_start(dragged_item.tag, 10, 20);
    event_dragging_set_callback(drop_item);
}

static void test_missing_before_threshold(void) {
    start_drag();
    item_present = false;
    /* Rendering and widget drops use this gate, even without pointer movement. */
    TEST_CHECK(!event_dragging_check());
    TEST_CHECK(cpl.dragging_tag == 0);
    TEST_CHECK(event_drag_cb == NULL);
    TEST_CHECK(!event_dragging_need_redraw());
    mouse_x += 3;
    event_dragging_stop_internal();
    TEST_CHECK(drops == 0);
}

static void test_missing_after_visible_drag(void) {
    start_drag();
    mouse_x += 3;
    TEST_CHECK(event_dragging_check());
    TEST_CHECK(event_dragging_need_redraw());
    item_present = false;
    TEST_CHECK(!event_dragging_need_redraw());
    TEST_CHECK(cpl.dragging_tag == 0);
    TEST_CHECK(event_drag_cb == NULL);
    TEST_CHECK(!event_dragging_check());
    event_dragging_stop_internal();
    TEST_CHECK(drops == 0);
}

static void test_missing_at_release(void) {
    start_drag();
    mouse_y -= 3;
    TEST_CHECK(event_dragging_check());
    item_present = false;
    /* Exercise the actual unhandled-drop path without a prior render check. */
    event_dragging_stop_internal();
    TEST_CHECK(drops == 0);
    TEST_CHECK(cpl.dragging_tag == 0);
    TEST_CHECK(event_drag_cb == NULL);
}

static void test_valid_threshold_and_drop(void) {
    start_drag();
    mouse_x += 2;
    mouse_y -= 2;
    TEST_CHECK(!event_dragging_check());
    TEST_CHECK(cpl.dragging_tag == dragged_item.tag);
    TEST_CHECK(event_drag_cb == drop_item);
    event_dragging_stop_internal();
    TEST_CHECK(drops == 0);
    TEST_CHECK(cpl.dragging_tag == 0);
    TEST_CHECK(event_drag_cb == NULL);

    start_drag();
    mouse_y -= 3;
    TEST_CHECK(event_dragging_check());
    TEST_CHECK(event_dragging_need_redraw());
    TEST_CHECK(!event_dragging_need_redraw());
    mouse_x++;
    TEST_CHECK(event_dragging_need_redraw());
    event_dragging_stop_internal();
    TEST_CHECK(drops == 1);
    TEST_CHECK(cpl.dragging_tag == 0);
    TEST_CHECK(event_drag_cb == NULL);
    event_dragging_stop_internal();
    TEST_CHECK(drops == 1);
}

static void test_cancel_clears_callback(void) {
    start_drag();
    event_dragging_stop();
    TEST_CHECK(cpl.dragging_tag == 0);
    TEST_CHECK(event_drag_cb == NULL);
    /* Some widget entry points start a drag by assigning the tag directly. */
    cpl.dragging_tag = dragged_item.tag;
    mouse_x += 3;
    event_dragging_stop_internal();
    TEST_CHECK(drops == 0);

    start_drag();
    event_dragging_start(dragged_item.tag, 10, 20);
    mouse_x += 3;
    event_dragging_stop_internal();
    TEST_CHECK(drops == 0);
}

int main(void) {
    test_missing_before_threshold();
    test_missing_after_visible_drag();
    test_missing_at_release();
    test_valid_threshold_and_drop();
    test_cancel_clears_callback();
    return 0;
}
