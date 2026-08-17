/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Low-overhead frame profiling and its optional display widget.
 */

#include <global.h>
#include <toolkit/datetime.h>
#include <toolkit/string.h>

static bool profiler_enabled;
static render_profile_snapshot_t accumulated;
static render_profile_snapshot_t completed;
static render_profile_snapshot_t statistics;
static uint64_t interval_started_us;
static uint64_t statistics_started_us;

typedef struct render_profiler_widget {
    uint32_t generation;
    scrollbar_struct scrollbar;
    scrollbar_info_struct scrollbar_info;
} render_profiler_widget_t;

static const render_profile_stage_metadata_t stage_metadata[RENDER_PROFILE_STAGE_NUM] = {
    [RENDER_PROFILE_FRAME] = {"frame", RENDER_PROFILE_SCOPE_FRAME},
    [RENDER_PROFILE_EVENTS] = {"events", RENDER_PROFILE_SCOPE_FRAME},
    [RENDER_PROFILE_GAME] = {"game", RENDER_PROFILE_SCOPE_FRAME},
    [RENDER_PROFILE_WIDGETS] = {"widgets", RENDER_PROFILE_SCOPE_FRAME},
    [RENDER_PROFILE_OVERLAYS] = {"overlays", RENDER_PROFILE_SCOPE_FRAME},
    [RENDER_PROFILE_MAINTENANCE] = {"maintenance", RENDER_PROFILE_SCOPE_FRAME},
    [RENDER_PROFILE_PRESENT] = {"present", RENDER_PROFILE_SCOPE_FRAME},
    [RENDER_PROFILE_WAIT] = {"wait", RENDER_PROFILE_SCOPE_FRAME},
    [RENDER_PROFILE_POINTER] = {"pointer", RENDER_PROFILE_SCOPE_FRAME},
    [RENDER_PROFILE_MAP] = {"map", RENDER_PROFILE_SCOPE_MAP_DRAW},
    [RENDER_PROFILE_MAP_SCRATCH_CLEAR] = {"map_scratch_clear", RENDER_PROFILE_SCOPE_MAP_DRAW},
    [RENDER_PROFILE_MAP_GROUND] = {"ground", RENDER_PROFILE_SCOPE_MAP_LEVEL},
    [RENDER_PROFILE_MAP_GROUND_COMPOSITE] = {"ground_composite", RENDER_PROFILE_SCOPE_MAP_DRAW},
    [RENDER_PROFILE_LIGHTING] = {"lighting", RENDER_PROFILE_SCOPE_MAP_LEVEL},
    [RENDER_PROFILE_MAP_OBJECTS] = {"objects", RENDER_PROFILE_SCOPE_MAP_LEVEL},
    [RENDER_PROFILE_MAP_PAINT] = {"paint", RENDER_PROFILE_SCOPE_MAP_DRAW},
    [RENDER_PROFILE_MAP_COMMAND_SORT] = {"command_sort", RENDER_PROFILE_SCOPE_MAP_DRAW},
    [RENDER_PROFILE_MAP_LIVING_OCCLUSION] = {"living_occlusion", RENDER_PROFILE_SCOPE_MAP_DRAW},
    [RENDER_PROFILE_MAP_SPRITE_EFFECTS] = {"sprite_effects", RENDER_PROFILE_SCOPE_MAP_DRAW},
    [RENDER_PROFILE_MAP_HINT_REPLAY] = {"hint_replay", RENDER_PROFILE_SCOPE_MAP_DRAW},
    [RENDER_PROFILE_MAP_UI] = {"ui", RENDER_PROFILE_SCOPE_MAP_DRAW},
};

_Static_assert(arraysize(stage_metadata) == RENDER_PROFILE_STAGE_NUM,
               "render profiler metadata must cover every stage");

/** Return monotonic time in microseconds. */
static uint64_t render_profiler_now(void) {
    return datetime_monotonic_us();
}

