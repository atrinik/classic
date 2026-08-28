/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Production lighting state hooks for the mandatory GPU renderer.
 *
 * The dense CPU field rasterizer and compositor are compiled only into
 * test-enabled oracle builds. Production retains compact Q5.11 samples in
 * map state and submits them directly to gpu_map_renderer.c.
 */

#include <global.h>

static int selected_depth;

bool lighting_begin(int width, int height, uint64_t cache_key) {
    (void)width;
    (void)height;
    (void)cache_key;
    SDL_SetError("CPU lighting compositor is unavailable in the GPU-only client");
    return false;
}

bool lighting_select_level(int depth) {
    if (depth < -MAP2_MAX_DEPTH || depth > MAP2_MAX_DEPTH) {
        return false;
    }
    selected_depth = depth;
    return true;
}

void lighting_set_level_mask(uint16_t mask) {
    (void)mask;
    selected_depth = 0;
}

void lighting_level_scroll(int dz) {
    selected_depth = MAX(-MAP2_MAX_DEPTH, MIN(MAP2_MAX_DEPTH, selected_depth - dz));
}

bool lighting_needs_update(void) {
    return false;
}

bool lighting_viewport_size_get(int *width, int *height) {
    (void)width;
    (void)height;
    return false;
}

void lighting_dirty_screen_rect(int depth, int x0, int y0, int x1, int y1) {
    (void)depth;
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
}

void lighting_scroll(int screen_dx, int screen_dy) {
    (void)screen_dx;
    (void)screen_dy;
}

void lighting_draw_quad(const lighting_vertex_t vertices[4]) {
    (void)vertices;
    SDL_SetError("CPU lighting rasterization is unavailable in the GPU-only client");
}

void lighting_render(SDL_Surface *destination) {
    (void)destination;
    SDL_SetError("CPU lighting composition is unavailable in the GPU-only client");
}

bool lighting_scene_begin(int width, int height) {
    (void)width;
    (void)height;
    SDL_SetError("CPU scene composition is unavailable in the GPU-only client");
    return false;
}

void lighting_scene_mark_surface(SDL_Surface *source,
                                 int x,
                                 int y,
                                 SDL_Rect *srcrect,
                                 int depth,
                                 int sample_y) {
    (void)source;
    (void)x;
    (void)y;
    (void)srcrect;
    (void)depth;
    (void)sample_y;
}

void lighting_scene_render(SDL_Surface *destination) {
    (void)destination;
    SDL_SetError("CPU scene composition is unavailable in the GPU-only client");
}

void lighting_scene_cancel(void) {}

void lighting_show_surface(SDL_Surface *destination,
                           int x,
                           int y,
                           SDL_Rect *srcrect,
                           SDL_Surface *source,
                           int sample_y,
                           lighting_surface_mode_t mode) {
    (void)destination;
    (void)x;
    (void)y;
    (void)srcrect;
    (void)source;
    (void)sample_y;
    (void)mode;
    SDL_SetError("CPU lit-surface composition is unavailable in the GPU-only client");
}

void lighting_invalidate_surface(SDL_Surface *source) {
    (void)source;
}

void lighting_clear_sprite_cache(void) {}
void lighting_deinit(void) {}

void lighting_benchmark_statistics_reset(void) {}

void lighting_benchmark_statistics_get(lighting_benchmark_statistics_t *statistics) {
    HARD_ASSERT(statistics != NULL);
    memset(statistics, 0, sizeof(*statistics));
}

void lighting_benchmark_timings_get(lighting_benchmark_timings_t *timings) {
    HARD_ASSERT(timings != NULL);
    memset(timings, 0, sizeof(*timings));
}

void lighting_benchmark_configure(bool timing_enabled,
                                  lighting_benchmark_reconstruction_t reconstruction) {
    (void)timing_enabled;
    (void)reconstruction;
}

bool lighting_benchmark_level_statistics_get(
    int depth, lighting_benchmark_level_statistics_t *statistics) {
    if (depth < -MAP2_MAX_DEPTH || depth > MAP2_MAX_DEPTH || statistics == NULL) {
        return false;
    }
    memset(statistics, 0, sizeof(*statistics));
    statistics->depth = depth;
    return true;
}
