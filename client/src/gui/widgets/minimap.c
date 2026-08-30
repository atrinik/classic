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
 * Implements minimap type widgets.
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <video.h>
#include <surface_primitives.h>
#include <region_map.h>

/**
 * Textures used by the minimap.
 */
enum {
    MINIMAP_TEXTURE_BG,
    MINIMAP_TEXTURE_MASK,
    MINIMAP_TEXTURE_BORDER,
    MINIMAP_TEXTURE_BORDER_ROTATED,
    MINIMAP_TEXTURE_NUM
};

/**
 * Possible minimap display types.
 */
typedef enum {
    MINIMAP_TYPE_PREFER_REGION_MAP,
    MINIMAP_TYPE_REGION_MAP,
    MINIMAP_TYPE_DYNAMIC,

    MINIMAP_TYPE_NUM
} minimap_type_t;

/**
 * Number of pixels from the border to the circle in the minimap texture.
 */
#define MINIMAP_CIRCLE_PADDING(widget) \
    (10. * ((double)(widget)->w / TEXTURE_CLIENT(minimap_texture_names[MINIMAP_TEXTURE_BG])->w))

/**
 * Minimap widget sub-structure.
 */
typedef struct minimap_widget {
    /**
     * Surface used when rendering dynamic maps.
     */
    SDL_Surface *surface;

    /**
     * Cached minimap textures.
     */
    SDL_Surface *textures[MINIMAP_TEXTURE_NUM];

    /**
     * Display type.
     */
    minimap_type_t type;

    /** Last expensive dynamic-world render, in SDL ticks. */
    uint32_t dynamic_redraw_ticks;
} minimap_widget_t;

/**
 * Texture names to load.
 */
static const char *const minimap_texture_names[MINIMAP_TEXTURE_NUM] = {"minimap_bg",
                                                                       "minimap_mask",
                                                                       "minimap_border",
                                                                       "minimap_border_rotated"};

/**
 * String representations of the display types.
 */
static const char *const minimap_display_modes[MINIMAP_TYPE_NUM] = {"Prefer region maps",
                                                                    "Only region maps",
                                                                    "Only dynamic maps"};

/** Return whether this minimap currently uses the dynamic world renderer. */
static bool minimap_is_dynamic(const minimap_widget_t *minimap) {
    return minimap->type == MINIMAP_TYPE_DYNAMIC ||
           (minimap->type == MINIMAP_TYPE_PREFER_REGION_MAP && !MapData.region_has_map);
}

/** Return whether a requested minimap refresh should run this frame. */
bool minimap_redraw_due(void) {
    if (!minimap_redraw_flag) {
        return false;
    }

    widgetdata *widget = cur_widget[MINIMAP_ID];
    if (widget == NULL || widget->hidden) {
        return false;
    }
    if (widget->subwidget == NULL) {
        return true;
    }

    minimap_widget_t *minimap = widget->subwidget;
    if (!minimap_is_dynamic(minimap) || minimap->surface == NULL) {
        return true;
    }

    return client_ui_ticks() - minimap->dynamic_redraw_ticks >= MINIMAP_DYNAMIC_REDRAW_INTERVAL;
}

void minimap_redraw_force(void) {
    minimap_redraw_flag = 1;

    widgetdata *widget = cur_widget[MINIMAP_ID];
    if (widget == NULL || widget->subwidget == NULL) {
        return;
    }

    minimap_widget_t *minimap = widget->subwidget;
    minimap->dynamic_redraw_ticks = client_ui_ticks() - MINIMAP_DYNAMIC_REDRAW_INTERVAL;
}

