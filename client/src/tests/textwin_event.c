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

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <textwin.h>
#include <widget.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

static widgetdata *primary_chat;

widgetdata *widget_find(widgetdata *where, int type, const char *id, SDL_Surface *surface) {
    (void)where;
    (void)id;
    (void)surface;

    return type == CHATWIN_ID ? primary_chat : NULL;
}

static void test_focused_chat_owns_printable_key_sequence(void) {
    textwin_tab_struct tab = {0};
    textwin_struct textwin = {.tabs = &tab, .tabs_num = 1};
    widgetdata widget = {.subwidget = &textwin};
    SDL_Event event = {0};

    primary_chat = &widget;
    tab.text_input.focus = 1;

    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_R;
    TEST_CHECK(textwin_chat_input_event_tab(&widget, &event) == &tab);

    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = "r";
    TEST_CHECK(textwin_chat_input_event_tab(&widget, &event) == &tab);

    event.type = SDL_EVENT_TEXT_EDITING;
    event.edit.text = "r";
    TEST_CHECK(textwin_chat_input_event_tab(&widget, &event) == &tab);

    event.type = SDL_EVENT_KEY_UP;
    event.key.key = SDLK_R;
    TEST_CHECK(textwin_chat_input_event_tab(&widget, &event) == &tab);
}

static void test_chat_ownership_boundaries(void) {
    textwin_tab_struct tab = {0};
    textwin_struct textwin = {.tabs = &tab, .tabs_num = 1};
    widgetdata widget = {.subwidget = &textwin};
    widgetdata other = {.subwidget = &textwin};
    SDL_Event event = {.type = SDL_EVENT_KEY_DOWN};

    primary_chat = &widget;
    event.key.key = SDLK_R;

    TEST_CHECK(textwin_chat_input_event_tab(&widget, &event) == NULL);

    tab.text_input.focus = 1;
    TEST_CHECK(textwin_chat_input_event_tab(&other, &event) == NULL);

    textwin.tab_selected = 1;
    TEST_CHECK(textwin_chat_input_event_tab(&widget, &event) == NULL);
    textwin.tab_selected = 0;

    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    TEST_CHECK(textwin_chat_input_event_tab(&widget, &event) == NULL);
}

int main(void) {
    test_focused_chat_owns_printable_key_sequence();
    test_chat_ownership_boundaries();
    return 0;
}
