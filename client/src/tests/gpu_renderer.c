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

#include <global.h>

int64_t setting_get_int(int category, int setting) {
    (void)category;
    (void)setting;
    return ZOOM_FILTER_OFF;
}

SDL_ScaleMode zoom_filter_to_scale_mode(int zoom_filter) {
    (void)zoom_filter;
    return SDL_SCALEMODE_NEAREST;
}

bool gpu_map_renderer_create(SDL_GPUDevice *device, SDL_Renderer *renderer) {
    (void)device;
    (void)renderer;
    return true;
}

void gpu_map_renderer_destroy(void) {}
bool gpu_map_renderer_begin(int width, int height) {
    (void)width;
    (void)height;
    return false;
}
bool gpu_map_renderer_active(void) {
    return false;
}
void gpu_map_renderer_set_owner(uint8_t owner) {
    (void)owner;
}
void gpu_map_renderer_light_quad(uint8_t owner, const lighting_vertex_t vertices[4]) {
    (void)owner;
    (void)vertices;
}
bool gpu_map_renderer_draw_surface(SDL_Surface *surface,
                                   const SDL_Rect *source,
                                   const SDL_FRect *destination) {
    (void)surface;
    (void)source;
    (void)destination;
    return false;
}
bool gpu_map_renderer_draw_rect(const SDL_FRect *destination,
                                uint8_t red,
                                uint8_t green,
                                uint8_t blue,
                                uint8_t alpha,
                                bool filled) {
    (void)destination;
    (void)red;
    (void)green;
    (void)blue;
    (void)alpha;
    (void)filled;
    return false;
}
bool gpu_map_renderer_set_clip(const SDL_Rect *rectangle) {
    (void)rectangle;
    return false;
}
bool gpu_map_renderer_end(void) {
    return false;
}
SDL_Texture *gpu_map_renderer_texture(void) {
    return NULL;
}
void gpu_map_renderer_invalidate_surface(SDL_Surface *surface) {
    (void)surface;
}

int main(void) {
    gpu_renderer_statistics_t statistics;

    HARD_ASSERT(!gpu_renderer_ready());
    HARD_ASSERT(!gpu_renderer_frame_valid());
    HARD_ASSERT(strcmp(gpu_renderer_backend(), "") == 0);
    HARD_ASSERT(!gpu_renderer_recreation_take_request());
    gpu_renderer_recreation_request();
    gpu_renderer_recreation_request();
    HARD_ASSERT(gpu_renderer_recreation_take_request());
    HARD_ASSERT(!gpu_renderer_recreation_take_request());

    gpu_renderer_statistics_reset();
    gpu_renderer_statistics_commands(17, 5, 7);
    gpu_renderer_statistics_upload(4096);
    gpu_renderer_statistics_resource_create(8192);
    gpu_renderer_statistics_resource_destroy(8192);
    gpu_renderer_statistics_recovery(true);
    gpu_renderer_statistics_recovery(false);
    uint64_t started = gpu_renderer_timing_begin();
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_COMMAND_BUILD, started);
    gpu_renderer_statistics_get(&statistics);

    HARD_ASSERT(statistics.commands == 17);
    HARD_ASSERT(statistics.batches == 5);
    HARD_ASSERT(statistics.draws == 7);
    HARD_ASSERT(statistics.upload_count == 1);
    HARD_ASSERT(statistics.upload_bytes == 4096);
    HARD_ASSERT(statistics.resource_creations == 1);
    HARD_ASSERT(statistics.resource_destructions == 1);
    HARD_ASSERT(statistics.retained_bytes == 0);
    HARD_ASSERT(statistics.peak_retained_bytes == 8192);
    HARD_ASSERT(statistics.device_recoveries == 2);
    HARD_ASSERT(statistics.recovery_failures == 1);
    HARD_ASSERT(statistics.fallbacks == 0);
    HARD_ASSERT(statistics.timings[GPU_RENDERER_TIMING_COMMAND_BUILD].calls == 1);

    gpu_renderer_destroy();
    return 0;
}
