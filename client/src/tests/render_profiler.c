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
#include <toolkit/stringbuffer.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

char *render_profiler_widget_text_for_test(const render_profile_snapshot_t *snapshot);
void render_profiler_set_completed_generation_for_test(uint32_t generation);

void gpu_renderer_statistics_get(gpu_renderer_statistics_t *statistics) {
    *statistics = (gpu_renderer_statistics_t){
        .timings =
            {
                [GPU_RENDERER_TIMING_COMMAND_BUILD] = {.calls = 2, .elapsed_ns = UINT64_C(4000000)},
            },
        .commands = 101,
        .batches = 23,
        .draws = 47,
        .upload_count = 7,
        .upload_bytes = UINT64_C(2097152),
        .resource_creations = 13,
        .resource_destructions = 5,
        .retained_bytes = UINT64_C(3145728),
        .peak_retained_bytes = UINT64_C(4194304),
        .device_recoveries = 2,
        .recovery_failures = 1,
    };
}

/* The formatter test does not execute the widget's SDL/scrollbar paths. */
Uint32
pixel_format_map_rgba(SDL_PixelFormat format, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
    (void)format;
    (void)red;
    (void)green;
    (void)blue;
    (void)alpha;
    return 0;
}

bool surface_fill_rect(SDL_Surface *surface, const SDL_Rect *rectangle, Uint32 color) {
    return SDL_FillSurfaceRect(surface, rectangle, color);
}

font_struct *font_get_weak(const char *name, uint8_t size) {
    (void)name;
    (void)size;
    return NULL;
}

void text_show(SDL_Surface *surface,
               font_struct *font,
               const char *text,
               int x,
               int y,
               const char *color_notation,
               uint64_t flags,
               SDL_Rect *box) {
    (void)surface;
    (void)font;
    (void)text;
    (void)x;
    (void)y;
    (void)color_notation;
    if ((flags & TEXT_LINES_CALC) != 0) {
        box->y = 500;
    }
}

void scrollbar_create(scrollbar_struct *scrollbar,
                      int width,
                      int height,
                      uint32_t *scroll_offset,
                      uint32_t *num_lines,
                      uint32_t max_lines) {
    (void)scrollbar;
    (void)width;
    (void)height;
    (void)scroll_offset;
    (void)num_lines;
    (void)max_lines;
}

void scrollbar_info_create(scrollbar_info_struct *info) {
    (void)info;
}

void scrollbar_scroll_to(scrollbar_struct *scrollbar, int scroll) {
    (void)scrollbar;
    (void)scroll;
}

void scrollbar_scroll_adjust(scrollbar_struct *scrollbar, int adjust) {
    (void)scrollbar;
    (void)adjust;
}

void scrollbar_show(scrollbar_struct *scrollbar, SDL_Surface *surface, int x, int y) {
    (void)scrollbar;
    (void)surface;
    (void)x;
    (void)y;
}

int scrollbar_event(scrollbar_struct *scrollbar, SDL_Event *event) {
    (void)scrollbar;
    (void)event;
    return 0;
}

static void test_stage_metadata_is_rendered(void) {
    render_profile_snapshot_t snapshot = {
        .interval_us = UINT64_C(2000000),
        .frames = 20,
        .drawn_frames = 18,
    };

    for (render_profile_stage_t stage = 0; stage < RENDER_PROFILE_STAGE_NUM; stage++) {
        snapshot.elapsed_us[stage] = (stage + 1) * UINT64_C(1000);
        snapshot.calls[stage] = stage + 1;
    }

    char *text = render_profiler_widget_text_for_test(&snapshot);
    TEST_CHECK(strstr(text, "[c=#ffd060]Stage breakdown[/c]") != NULL);
    TEST_CHECK(strstr(text, "commands 101  batches 23  draws 47") != NULL);
    TEST_CHECK(strstr(text, "command  2.00") != NULL);

    for (render_profile_stage_t stage = 0; stage < RENDER_PROFILE_STAGE_NUM; stage++) {
        render_profile_stage_metadata_t metadata = {0};
        TEST_CHECK(render_profiler_stage_metadata_get(stage, &metadata));
        TEST_CHECK(strstr(text, metadata.name) != NULL);
        TEST_CHECK(strstr(text, render_profiler_scope_name(metadata.scope)) != NULL);
    }

    free(text);
}

static void test_zero_interval_and_calls_are_safe(void) {
    render_profile_snapshot_t snapshot = {0};
    char *text = render_profiler_widget_text_for_test(&snapshot);

    TEST_CHECK(strstr(text, "frame &lsqb;per_frame&rsqb; 0.00 ms, 0 calls, 0.0/s") != NULL);
    TEST_CHECK(render_profiler_scope_name((render_profile_scope_t)-1) == NULL);
    TEST_CHECK(!render_profiler_stage_metadata_get(-1, NULL));
    TEST_CHECK(!render_profiler_stage_metadata_get(RENDER_PROFILE_STAGE_NUM, NULL));
    TEST_CHECK(!render_profiler_stage_metadata_get(RENDER_PROFILE_FRAME, NULL));
    free(text);
}

static void test_widget_navigation_and_resize(void) {
    SDL_Surface *surface = SDL_CreateSurface(300, 145, SDL_PIXELFORMAT_RGBA32);
    widgetdata widget = {
        .w = 300,
        .h = 145,
        .redraw = 1,
        .surface = surface,
    };
    SDL_Event event = {0};

    TEST_CHECK(surface != NULL);
    widget_render_profiler_init(&widget);
    widget.draw_func(&widget);

    widget.h = 200;
    widget.redraw = 1;
    widget.draw_func(&widget);

    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.y = 1;
    TEST_CHECK(widget.event_func(&widget, &event) == 1);
    event.wheel.y = -1;
    TEST_CHECK(widget.event_func(&widget, &event) == 1);

    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_PAGEUP;
    TEST_CHECK(widget.event_func(&widget, &event) == 1);
    event.key.key = SDLK_PAGEDOWN;
    TEST_CHECK(widget.event_func(&widget, &event) == 1);

    render_profiler_set_completed_generation_for_test(1);
    widget.redraw = 0;
    widget.background_func(&widget, 0);
    TEST_CHECK(widget.redraw == 1);
    TEST_CHECK(widget.deinit_func == NULL);
    free(widget.subwidget);
    SDL_DestroySurface(surface);
}

int main(void) {
    toolkit_import(stringbuffer);
    test_stage_metadata_is_rendered();
    test_zero_interval_and_calls_are_safe();
    test_widget_navigation_and_resize();
    toolkit_deinit();
    return 0;
}
