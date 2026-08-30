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
 * Implements notification type widgets.
 *
 * Similar to tooltips, but instead triggered by player actions. Such a
 * notification can even define an action to execute when the notification
 * is clicked, or if the notification has a keybinding shortcut assigned
 * to it, when the shortcut key is pressed (thus overriding normal
 * behavior of that particular shortcut).
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <video.h>
#include <surface_primitives.h>
#include <notification.h>
#include <toolkit/packet.h>
#include <toolkit/string.h>

/**
 * The notification data.
 */
static notification_struct *notification = NULL;

#ifdef ATRINIK_WIDGET_TESTS
static bool notification_test_elapsed_valid;
static uint32_t notification_test_elapsed_ms;
static uint64_t notification_test_compositions;

bool notification_test_fade(uint32_t elapsed_ms) {
    if (notification == NULL || elapsed_ms >= NOTIFICATION_DEFAULT_FADEOUT) {
        return false;
    }
    notification_test_elapsed_valid = true;
    notification_test_elapsed_ms = elapsed_ms;
    return true;
}

uint64_t notification_test_canvas_compositions(void) {
    return notification_test_compositions;
}
#endif

/**
 * Destroy notification data.
 */
void notification_destroy(void) {
#ifdef ATRINIK_WIDGET_TESTS
    notification_test_elapsed_valid = false;
    notification_test_elapsed_ms = 0;
#endif
    if (!notification) {
        return;
    }

    free(notification->action);

    free(notification->shortcut);

    free(notification->message);

    free(notification);
    notification = NULL;
    cur_widget[NOTIFICATION_ID]->show = 0;
}

/**
 * Process notification's action, if any.
 */
static void notification_action_do(void) {
    if (notification && notification->action) {
        /* Macro or command? */
        if (*notification->action == '?') {
            keybind_process_command(notification->action);
        } else {
            send_command_check(notification->action);
        }

        /* Done the action, destroy it... */
        notification_destroy();
    }
}

/**
 * Check whether notification should handle keybinding macro.
 * @param cmd
 * Macro to check.
 * @return
 * 1 if the notification handled the keybinding, 0 otherwise.
 */
int notification_keybind_check(const char *cmd) {
    if (notification_keybind_matches(cmd)) {
        notification_action_do();
        return 1;
    }

    return 0;
}

/** Return whether the active notification owns a keybinding macro. */
bool notification_keybind_matches(const char *cmd) {
    return notification && notification->action && notification->shortcut &&
           !strcmp(notification->shortcut, cmd);
}

/** Recompose the complete notification into its retained GPU canvas. */
static void notification_canvas_compose(widgetdata *widget) {
    HARD_ASSERT(widget != NULL);
    HARD_ASSERT(notification != NULL);
    HARD_ASSERT(notification->message != NULL);
    HARD_ASSERT(widget->surface != NULL);

    SDL_Rect box = {0, 0, widget->surface->w, widget->surface->h};
    SDL_Color color;
    if (text_color_parse("e6e796", &color)) {
        surface_fill_rect(widget->surface,
                          &box,
                          surface_map_rgb(widget->surface, color.r, color.g, color.b));
    }
    border_create_color(widget->surface, &box, 1, "606060");

    box.w = MAX(0, widget->surface->w - 6);
    box.h = MAX(0, widget->surface->h - 6);
    text_show(widget->surface,
              NOTIFICATION_DEFAULT_FONT,
              notification->message,
              3,
              3,
              COLOR_BLACK,
              TEXT_MARKUP | TEXT_WORD_WRAP,
              &box);
#ifdef ATRINIK_WIDGET_TESTS
    notification_test_compositions++;
#endif
}

