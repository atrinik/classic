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
 * Implements active effects type widgets.
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <active_effects_model.h>
#include <video.h>
#include <toolkit/string.h>

/**
 * Active effects widget data.
 */
typedef struct widget_active_effects_struct {
    uint32_t update_ticks;
} widget_active_effects_struct;

/** @copydoc widgetdata::draw_func */
static void widget_draw(widgetdata *widget) {
    widget_active_effects_struct *tmp;
    const player_status_t *status;
    SDL_Rect box;

    tmp = widget->subwidget;

    if (SDL_GetTicks() - tmp->update_ticks > 1000) {
        uint8_t redraw;
        int sec;

        redraw = 0;
        sec = (SDL_GetTicks() - tmp->update_ticks) / 1000;
        tmp->update_ticks = SDL_GetTicks();

        redraw = active_effects_model_tick(sec);

        widget->redraw += redraw;
    }

    if (!widget->surface || widget->w != widget->surface->w || widget->h != widget->surface->h) {
        if (widget->surface) {
            SDL_DestroySurface(widget->surface);
        }

        widget->surface = surface_create_rgb(get_video_flags(),
                                             widget->w,
                                             widget->h,
                                             video_get_bpp(),
                                             0,
                                             0,
                                             0,
                                             0);
        if (widget->surface != NULL && !gpu_renderer_canvas_register(widget->surface)) {
            LOG(ERROR, "Could not create retained GPU active-effects target: %s", SDL_GetError());
        }
        SDL_SetSurfaceColorKey(widget->surface, true, 0);
        SDL_SetSurfaceRLE(widget->surface, true);
    }

    if (widget->redraw) {
        int x, y;
        sprite_struct *sprite;

        x = y = 0;

        surface_fill_rect(widget->surface, NULL, 0);

        for (status = active_effects_model_rows(); status != NULL; status = status->next) {
            sprite = image_get_sprite(status->face);

            if (!sprite) {
                continue;
            }

            if (x + sprite->bitmap->w > widget->w) {
                x = 0;
                y += sprite->bitmap->h + 5;
            }

            if (y + sprite->bitmap->h > widget->h) {
                resize_widget(widget, RESIZE_BOTTOM, y + sprite->bitmap->h);
                widget->redraw++;
            }

            if (image_get_sprite(status->face) != NULL) {
                surface_show(widget->surface, x, y, NULL, image_get_sprite(status->face)->bitmap);
            }

            if (status->seconds != -1) {
                SDL_Rect textbox;
                char buf[MAX_BUF];

                textbox.w = sprite->bitmap->w;

                if (status->seconds > 60) {
                    snprintf(buf,
                             sizeof(buf),
                             "%d:%02d",
                             status->seconds / 60,
                             status->seconds % 60);
                } else {
                    snprintf(buf, sizeof(buf), "%d", status->seconds);
                }

                text_show(widget->surface,
                          FONT_MONO8,
                          buf,
                          x,
                          y + sprite->bitmap->h - FONT_HEIGHT(FONT_MONO8),
                          COLOR_WHITE,
                          TEXT_OUTLINE | TEXT_ALIGN_CENTER,
                          &textbox);
            }

            x += sprite->bitmap->w + 5;
        }
    }

    box.x = widget->x;
    box.y = widget->y;
    surface_show(OfflineRenderSurface, box.x, box.y, NULL, widget->surface);
}

/** @copydoc widgetdata::event_func */
static int widget_event(widgetdata *widget, SDL_Event *event) {
    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        const player_status_t *status;
        int x, y;
        sprite_struct *sprite;

        x = y = 0;

        for (status = active_effects_model_rows(); status != NULL; status = status->next) {
            sprite = image_get_sprite(status->face);

            if (!sprite) {
                continue;
            }

            if (x + sprite->bitmap->w > widget->w) {
                x = 0;
                y += sprite->bitmap->h + 5;
            }

            if (event_mouse_x(event) >= widget->x + x &&
                event_mouse_x(event) < widget->x + x + sprite->bitmap->w &&
                event_mouse_y(event) >= widget->y + y &&
                event_mouse_y(event) < widget->y + y + sprite->bitmap->h) {
                char buf[HUGE_BUF];

                snprintf(buf,
                         sizeof(buf),
                         "[b]%s[/b]%s%s",
                         status->name,
                         status->tooltip[0] != '\0' ? "\n" : "",
                         status->tooltip);
                tooltip_create(event_mouse_x(event), event_mouse_y(event), FONT_ARIAL11, buf);
                tooltip_multiline(200);
                break;
            }

            x += sprite->bitmap->w + 5;
        }
    }

    return 0;
}

/**
 * Initialize one active effects widget.
 */
void widget_active_effects_init(widgetdata *widget) {
    widget_active_effects_struct *tmp;

    tmp = xcalloc(1, sizeof(*tmp));

    widget->draw_func = widget_draw;
    widget->event_func = widget_event;
    widget->subwidget = tmp;
    widget->unique = 1;
}
