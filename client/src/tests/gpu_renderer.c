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

#include <SDL3/SDL.h>
#include <gpu_map_renderer.h>
#include <gpu_renderer.h>
#include <lighting.h>
#include <settings.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <toolkit/toolkit.h>

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
void gpu_map_renderer_presentation_begin(void) {}
void gpu_map_renderer_presentation_stage(void) {}
void gpu_map_renderer_presentation_finish(bool presented) {
    (void)presented;
}
void gpu_map_renderer_set_presentation(uint64_t token, SDL_Surface *source, int x, int y) {
    (void)token;
    (void)source;
    (void)x;
    (void)y;
}
SDL_Surface *
gpu_map_renderer_presentation_source(uint64_t identity, uint32_t variant, uint64_t token) {
    (void)identity;
    (void)variant;
    (void)token;
    return NULL;
}
bool gpu_map_renderer_replay_presentation(uint64_t identity,
                                          uint32_t variant,
                                          uint64_t token,
                                          int x,
                                          int y,
                                          uint8_t alpha,
                                          uint8_t start_alpha) {
    (void)identity;
    (void)variant;
    (void)token;
    (void)x;
    (void)y;
    (void)alpha;
    (void)start_alpha;
    return true;
}
void gpu_map_renderer_poll(void) {}
bool gpu_map_renderer_wait_idle(void) {
    return true;
}
bool gpu_map_renderer_begin(int width, int height, bool auxiliary) {
    (void)width;
    (void)height;
    (void)auxiliary;
    return false;
}
bool gpu_map_renderer_retain(int width, int height, bool auxiliary) {
    (void)width;
    (void)height;
    (void)auxiliary;
    return false;
}
bool gpu_map_renderer_active(void) {
    return false;
}
void gpu_map_renderer_set_invalidation_hint(gpu_renderer_map_invalidation_reason_t reason) {
    (void)reason;
}
void gpu_map_renderer_set_owner(uint8_t owner, int sample_y, bool projected) {
    (void)owner;
    (void)sample_y;
    (void)projected;
}
void gpu_map_renderer_set_ground_coverage(bool enabled) {
    (void)enabled;
}
void gpu_map_renderer_set_instance_identity(uint64_t record_identity, uint32_t draw_variant) {
    (void)record_identity;
    (void)draw_variant;
}
void gpu_map_renderer_light_quad(uint8_t owner, const lighting_vertex_t vertices[4]) {
    (void)owner;
    (void)vertices;
}
void gpu_map_renderer_light_quad_coverage(uint8_t owner,
                                          const lighting_vertex_t vertices[4],
                                          const uint8_t coverage[9]) {
    (void)owner;
    (void)vertices;
    (void)coverage;
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
SDL_Texture *gpu_map_renderer_texture(bool auxiliary) {
    (void)auxiliary;
    return NULL;
}
void gpu_map_renderer_invalidate_surface(SDL_Surface *surface) {
    (void)surface;
}

static void test_recreation_diagnostics(void) {
    gpu_renderer_recreation_diagnostic_t diagnostic;
    memset(&diagnostic, 0xff, sizeof(diagnostic));
    HARD_ASSERT(!gpu_renderer_recreation_take_diagnostic(&diagnostic));
    HARD_ASSERT(diagnostic.origin[0] == '\0' && diagnostic.error_snapshot[0] == '\0' &&
                diagnostic.request_count == 0 && diagnostic.event_type == 0 &&
                diagnostic.window_id == 0 && diagnostic.data1 == 0 && diagnostic.data2 == 0);
    SDL_Event event = {0};
    event.type = SDL_EVENT_WINDOW_DISPLAY_CHANGED;
    event.window.windowID = 17;
    event.window.data1 = 23;
    event.window.data2 = -9;
    SDL_SetError("unrelated old SDL error");
    gpu_renderer_recreation_request_at("window_event", 12, &event, NULL);
    gpu_renderer_recreation_request_at("later_failure", 34, NULL, "later error");
    HARD_ASSERT(gpu_renderer_recreation_take_diagnostic(&diagnostic));
    HARD_ASSERT(strcmp(diagnostic.origin, "window_event") == 0 && diagnostic.line == 12 &&
                diagnostic.request_count == 2 && diagnostic.error_snapshot[0] == '\0' &&
                diagnostic.event_type == SDL_EVENT_WINDOW_DISPLAY_CHANGED &&
                diagnostic.window_id == 17 && diagnostic.data1 == 23 && diagnostic.data2 == -9);
    HARD_ASSERT(!gpu_renderer_recreation_take_diagnostic(&diagnostic));
    HARD_ASSERT(diagnostic.origin[0] == '\0' && diagnostic.event_type == 0);
    char error[] = "original failure";
    gpu_renderer_recreation_request_at("gpu_failure", 56, NULL, error);
    error[0] = 'X';
    HARD_ASSERT(gpu_renderer_recreation_take_diagnostic(&diagnostic));
    HARD_ASSERT(strcmp(diagnostic.error_snapshot, "original failure") == 0 &&
                diagnostic.event_type == 0 && diagnostic.window_id == 0 &&
                diagnostic.request_count == 1);
    gpu_renderer_recreation_request_at("discarded", 78, NULL, "discarded error");
    HARD_ASSERT(gpu_renderer_recreation_take_request());
    HARD_ASSERT(!gpu_renderer_recreation_take_diagnostic(&diagnostic));
    HARD_ASSERT(diagnostic.origin[0] == '\0' && diagnostic.error_snapshot[0] == '\0');
    event.type = SDL_EVENT_DID_ENTER_FOREGROUND;
    gpu_renderer_recreation_request_at("foreground", 90, &event, NULL);
    HARD_ASSERT(gpu_renderer_recreation_take_diagnostic(&diagnostic));
    HARD_ASSERT(diagnostic.event_type == SDL_EVENT_DID_ENTER_FOREGROUND &&
                diagnostic.window_id == 0 && diagnostic.data1 == 0 && diagnostic.data2 == 0);
    char long_text[512];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    gpu_renderer_recreation_request_at(long_text, 1, NULL, long_text);
    HARD_ASSERT(gpu_renderer_recreation_take_diagnostic(&diagnostic));
    HARD_ASSERT(strlen(diagnostic.origin) == sizeof(diagnostic.origin) - 1 &&
                strlen(diagnostic.error_snapshot) == sizeof(diagnostic.error_snapshot) - 1);
    SDL_ClearError();
}

int main(void) {
    test_recreation_diagnostics();
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
    gpu_renderer_statistics_source_upload(1024);
    gpu_renderer_statistics_instance_upload(2048);
    gpu_renderer_statistics_light_upload(1024);
    gpu_renderer_statistics_slot_uniform_upload(256);
    gpu_renderer_statistics_resource_create(8192);
    gpu_renderer_statistics_resource_destroy(8192);
    gpu_renderer_statistics_recovery(true);
    gpu_renderer_statistics_recovery(false);
    gpu_renderer_statistics_map_submission(2);
    gpu_renderer_statistics_map_completion(100, 200);
    gpu_renderer_statistics_map_update(true, false);
    uint64_t started = gpu_renderer_timing_begin();
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_COMMAND_BUILD, started);
    gpu_renderer_statistics_get(&statistics);

    HARD_ASSERT(statistics.commands == 17);
    HARD_ASSERT(statistics.batches == 5);
    HARD_ASSERT(statistics.draws == 7);
    HARD_ASSERT(statistics.upload_count == 4);
    HARD_ASSERT(statistics.upload_bytes == 4352);
    HARD_ASSERT(statistics.source_upload_count == 1);
    HARD_ASSERT(statistics.source_upload_bytes == 1024);
    HARD_ASSERT(statistics.instance_upload_count == 1);
    HARD_ASSERT(statistics.instance_upload_bytes == 2048);
    HARD_ASSERT(statistics.light_upload_count == 1);
    HARD_ASSERT(statistics.light_upload_bytes == 1024);
    HARD_ASSERT(statistics.slot_uniform_upload_count == 1);
    HARD_ASSERT(statistics.slot_uniform_upload_bytes == 256);
    HARD_ASSERT(statistics.resource_creations == 1);
    HARD_ASSERT(statistics.resource_destructions == 1);
    HARD_ASSERT(statistics.retained_bytes == 0);
    HARD_ASSERT(statistics.peak_retained_bytes == 8192);
    HARD_ASSERT(statistics.device_recoveries == 2);
    HARD_ASSERT(statistics.recovery_failures == 1);
    HARD_ASSERT(statistics.fallbacks == 0);
    HARD_ASSERT(statistics.map_submissions == 1);
    HARD_ASSERT(statistics.map_completions == 1);
    HARD_ASSERT(statistics.map_in_flight_peak == 3);
    HARD_ASSERT(statistics.map_queue_depth_samples == 1);
    HARD_ASSERT(statistics.map_queue_depth_total == 2);
    HARD_ASSERT(statistics.map_queue_age_total_ns == 100);
    HARD_ASSERT(statistics.map_queue_age_max_ns == 100);
    HARD_ASSERT(statistics.map_frame_latency_total_ns == 200);
    HARD_ASSERT(statistics.map_frame_latency_max_ns == 200);
    HARD_ASSERT(statistics.map_dropped_updates == 1);
    HARD_ASSERT(statistics.map_merged_updates == 0);
    HARD_ASSERT(statistics.timings[GPU_RENDERER_TIMING_COMMAND_BUILD].calls == 1);

    gpu_renderer_destroy();
    return 0;
}