/** @copydoc socket_command_struct::handle_func */
void socket_command_notification(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    int wd, ht;
    char type, *cp;
    SDL_Rect box;
    StringBuffer *sb;

    /* Destroy previous notification, if any. */
    notification_destroy();
    /* Show the widget... */
    cur_widget[NOTIFICATION_ID]->show = 1;
    SetPriorityWidget(cur_widget[NOTIFICATION_ID]);
    /* Create the data structure and initialize default values. */
    notification = xcalloc(1, sizeof(*notification));
    notification->start_ticks = client_ui_ticks();
    notification->alpha = 255;
    notification->delay = NOTIFICATION_DEFAULT_DELAY;
    sb = stringbuffer_new();

    /* Parse the data. */
    while (packet_reader_error(&reader) == PACKET_ERROR_NONE && pos < len) {
        type = packet_reader_read_uint8(&reader);

        switch (type) {
            case CMD_NOTIFICATION_TEXT: {
                char message[HUGE_BUF];

                if (packet_reader_read_string(&reader, message, sizeof(message))) {
                    stringbuffer_append_string(sb, message);
                }
                break;
            }

            case CMD_NOTIFICATION_ACTION: {
                char action[HUGE_BUF];

                if (packet_reader_read_string(&reader, action, sizeof(action))) {
                    notification->action = xstrdup(action);
                }
                break;
            }

            case CMD_NOTIFICATION_SHORTCUT: {
                char shortcut[HUGE_BUF];

                if (packet_reader_read_string(&reader, shortcut, sizeof(shortcut))) {
                    notification->shortcut = xstrdup(shortcut);
                }
                break;
            }

            case CMD_NOTIFICATION_DELAY:
                notification->delay =
                    MAX(NOTIFICATION_DEFAULT_FADEOUT, packet_reader_read_uint32(&reader));
                break;

            default:
                packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
                break;
        }
    }

    (void)packet_reader_finish(&reader);

    /* Shortcut specified, add the shortcut name to the notification
     * message. */
    if (notification->shortcut) {
        keybind_struct *keybind = keybind_find_by_command(notification->shortcut);

        if (keybind) {
            char key_buf[MAX_BUF];

            keybind_get_key_shortcut(keybind->key, keybind->mod, key_buf, sizeof(key_buf));
            string_toupper(key_buf);
            stringbuffer_append_printf(sb, " (click or [b]%s[/b])", key_buf);
        }
    } else if (notification->action) {
        /* No shortcut, clicking is the best one can do... */
        stringbuffer_append_string(sb, " (click)");
    }

    cp = stringbuffer_finish(sb);
    notification->message = cp;

    /* Calculate the maximum height the text will need. */
    box.x = 0;
    box.y = 0;
    box.w = NOTIFICATION_DEFAULT_WIDTH;
    box.h = 0;
    text_show(NULL,
              NOTIFICATION_DEFAULT_FONT,
              cp,
              0,
              0,
              COLOR_BLACK,
              TEXT_MARKUP | TEXT_WORD_WRAP | TEXT_HEIGHT,
              &box);
    ht = box.h;

    /* Calculate the maximum text width. */
    box.h = 0;
    text_show(NULL,
              NOTIFICATION_DEFAULT_FONT,
              cp,
              0,
              0,
              COLOR_BLACK,
              TEXT_MARKUP | TEXT_WORD_WRAP | TEXT_MAX_WIDTH,
              &box);
    wd = box.w;

    box.x = 0;
    box.y = 0;
    box.w = wd + 6;
    box.h = ht + 6;

    /* Update the notification widget width/height. */
    resize_widget(cur_widget[NOTIFICATION_ID], RESIZE_RIGHT, box.w);
    resize_widget(cur_widget[NOTIFICATION_ID], RESIZE_BOTTOM, box.h);

    if (cur_widget[NOTIFICATION_ID]->surface) {
        SDL_DestroySurface(cur_widget[NOTIFICATION_ID]->surface);
    }

    /* Create a new surface. */
    cur_widget[NOTIFICATION_ID]->surface =
        surface_create_rgb(get_video_flags(), box.w, box.h, video_get_bpp(), 0, 0, 0, 0);
    if (cur_widget[NOTIFICATION_ID]->surface == NULL ||
        !gpu_renderer_canvas_register(&cur_widget[NOTIFICATION_ID]->surface)) {
        LOG(ERROR, "Could not create retained GPU notification canvas: %s", SDL_GetError());
        notification_destroy();
        return;
    }

    notification_canvas_compose(cur_widget[NOTIFICATION_ID]);
}

/** @copydoc widgetdata::draw_func */
static void widget_draw(widgetdata *widget) {
    SDL_Rect dst;

    /* Nothing to render... */
    if (!notification) {
        return;
    }

    if (widget->redraw) {
        notification_canvas_compose(widget);
    }

    /* Update the widget's position to below map name. */
    widget->x = cur_widget[MAPNAME_ID]->x;
    widget->y = cur_widget[MAPNAME_ID]->y + cur_widget[MAPNAME_ID]->h;

    uint32_t elapsed = client_ui_ticks() - notification->start_ticks;
#ifdef ATRINIK_WIDGET_TESTS
    if (notification_test_elapsed_valid) {
        elapsed = notification->delay - NOTIFICATION_DEFAULT_FADEOUT + notification_test_elapsed_ms;
    }
#endif

    /* Check whether we should do fade out. */
    if (elapsed > notification->delay - NOTIFICATION_DEFAULT_FADEOUT) {
        int fade;

        /* Calculate how far into the fading animation we are. */
        fade = elapsed - (notification->delay - NOTIFICATION_DEFAULT_FADEOUT);

        /* Completed the fading animation? */
        if (fade > NOTIFICATION_DEFAULT_FADEOUT) {
            notification_destroy();
            return;
        }

        /* Adjust the alpha value... */
        notification->alpha =
            255 * ((NOTIFICATION_DEFAULT_FADEOUT - fade) / (double)NOTIFICATION_DEFAULT_FADEOUT);
    }

    dst.x = widget->x;
    dst.y = widget->y;
    surface_set_alpha(widget->surface, notification->alpha);
    surface_show(OfflineRenderSurface, dst.x, dst.y, NULL, widget->surface);

    /* Do highlight. */
    if (widget_mouse_event.owner == widget && notification->action) {
        filledRectAlpha(OfflineRenderSurface,
                        dst.x,
                        dst.y,
                        dst.x + widget->w,
                        dst.y + widget->h,
                        0xffffff3c);
    }
}

/** @copydoc widgetdata::event_func */
static int widget_event(widgetdata *widget, SDL_Event *event) {
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
        notification_action_do();
        return 1;
    }

    return 0;
}

/**
 * Initialize one notification widget.
 */
void widget_notification_init(widgetdata *widget) {
    widget->draw_func = widget_draw;
    widget->event_func = widget_event;
}
