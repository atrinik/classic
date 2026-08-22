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
 * Implements network graph type widgets.
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <surface_primitives.h>
#include <network_graph.h>
#include <network_graph_data.h>
#include <toolkit/string.h>

/**
 * Holds network graph data about a particular data type.
 */
typedef struct network_graph_data {
    network_graph_series_t series; ///< The unit-aware graph history.
    Uint32 ticks; ///< When the data was last logged.
} network_graph_data_t;

/**
 * Network graph widget sub-structure.
 */
typedef struct network_graph_widget {
    /**
     * The data type sources.
     */
    network_graph_data_t data[NETWORK_GRAPH_TYPE_MAX];

    /**
     * Currently displayed type.
     */
    int type;

    /**
     * Which traffic types to display.
     */
    uint32_t filters;
} network_graph_widget_t;

/**
 * Work structure used to enqueue a network graph update.
 */
typedef struct network_graph_work {
    struct network_graph_work *next; ///< Next entry.
    int type; ///< Data type.
    int traffic; ///< Traffic type.
    uint64_t value; ///< Sample value.
    bool sample; ///< Whether the current bucket has a sample.
} network_graph_work_t;

/**
 * String representations of the network graph types.
 */
static const char *const network_graph_types[NETWORK_GRAPH_TYPE_MAX] = {
    "Game data",
    "QUIC assets",
    "HTTP data",
    "Latency (ms)",
};

/**
 * String representations of the network traffic types.
 */
static const char *const network_graph_filters[NETWORK_GRAPH_TRAFFIC_MAX] = {"Received",
                                                                             "Transmitted"};

/**
 * Colors of the network graph types.
 */
static const char *const network_graph_colors[NETWORK_GRAPH_TYPE_MAX] = {
    "#ff0000",
    "#0080ff",
    "#00ff00",
    "#00ff00",
};

/**
 * Mutex used to provide reentrant API for network graph updates.
 */
static SDL_Mutex *network_graph_mutex = NULL;

/**
 * The work queue.
 */
static network_graph_work_t *work_queue = NULL;

/* Prototypes */
static void
widget_network_graph_update(widgetdata *widget, int type, int traffic, uint64_t value, bool sample);

static size_t network_graph_series_count(int type) {
    return type == NETWORK_GRAPH_TYPE_LATENCY ? 1 : NETWORK_GRAPH_TRAFFIC_MAX;
}

/** @copydoc widgetdata::draw_func */
static void widget_draw(widgetdata *widget) {
    if (!widget->redraw) {
        return;
    }

    network_graph_widget_t *network_graph = widget->subwidget;
    network_graph_data_t *data = &network_graph->data[network_graph->type];

    SDL_FillSurfaceRect(widget->surface, NULL, 0);

    if (data->series.values == NULL) {
        return;
    }

    if (network_graph->type == NETWORK_GRAPH_TYPE_LATENCY) {
        SDL_Color color;
        if (!text_color_parse(network_graph_colors[NETWORK_GRAPH_TYPE_LATENCY], &color)) {
            LOG(ERROR,
                "Could not parse color: %s",
                network_graph_colors[NETWORK_GRAPH_TYPE_LATENCY]);
            return;
        }

        int previous_x = -1;
        int previous_y = widget->h - 1;
        for (size_t x = 0; x < data->series.pos && x < (size_t)widget->w; x++) {
            if (!network_graph_series_is_valid(&data->series, x)) {
                previous_x = -1;
                continue;
            }

            uint64_t value = network_graph_series_value(&data->series, x, 0);
            long double factor = data->series.max == 0
                                     ? 0.0L
                                     : value / (long double)data->series.max;
            int y = (widget->h - 1) - (widget->h - 1) * factor;

            if (previous_x >= 0) {
                lineRGBA(widget->surface,
                         previous_x,
                         previous_y,
                         (int)x,
                         y,
                         color.r,
                         color.g,
                         color.b,
                         255);
            } else {
                lineRGBA(widget->surface,
                         (int)x,
                         y,
                         (int)x,
                         y,
                         color.r,
                         color.g,
                         color.b,
                         255);
            }
            previous_x = (int)x;
            previous_y = y;
        }
        return;
    }

    for (int i = 0; i < NETWORK_GRAPH_TRAFFIC_MAX; i++) {
        if (!BIT_QUERY(network_graph->filters, i)) {
            continue;
        }

        SDL_Color color;
        if (!text_color_parse(network_graph_colors[i], &color)) {
            LOG(ERROR, "Could not parse color: %s", network_graph_colors[i]);
            continue;
        }

        int ly = widget->h - 1;
        for (size_t x = 0; x < data->series.pos && x < (size_t)widget->w; x++) {
            uint64_t bytes = network_graph_series_value(&data->series, x, i);
            long double factor;
            if (data->series.max == 0) {
                factor = 0.0;
            } else {
                factor = bytes / (long double)data->series.max;
            }
            int y = (widget->h - 1) - (widget->h - 1) * factor;

            lineRGBA(widget->surface,
                     MAX(0, (int)x - 1),
                     ly,
                     (int)x,
                     y,
                     color.r,
                     color.g,
                     color.b,
                     255);
            ly = y;
        }
    }
}

