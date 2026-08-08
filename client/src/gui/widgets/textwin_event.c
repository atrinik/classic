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
 * Event routing helpers for text window widgets.
 */

#include <global.h>

textwin_tab_struct *textwin_chat_input_event_tab(widgetdata *widget, const SDL_Event *event) {
    textwin_struct *textwin;

    if (event->type != SDL_EVENT_KEY_DOWN && event->type != SDL_EVENT_KEY_UP &&
        event->type != SDL_EVENT_TEXT_INPUT && event->type != SDL_EVENT_TEXT_EDITING) {
        return NULL;
    }

    textwin = TEXTWIN(widget);

    if (textwin->tabs == NULL || textwin->tab_selected >= textwin->tabs_num ||
        textwin->tabs[textwin->tab_selected].text_input.focus != 1 ||
        widget != widget_find(NULL, CHATWIN_ID, NULL, NULL)) {
        return NULL;
    }

    return &textwin->tabs[textwin->tab_selected];
}