void render_profiler_set_enabled(bool enabled) {
    if (profiler_enabled == enabled) {
        return;
    }

    profiler_enabled = enabled;
    memset(&accumulated, 0, sizeof(accumulated));
    memset(&completed, 0, sizeof(completed));
    memset(&statistics, 0, sizeof(statistics));
    interval_started_us = enabled ? render_profiler_now() : 0;
    statistics_started_us = interval_started_us;
}

uint64_t render_profiler_begin(void) {
    return profiler_enabled ? render_profiler_now() : 0;
}

void render_profiler_end(render_profile_stage_t stage, uint64_t started_us) {
    if (!profiler_enabled || started_us == 0) {
        return;
    }

    HARD_ASSERT(stage >= 0 && stage < RENDER_PROFILE_STAGE_NUM);
    uint64_t elapsed = render_profiler_now() - started_us;
    accumulated.elapsed_us[stage] += elapsed;
    accumulated.calls[stage]++;
    statistics.elapsed_us[stage] += elapsed;
    statistics.calls[stage]++;
}

void render_profiler_frame_finished(bool drawn) {
    if (!profiler_enabled) {
        return;
    }

    accumulated.frames++;
    accumulated.drawn_frames += drawn;
    statistics.frames++;
    statistics.drawn_frames += drawn;

    uint64_t now = render_profiler_now();
    uint64_t elapsed = now - interval_started_us;
    if (elapsed < UINT64_C(1000000)) {
        return;
    }

    uint32_t generation = completed.generation + 1;
    completed = accumulated;
    completed.interval_us = elapsed;
    completed.generation = generation;
    memset(&accumulated, 0, sizeof(accumulated));
    interval_started_us = now;
}

const render_profile_snapshot_t *render_profiler_snapshot(void) {
    return &completed;
}

void render_profiler_statistics_reset(void) {
    memset(&statistics, 0, sizeof(statistics));
    statistics_started_us = profiler_enabled ? render_profiler_now() : 0;
}

void render_profiler_statistics_get(render_profile_snapshot_t *result) {
    HARD_ASSERT(result != NULL);
    *result = statistics;
    result->interval_us = profiler_enabled ? render_profiler_now() - statistics_started_us : 0;
}

bool render_profiler_stage_metadata_get(render_profile_stage_t stage,
                                        render_profile_stage_metadata_t *metadata) {
    if (metadata == NULL || stage < 0 || stage >= RENDER_PROFILE_STAGE_NUM) {
        return false;
    }

    *metadata = stage_metadata[stage];
    return true;
}

const char *render_profiler_scope_name(render_profile_scope_t scope) {
    switch (scope) {
        case RENDER_PROFILE_SCOPE_FRAME:
            return "per_frame";
        case RENDER_PROFILE_SCOPE_MAP_DRAW:
            return "per_map_draw";
        case RENDER_PROFILE_SCOPE_MAP_LEVEL:
            return "per_level";
        default:
            return NULL;
    }
}

static double render_profile_average_ms(const render_profile_snapshot_t *snapshot,
                                        render_profile_stage_t stage) {
    return snapshot->calls[stage] == 0
               ? 0.0
               : snapshot->elapsed_us[stage] / 1000.0 / snapshot->calls[stage];
}

static double render_profile_rate(const render_profile_snapshot_t *snapshot, uint32_t count) {
    return snapshot->interval_us == 0 ? 0.0 : count * 1000000.0 / snapshot->interval_us;
}