/** @copydoc widgetdata::draw_func */
static void widget_draw(widgetdata *widget) {
    minimap_widget_t *minimap;
    SDL_Rect box;
    size_t i;

    minimap = widget->subwidget;

    /* No surface or the widget dimensions changed, (re-)create the surface
     * and the zoomed minimap textures. */
    if (widget->surface == NULL || widget->surface->w != widget->w ||
        widget->surface->h != widget->h) {
        SDL_Surface *texture;

        if (widget->surface != NULL) {
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
        if (widget->surface == NULL) {
            LOG(ERROR, "Could not create minimap widget surface: %s", SDL_GetError());
            return;
        }
        if (!gpu_renderer_canvas_register(&widget->surface)) {
            LOG(ERROR, "Could not create retained GPU minimap target: %s", SDL_GetError());
            return;
        }
        minimap_redraw_flag = 1;
        /* A newly allocated canvas has no composed minimap to throttle. Make
         * its first dynamic draw due even during the first 250 ms of uptime. */
        minimap->dynamic_redraw_ticks = client_ui_ticks() - MINIMAP_DYNAMIC_REDRAW_INTERVAL;

        for (i = 0; i < MINIMAP_TEXTURE_NUM; i++) {
            if (minimap->textures[i] != NULL) {
                SDL_DestroySurface(minimap->textures[i]);
            }

            texture = TEXTURE_CLIENT(minimap_texture_names[i]);
            minimap->textures[i] = zoomSurface(texture,
                                               (double)widget->w / texture->w + 0.001,
                                               (double)widget->h / texture->h + 0.001,
                                               setting_get_int(OPT_CAT_CLIENT, OPT_ZOOM_FILTER));
            if (minimap->textures[i] == NULL) {
                LOG(ERROR, "Could not resize minimap texture: %s", SDL_GetError());
                minimap->textures[i] = surface_to_display_alpha(texture);
                if (minimap->textures[i] == NULL) {
                    SDL_DestroySurface(widget->surface);
                    widget->surface = NULL;
                    return;
                }
            }
        }
    }

    if (minimap_redraw_due()) {
        minimap_redraw_flag = 0;
        surface_fill_rect(widget->surface, NULL, 0);
        surface_blit(minimap->textures[MINIMAP_TEXTURE_BG], NULL, widget->surface, NULL);

        /* Determine which version of the minimap to show based on the user's
         * preferences. */
        if (minimap->type == MINIMAP_TYPE_REGION_MAP ||
            (minimap->type == MINIMAP_TYPE_PREFER_REGION_MAP && MapData.region_has_map)) {
            /* Free dynamic map surface. */
            if (minimap->surface != NULL) {
                SDL_DestroySurface(minimap->surface);
                minimap->surface = NULL;
            }

            if (region_map_ready(MapData.region_map)) {
                int cx, cy, sx, sy;
                double rad;
                SDL_Rect rect;
                SDL_Surface *surface;

                cx = (widget->w - MINIMAP_CIRCLE_PADDING(widget) * 2) / 2;
                cy = (widget->h - MINIMAP_CIRCLE_PADDING(widget) * 2) / 2;
                rad = 45.0 * (M_PI / 180.0);
                sx = cx * cos(rad);
                sy = cy * sin(rad);

                rect.x = cx - sx + MINIMAP_CIRCLE_PADDING(widget);
                rect.y = cy - sy + MINIMAP_CIRCLE_PADDING(widget);
                rect.w = widget->surface->w - MINIMAP_CIRCLE_PADDING(widget) * 2 - (cx - sx) * 2;
                rect.h = widget->surface->h - MINIMAP_CIRCLE_PADDING(widget) * 2 - (cy - sy) * 2;

                MapData.region_map->pos.x += rect.x;
                MapData.region_map->pos.y += rect.y;
                MapData.region_map->pos.w = rect.w;
                MapData.region_map->pos.h = rect.h;
                region_map_resize(MapData.region_map, 0);
                region_map_pan(MapData.region_map);
                MapData.region_map->pos.x -= rect.x;
                MapData.region_map->pos.y -= rect.y;
                MapData.region_map->pos.w = widget->surface->w;
                MapData.region_map->pos.h = widget->surface->h;

                surface = region_map_surface(MapData.region_map);

                surface_blit(surface, &MapData.region_map->pos, widget->surface, NULL);
                region_map_render_fow(MapData.region_map, widget->surface, 0, 0);
                region_map_render_marker(MapData.region_map, widget->surface, 0, 0);
            } else {
                SDL_Rect tmp;

                tmp.w = widget->w;
                tmp.h = widget->h;
                text_show(widget->surface,
                          FONT_SANS10,
                          "Downloading...",
                          0,
                          0,
                          COLOR_HGOLD,
                          TEXT_ALIGN_CENTER | TEXT_VALIGN_CENTER | TEXT_OUTLINE,
                          &tmp);
            }

            surface_blit(minimap->textures[MINIMAP_TEXTURE_MASK], NULL, widget->surface, NULL);
            surface_blit(minimap->textures[MINIMAP_TEXTURE_BORDER_ROTATED],
                         NULL,
                         widget->surface,
                         NULL);
        } else {
            if (minimap->surface == NULL) {
                minimap->surface = surface_create_rgb(get_video_flags(),
                                                      MINIMAP_DYNAMIC_SURFACE_WIDTH,
                                                      MINIMAP_DYNAMIC_SURFACE_HEIGHT,
                                                      video_get_bpp(),
                                                      0,
                                                      0,
                                                      0,
                                                      0);
                if (minimap->surface == NULL) {
                    LOG(ERROR, "Could not create dynamic minimap surface: %s", SDL_GetError());
                    return;
                }
            }

            map_draw_map_gpu_auxiliary(minimap->surface);
            minimap->dynamic_redraw_ticks = client_ui_ticks();

            float source_width =
                (float)(MAP_FOW_SIZE + 1) * 10000.0f / (float)MapData.region_map->zoom;
            float source_height = source_width;
            source_width = MIN(source_width, (float)minimap->surface->w);
            source_height = MIN(source_height, (float)minimap->surface->h);
            SDL_FRect source = {
                ((float)minimap->surface->w - source_width) / 2.0f,
                ((float)minimap->surface->h - source_height) / 2.0f,
                source_width,
                source_height,
            };
            SDL_FRect destination = {0.0f, 0.0f, (float)widget->w, (float)widget->h};
            if (!gpu_renderer_draw_map_to(
                    widget->surface,
                    &source,
                    &destination,
                    zoom_filter_to_scale_mode(setting_get_int(OPT_CAT_CLIENT, OPT_ZOOM_FILTER)))) {
                LOG(ERROR, "Could not compose dynamic GPU minimap: %s", SDL_GetError());
                return;
            }

            surface_blit(minimap->textures[MINIMAP_TEXTURE_MASK], NULL, widget->surface, NULL);
            surface_blit(minimap->textures[MINIMAP_TEXTURE_BORDER], NULL, widget->surface, NULL);
        }

        SDL_SetSurfaceColorKey(widget->surface, true, getpixel(widget->surface, 0, 0));
        SDL_SetSurfaceRLE(widget->surface, true);
    }

    box.x = widget->x;
    box.y = widget->y;
    surface_show(OfflineRenderSurface, box.x, box.y, NULL, widget->surface);
}

#ifdef ATRINIK_WIDGET_TESTS
void widget_minimap_draw_test(widgetdata *widget) {
    widget_draw(widget);
}

void widget_minimap_cold_start_draw_test(widgetdata *widget) {
    HARD_ASSERT(widget != NULL);
    HARD_ASSERT(widget->subwidget != NULL);

    minimap_widget_t *minimap = widget->subwidget;
    minimap_type_t type = minimap->type;
    uint8_t hidden = widget->hidden;
    SDL_DestroySurface(widget->surface);
    widget->surface = NULL;
    SDL_DestroySurface(minimap->surface);
    minimap->surface = NULL;
    minimap_redraw_flag = 0;
    minimap->type = MINIMAP_TYPE_DYNAMIC;
    widget->hidden = 0;
    widget_draw(widget);
    widget->hidden = hidden;
    minimap->type = type;
}
#endif

void widget_minimap_refresh_test(widgetdata *widget) {
    HARD_ASSERT(widget != NULL);
    HARD_ASSERT(widget->subwidget != NULL);
    HARD_ASSERT(widget == cur_widget[MINIMAP_ID]);
    minimap_redraw_force();
}

/** @copydoc widgetdata::event_func */
static int widget_event(widgetdata *widget, SDL_Event *event) {
    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        if (event_wheel_y(event) > 0.0f) {
            /* Zoom in. */
            if (MapData.region_map->zoom < 100) {
                MapData.region_map->zoom += 5;
                minimap_redraw_flag = 1;
                return 1;
            }
        } else if (event_wheel_y(event) < 0.0f) {
            /* Zoom out. */
            if (MapData.region_map->zoom > 10) {
                MapData.region_map->zoom -= 5;
                minimap_redraw_flag = 1;
                return 1;
            }
        }
    }

    return 0;
}