/** @copydoc widgetdata::background_func */
static void widget_background(widgetdata *widget, int draw) {
    if (!widget->show) {
        if (network_graph_mutex != NULL) {
            SDL_DestroyMutex(network_graph_mutex);
            network_graph_mutex = NULL;
        }

        return;
    }

    if (network_graph_mutex == NULL) {
        network_graph_mutex = SDL_CreateMutex();
    }

    network_graph_widget_t *network_graph = widget->subwidget;

    SDL_LockMutex(network_graph_mutex);
    while (work_queue != NULL) {
        network_graph_work_t *work = work_queue;

        for (widgetdata *tmp = cur_widget[NETWORK_GRAPH_ID]; tmp != NULL; tmp = tmp->type_next) {
            widget_network_graph_update(tmp,
                                        work->type,
                                        work->traffic,
                                        work->value,
                                        work->sample);
        }

        LL_DELETE(work_queue, work);
        free(work);
    }
    SDL_UnlockMutex(network_graph_mutex);

    for (int type = 0; type < NETWORK_GRAPH_TYPE_MAX; type++) {
        network_graph_data_t *data = &network_graph->data[type];
        if (LastTick - data->ticks <= 1000) {
            continue;
        }

        if (type == NETWORK_GRAPH_TYPE_LATENCY) {
            widget_network_graph_update(widget, type, 0, 0, false);
            continue;
        }

        for (int traffic = 0; traffic < NETWORK_GRAPH_TRAFFIC_MAX; traffic++) {
            widget_network_graph_update(widget, type, traffic, 0, true);
        }
    }
}

/** @copydoc widgetdata::event_func */
static int widget_event(widgetdata *widget, SDL_Event *event) {
    network_graph_widget_t *network_graph = widget->subwidget;
    network_graph_data_t *data = &network_graph->data[network_graph->type];

    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        int x = event_mouse_x(event) - widget->x;
        if (x < 0 || x >= widget->w) {
            return 0;
        }

        if (data->series.values == NULL || (size_t)x >= data->series.width) {
            return 0;
        }

        char buf[HUGE_BUF];
        if (network_graph->type == NETWORK_GRAPH_TYPE_LATENCY) {
            snprintf(VS(buf),
                     "Maximum: %.3Lf ms",
                     data->series.max / 1000.0L);
            if (network_graph_series_is_valid(&data->series, (size_t)x)) {
                snprintfcat(VS(buf),
                            "\nApplication RTT: %.3Lf ms",
                            network_graph_series_value(&data->series, (size_t)x, 0) / 1000.0L);
            } else {
                snprintfcat(VS(buf), "\nApplication RTT: no sample");
            }
        } else {
            snprintf(VS(buf),
                     "Maximum: %" PRIu64 " Bytes/s (%" PRIu64 " kB/s)",
                     data->series.max,
                     data->series.max / 1000);

            for (int i = 0; i < NETWORK_GRAPH_TRAFFIC_MAX; i++) {
                uint64_t bytes = network_graph_series_value(&data->series, (size_t)x, i);
                snprintfcat(VS(buf),
                            "\n%s: %" PRIu64 " Bytes/s (%" PRIu64 " kB/s)",
                            network_graph_filters[i],
                            bytes,
                            bytes / 1000);
            }
        }

        tooltip_create(event_mouse_x(event), event_mouse_y(event), FONT_ARIAL11, buf);
        tooltip_multiline(200);
        tooltip_enable_delay(100);
    }

    return 0;
}