static char *render_profiler_widget_text(const render_profile_snapshot_t *snapshot) {
    StringBuffer *sb = stringbuffer_new();
    render_profile_stage_t stage;

    stringbuffer_append_printf(sb,
                               "[c=#ffd060]Render profiler[/c] (last %.2fs)\n"
                               "Loop %.1f fps, drawn %.1f fps\n"
                               "Frame %6.2f ms  work %6.2f  wait %6.2f\n"
                               "Events %5.2f  game %5.2f\n"
                               "Widgets %5.2f  overlays %5.2f\n"
                               "GC %8.2f  present %5.2f\n"
                               "[c=#ffd060]Map/draw[/c] %5.2f ms @ %.1f/s\n"
                               " scratch/draw %4.2f  ground/level %4.2f\n"
                               " composite/draw %4.2f  lighting/level %4.2f\n"
                               " objects/level %4.2f  paint/draw %4.2f\n"
                               " sort %4.2f  living %4.2f  sprites %4.2f  hints %4.2f\n"
                               " UI/draw %4.2f\n"
                               "[c=#ffd060]Stage breakdown[/c] (average, calls, rate)\n",
                               snapshot->interval_us / 1000000.0,
                               render_profile_rate(snapshot, snapshot->frames),
                               render_profile_rate(snapshot, snapshot->drawn_frames),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_FRAME),
                               MAX(0.0,
                                   render_profile_average_ms(snapshot, RENDER_PROFILE_FRAME) -
                                       render_profile_average_ms(snapshot, RENDER_PROFILE_WAIT)),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_WAIT),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_EVENTS),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_GAME),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_WIDGETS),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_OVERLAYS),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_MAINTENANCE),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_PRESENT),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_MAP),
                               render_profile_rate(snapshot, snapshot->calls[RENDER_PROFILE_MAP]),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_MAP_SCRATCH_CLEAR),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_MAP_GROUND),
                               render_profile_average_ms(snapshot,
                                                         RENDER_PROFILE_MAP_GROUND_COMPOSITE),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_LIGHTING),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_MAP_OBJECTS),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_MAP_PAINT),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_MAP_COMMAND_SORT),
                               render_profile_average_ms(snapshot,
                                                         RENDER_PROFILE_MAP_LIVING_OCCLUSION),
                               render_profile_average_ms(snapshot,
                                                         RENDER_PROFILE_MAP_SPRITE_EFFECTS),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_MAP_HINT_REPLAY),
                               render_profile_average_ms(snapshot, RENDER_PROFILE_MAP_UI));

    for (stage = 0; stage < RENDER_PROFILE_STAGE_NUM; stage++) {
        render_profile_stage_metadata_t metadata = {0};
        const char *scope;

        if (!render_profiler_stage_metadata_get(stage, &metadata)) {
            continue;
        }
        scope = render_profiler_scope_name(metadata.scope);
        HARD_ASSERT(scope != NULL);
        stringbuffer_append_printf(sb,
                                   "%s [%s] %.2f ms, %" PRIu32 " calls, %.1f/s\n",
                                   metadata.name,
                                   scope,
                                   render_profile_average_ms(snapshot, stage),
                                   snapshot->calls[stage],
                                   render_profile_rate(snapshot, snapshot->calls[stage]));
    }

    return stringbuffer_finish(sb);
}

#ifdef ATRINIK_RENDER_PROFILER_TESTING
char *render_profiler_widget_text_for_test(const render_profile_snapshot_t *snapshot) {
    return render_profiler_widget_text(snapshot);
}

void render_profiler_set_completed_generation_for_test(uint32_t generation) {
    completed.generation = generation;
}
#endif

/** @copydoc widgetdata::draw_func */
static void widget_draw(widgetdata *widget) {
    render_profiler_widget_t *profiler_widget = widget->subwidget;
    char *text;
    SDL_Rect box;
    int content_width;
    int content_height;
    int scrollbar_width = 11;

    if (!widget->redraw) {
        return;
    }

    const render_profile_snapshot_t *snapshot = render_profiler_snapshot();
    text = render_profiler_widget_text(snapshot);
    SDL_FillSurfaceRect(widget->surface,
                        NULL,
                        pixel_format_map_rgba(widget->surface->format, 0, 0, 0, SDL_ALPHA_OPAQUE));

    content_width = MAX(1, widget->w - 10 - scrollbar_width);
    content_height = MAX(1, widget->h - 8);
    box = (SDL_Rect){.x = 0, .y = 0, .w = content_width, .h = content_height};
    text_show(NULL,
              FONT_ARIAL10,
              text,
              0,
              0,
              COLOR_WHITE,
              TEXT_MARKUP | TEXT_WORD_WRAP | TEXT_LINES_CALC,
              &box);

    if (profiler_widget->scrollbar.background.w != scrollbar_width ||
        profiler_widget->scrollbar.background.h != content_height) {
        scrollbar_create(&profiler_widget->scrollbar,
                         scrollbar_width,
                         content_height,
                         &profiler_widget->scrollbar_info.scroll_offset,
                         &profiler_widget->scrollbar_info.num_lines,
                         MAX(1, box.y));
        profiler_widget->scrollbar.redraw = &profiler_widget->scrollbar_info.redraw;
    }

    profiler_widget->scrollbar.max_lines = MAX(1, box.y);
    profiler_widget->scrollbar_info.num_lines = box.h;
    scrollbar_scroll_to(&profiler_widget->scrollbar,
                        profiler_widget->scrollbar_info.scroll_offset);

    box.x = 5;
    box.y = profiler_widget->scrollbar_info.scroll_offset;
    box.w = content_width;
    box.h = content_height;
    text_show(widget->surface,
              FONT_ARIAL10,
              text,
              box.x,
              4,
              COLOR_WHITE,
              TEXT_MARKUP | TEXT_WORD_WRAP | TEXT_LINES_SKIP,
              &box);

    profiler_widget->scrollbar.px = widget->x;
    profiler_widget->scrollbar.py = widget->y;
    scrollbar_show(&profiler_widget->scrollbar,
                   widget->surface,
                   widget->w - 5 - scrollbar_width,
                   4);
    profiler_widget->scrollbar_info.redraw = 0;
    free(text);
}