/** @copydoc widgetdata::deinit_func */
static void widget_deinit(widgetdata *widget) {
    minimap_widget_t *minimap;
    size_t i;

    minimap = widget->subwidget;
    SDL_DestroySurface(minimap->surface);

    for (i = 0; i < MINIMAP_TEXTURE_NUM; i++) {
        SDL_DestroySurface(minimap->textures[i]);
    }
}

/** @copydoc widgetdata::load_func */
static int widget_load(widgetdata *widget, const char *keyword, const char *parameter) {
    minimap_widget_t *minimap;

    minimap = widget->subwidget;

    if (strcmp(keyword, "type") == 0) {
        minimap->type = atoi(parameter);
        return 1;
    }

    return 0;
}

/** @copydoc widgetdata::save_func */
static void widget_save(widgetdata *widget, FILE *fp, const char *padding) {
    minimap_widget_t *minimap;

    minimap = widget->subwidget;

    fprintf(fp, "%stype = %d\n", padding, minimap->type);
}

static void
menu_minimap_display_change(widgetdata *widget, widgetdata *menuitem, SDL_Event *event) {
    minimap_widget_t *minimap;
    widgetdata *tmp2;
    _widget_label *label;
    size_t i;

    minimap = widget->subwidget;

    for (tmp2 = menuitem->inv; tmp2; tmp2 = tmp2->next) {
        if (tmp2->type == LABEL_ID) {
            label = LABEL(tmp2);

            for (i = 0; i < MINIMAP_TYPE_NUM; i++) {
                if (strcmp(minimap_display_modes[i], label->text) == 0) {
                    minimap->type = i;
                    break;
                }
            }

            minimap_redraw_flag = 1;

            break;
        }
    }
}