/** @copydoc widgetdata::deinit_func */
static void widget_deinit(widgetdata *widget) {
    network_graph_widget_t *network_graph = widget->subwidget;

    for (int i = 0; i < NETWORK_GRAPH_TYPE_MAX; i++) {
        network_graph_series_free(&network_graph->data[i].series);
    }

    if (network_graph_mutex != NULL) {
        SDL_LockMutex(network_graph_mutex);
        while (work_queue != NULL) {
            network_graph_work_t *work = work_queue;
            LL_DELETE(work_queue, work);
            free(work);
        }
        SDL_UnlockMutex(network_graph_mutex);

        SDL_DestroyMutex(network_graph_mutex);
        network_graph_mutex = NULL;
    }
}

static void
menu_network_graph_display_change(widgetdata *widget, widgetdata *menuitem, SDL_Event *event) {
    network_graph_widget_t *network_graph = widget->subwidget;

    for (widgetdata *tmp = menuitem->inv; tmp != NULL; tmp = tmp->next) {
        if (tmp->type == LABEL_ID) {
            _widget_label *label = LABEL(tmp);

            for (int i = 0; i < NETWORK_GRAPH_TYPE_MAX; i++) {
                if (strcmp(network_graph_types[i], label->text) == 0) {
                    network_graph->type = i;
                    widget->redraw = 1;
                    break;
                }
            }

            break;
        }
    }
}

static void menu_network_graph_display(widgetdata *widget, widgetdata *menuitem, SDL_Event *event) {
    network_graph_widget_t *network_graph = widget->subwidget;
    widgetdata *submenu = MENU(menuitem->env)->submenu;

    for (int i = 0; i < NETWORK_GRAPH_TYPE_MAX; i++) {
        add_menuitem(submenu,
                     network_graph_types[i],
                     &menu_network_graph_display_change,
                     MENU_RADIO,
                     network_graph->type == i);
    }
}

static void
menu_network_graph_filters_change(widgetdata *widget, widgetdata *menuitem, SDL_Event *event) {
    network_graph_widget_t *network_graph = widget->subwidget;

    for (widgetdata *tmp = menuitem->inv; tmp != NULL; tmp = tmp->next) {
        if (tmp->type == LABEL_ID) {
            _widget_label *label = LABEL(tmp);

            for (int i = 0; i < NETWORK_GRAPH_TRAFFIC_MAX; i++) {
                if (strcmp(network_graph_filters[i], label->text) == 0) {
                    BIT_FLIP(network_graph->filters, i);
                    widget->redraw = 1;
                    break;
                }
            }

            break;
        }
    }
}

static void menu_network_graph_filters(widgetdata *widget, widgetdata *menuitem, SDL_Event *event) {
    network_graph_widget_t *network_graph = widget->subwidget;
    widgetdata *submenu = MENU(menuitem->env)->submenu;

    for (int i = 0; i < NETWORK_GRAPH_TRAFFIC_MAX; i++) {
        add_menuitem(submenu,
                     network_graph_filters[i],
                     &menu_network_graph_filters_change,
                     MENU_CHECKBOX,
                     BIT_QUERY(network_graph->filters, i));
    }
}

/** @copydoc widgetdata::menu_handle_func */
static int widget_menu_handle(widgetdata *widget, SDL_Event *event) {
    widgetdata *menu = create_menu(event_mouse_x(event), event_mouse_y(event), widget);
    widget_menu_standard_items(widget, menu);
    add_menuitem(menu, "Display  >", &menu_network_graph_display, MENU_SUBMENU, 0);
    add_menuitem(menu, "Filters  >", &menu_network_graph_filters, MENU_SUBMENU, 0);
    menu_finalize(menu);
    return 1;
}

