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
 * Implements player info type widgets.
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <session_client.h>

/** @copydoc widgetdata::draw_func */
static void widget_draw(widgetdata *widget) {
    if (!widget->redraw) {
        return;
    }

    SDL_Rect box;
    session_player_t player = {0};
    HARD_ASSERT(session_player_view(client_session_get(), &player));
    box.w = widget->w;
    box.h = widget->h;

    text_show_format(widget->surface,
                     FONT_SERIF14,
                     4,
                     0,
                     COLOR_WHITE,
                     TEXT_VALIGN_CENTER | TEXT_OUTLINE | TEXT_MARKUP,
                     &box,
                     "[font=arial 11][tooltip_conf=800 300]"
                     "[tooltip=This is your character's action timer, i.e., "
                     "the amount of time before the next skill-based action "
                     "can be performed (such as spell casting or firing a "
                     "bow).][/font]%1.2f[/tooltip]",
                     player.action_timer);
    text_show_format(widget->surface,
                     FONT_SERIF14,
                     0,
                     0,
                     COLOR_HGOLD,
                     TEXT_ALIGN_CENTER | TEXT_VALIGN_CENTER | TEXT_OUTLINE | TEXT_MARKUP,
                     &box,
                     "[b]%s[/b]",
                     player.name);

    char buf[32];
    snprintf(VS(buf), "[b]%d[/b]", player.stats.level);
    int wd = text_get_width(FONT_SERIF14, buf, TEXT_MARKUP);
    const char *color = player.stats.level == s_settings->max_level ? COLOR_HGOLD : COLOR_WHITE;
    text_show_format(widget->surface,
                     FONT_SERIF14,
                     widget->w - 4 - wd,
                     0,
                     color,
                     TEXT_MARKUP | TEXT_OUTLINE | TEXT_VALIGN_CENTER,
                     &box,
                     "[font=arial 11][tooltip_conf=800 300][tooltip=This is "
                     "your character's main level.][/font]%s[/tooltip]",
                     buf);

    tooltip_enable_delay(750);
}

/** @copydoc widgetdata::background_func */
static void widget_background(widgetdata *widget, int draw) {
    static uint32_t action_tick = 0;
    session_player_t player = {0};
    HARD_ASSERT(session_player_view(client_session_get(), &player));

    /* Pre-emptively tick down the skill delay timer */
    if (player.action_timer > 0.0f) {
        if (LastTick - action_tick > 125) {
            player.action_timer -= (LastTick - action_tick) / 1000.0f;
            if (player.action_timer < 0.0f) {
                player.action_timer = 0.0f;
            }

            session_reduce_player(client_session_get(), &player);
            cpl.action_timer = player.action_timer;
            action_tick = LastTick;
            WIDGET_REDRAW(widget);
        }
    } else {
        action_tick = LastTick;
    }
}

/** @copydoc widgetdata::event_func */
static int widget_event(struct widgetdata *widget, SDL_Event *event) {
    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        WIDGET_REDRAW(widget);
        return 1;
    }

    return 0;
}

/**
 * Initialize one player info widget.
 */
void widget_playerinfo_init(widgetdata *widget) {
    widget->draw_func = widget_draw;
    widget->background_func = widget_background;
    widget->event_func = widget_event;
}