/** @copydoc widgetdata::event_func */
static int widget_event(widgetdata *widget, SDL_Event *event) {
    render_profiler_widget_t *profiler_widget = widget->subwidget;

    if (scrollbar_event(&profiler_widget->scrollbar, event)) {
        WIDGET_REDRAW(widget);
        return 1;
    }

    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        if (event_wheel_y(event) > 0.0f) {
            scrollbar_scroll_adjust(&profiler_widget->scrollbar, -1);
            WIDGET_REDRAW(widget);
            return 1;
        } else if (event_wheel_y(event) < 0.0f) {
            scrollbar_scroll_adjust(&profiler_widget->scrollbar, 1);
            WIDGET_REDRAW(widget);
            return 1;
        }
    }

    if (event->type == SDL_EVENT_KEY_DOWN) {
        if (event->key.key == SDLK_PAGEUP) {
            scrollbar_scroll_adjust(&profiler_widget->scrollbar,
                                     -(int)profiler_widget->scrollbar.max_lines);
            WIDGET_REDRAW(widget);
            return 1;
        } else if (event->key.key == SDLK_PAGEDOWN) {
            scrollbar_scroll_adjust(&profiler_widget->scrollbar,
                                     profiler_widget->scrollbar.max_lines);
            WIDGET_REDRAW(widget);
            return 1;
        }
    }

    return 0;
}

/** @copydoc widgetdata::background_func */
static void widget_background(widgetdata *widget, int draw) {
    const render_profile_snapshot_t *snapshot = render_profiler_snapshot();
    render_profiler_widget_t *profiler_widget = widget->subwidget;

    (void)draw;

    if (profiler_widget->generation != snapshot->generation) {
        profiler_widget->generation = snapshot->generation;
        widget->redraw = 1;
    }

    if (profiler_widget->scrollbar_info.redraw) {
        widget->redraw = 1;
    }
}

/** @copydoc widgetdata::deinit_func */
static void widget_deinit(widgetdata *widget) {
    free(widget->subwidget);
}

void widget_render_profiler_init(widgetdata *widget) {
    render_profiler_widget_t *profiler_widget = xcalloc(1, sizeof(*profiler_widget));

    widget->draw_func = widget_draw;
    widget->background_func = widget_background;
    widget->event_func = widget_event;
    widget->deinit_func = widget_deinit;
    scrollbar_info_create(&profiler_widget->scrollbar_info);
    scrollbar_create(&profiler_widget->scrollbar,
                     11,
                     MAX(1, widget->h - 8),
                     &profiler_widget->scrollbar_info.scroll_offset,
                     &profiler_widget->scrollbar_info.num_lines,
                     1);
    profiler_widget->scrollbar.redraw = &profiler_widget->scrollbar_info.redraw;
    widget->subwidget = profiler_widget;
}