static void menu_minimap_display(widgetdata *widget, widgetdata *menuitem, SDL_Event *event) {
    minimap_widget_t *minimap;
    widgetdata *submenu;
    size_t i;

    minimap = widget->subwidget;
    submenu = MENU(menuitem->env)->submenu;

    for (i = 0; i < MINIMAP_TYPE_NUM; i++) {
        add_menuitem(submenu,
                     minimap_display_modes[i],
                     &menu_minimap_display_change,
                     MENU_RADIO,
                     minimap->type == i);
    }
}

/** @copydoc widgetdata::menu_handle_func */
static int widget_menu_handle(widgetdata *widget, SDL_Event *event) {
    widgetdata *menu;

    menu = create_menu(event_mouse_x(event), event_mouse_y(event), widget);

    widget_menu_standard_items(widget, menu);
    add_menuitem(menu, "Display  >", &menu_minimap_display, MENU_SUBMENU, 0);

    menu_finalize(menu);

    return 1;
}

/** @copydoc widgetdata::padding_func */
static void widget_padding(widgetdata *widget, int *x, int *y) {
    *x = *y = MINIMAP_CIRCLE_PADDING(widget);
}

void widget_minimap_init(widgetdata *widget) {
    minimap_widget_t *minimap;

    minimap = xcalloc(1, sizeof(*minimap));
    minimap->type = MINIMAP_TYPE_PREFER_REGION_MAP;
    MapData.region_map->zoom = 50;

    widget->draw_func = widget_draw;
    widget->event_func = widget_event;
    widget->deinit_func = widget_deinit;
    widget->load_func = widget_load;
    widget->save_func = widget_save;
    widget->menu_handle_func = widget_menu_handle;
    widget->padding_func = widget_padding;
    widget->subwidget = minimap;
}