/**
 * Actually performs updating for the specified network graph widget.
 * @param widget
 * The widget.
 * @param type
 * The network graph type.
 * @param traffic
 * The traffic type (tx/rx), or zero for latency.
 * @param value
 * Sample value.
 * @param sample
 * Whether to record a sample in the current bucket.
 */
static void widget_network_graph_update(widgetdata *widget,
                                        int type,
                                        int traffic,
                                        uint64_t value,
                                        bool sample) {
    HARD_ASSERT(widget != NULL);
    HARD_ASSERT(type >= 0 && type < NETWORK_GRAPH_TYPE_MAX);
    HARD_ASSERT(traffic >= 0 && (size_t)traffic < network_graph_series_count(type));

    if (!widget->show) {
        return;
    }

    network_graph_widget_t *network_graph = widget->subwidget;
    network_graph_data_t *data = &network_graph->data[type];

    if (widget->w <= 0) {
        network_graph_series_resize(&data->series, 0);
        data->ticks = LastTick;
        return;
    }

    if (data->series.width != (size_t)widget->w) {
        size_t new_width = (size_t)widget->w;

        if (data->series.width == 0) {
            data->ticks = LastTick;
        }

        if (!network_graph_series_resize(&data->series, new_width)) {
            LOG(ERROR, "Unable to resize network graph history to %zu buckets", new_width);
            return;
        }
    }

    if (data->series.width == 0) {
        return;
    }

    if (LastTick - data->ticks > 1000) {
        network_graph_series_advance(&data->series);
        data->ticks = LastTick;
    }

    if (sample) {
        network_graph_series_add(&data->series,
                                 (size_t)traffic,
                                 value,
                                 type != NETWORK_GRAPH_TYPE_LATENCY);
    }

    widget->redraw = 1;
}

/**
 * Updates all network graph widgets with new data.
 * @param type
 * The network graph type.
 * @param traffic
 * The traffic type (tx/rx).
 * @param bytes
 * Bytes.
 * @note This function is thread-safe.
 */
void network_graph_update(int type, int traffic, size_t bytes) {
    HARD_ASSERT(type >= 0 && type < NETWORK_GRAPH_TYPE_MAX);
    HARD_ASSERT(traffic >= 0 && traffic < NETWORK_GRAPH_TRAFFIC_MAX);

    if (network_graph_mutex == NULL) {
        return;
    }

    network_graph_work_t *work = xcalloc(1, sizeof(*work));
    work->type = type;
    work->traffic = traffic;
    work->value = bytes;
    work->sample = true;

    SDL_LockMutex(network_graph_mutex);
    LL_PREPEND(work_queue, work);
    SDL_UnlockMutex(network_graph_mutex);
}

/**
 * Updates all network graph widgets with a keepalive application RTT.
 * @param rtt_us
 * Matched keepalive round-trip time in microseconds.
 * @note This function is thread-safe.
 */
void network_graph_update_latency(uint64_t rtt_us) {
    if (network_graph_mutex == NULL) {
        return;
    }

    network_graph_work_t *work = xcalloc(1, sizeof(*work));
    work->type = NETWORK_GRAPH_TYPE_LATENCY;
    work->value = rtt_us;
    work->sample = true;

    SDL_LockMutex(network_graph_mutex);
    LL_PREPEND(work_queue, work);
    SDL_UnlockMutex(network_graph_mutex);
}

/**
 * Initializes one network graph widget.
 * @param widget
 * The widget.
 */
void widget_network_graph_init(widgetdata *widget) {
    network_graph_widget_t *network_graph = xcalloc(1, sizeof(*network_graph));
    network_graph->filters = ~0U;
    for (int type = 0; type < NETWORK_GRAPH_TYPE_MAX; type++) {
        network_graph_series_init(&network_graph->data[type].series,
                                  network_graph_series_count(type));
    }

    widget->draw_func = widget_draw;
    widget->background_func = widget_background;
    widget->event_func = widget_event;
    widget->deinit_func = widget_deinit;
    widget->menu_handle_func = widget_menu_handle;
    widget->subwidget = network_graph;
}
