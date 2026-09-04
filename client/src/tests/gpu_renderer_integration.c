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

#include <gpu_map_renderer.h>
#include <gpu_renderer.h>
#include <lighting.h>
#include <settings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <surface_primitives.h>
#include <toolkit/memory.h>
#include <toolkit/socket.h>
#include <toolkit/toolkit.h>
#include <openssl/evp.h>

#define GPU_CONFORMANCE_SKIP 77
typedef struct async_readback_result {
    bool called;
    bool canceled;
    SDL_Surface *surface;
} async_readback_result_t;

static void async_readback_complete(SDL_Surface *surface, void *userdata) {
    async_readback_result_t *result = userdata;
    result->called = true;
    result->surface = surface;
}

static void async_readback_cancel(void *userdata) {
    async_readback_result_t *result = userdata;
    result->canceled = true;
}

int64_t setting_get_int(int category, int setting) {
    (void)category;
    (void)setting;
    return ZOOM_FILTER_OFF;
}

SDL_ScaleMode zoom_filter_to_scale_mode(int zoom_filter) {
    (void)zoom_filter;
    return SDL_SCALEMODE_NEAREST;
}

static bool conformance_required(void) {
    const char *value =
        SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "ATRINIK_GPU_CONFORMANCE_REQUIRED");
    return value != NULL && strcmp(value, "1") == 0;
}

static int conformance_unavailable(const char *stage) {
    fprintf(stderr, "GPU conformance unavailable at %s: %s\n", stage, SDL_GetError());
    return conformance_required() ? EXIT_FAILURE : GPU_CONFORMANCE_SKIP;
}

static bool surface_pixel_is(SDL_Surface *surface,
                             int x,
                             int y,
                             Uint8 expected_red,
                             Uint8 expected_green,
                             Uint8 expected_blue) {
    Uint8 red, green, blue, alpha;
    return SDL_ReadSurfacePixel(surface, x, y, &red, &green, &blue, &alpha) &&
           red == expected_red && green == expected_green && blue == expected_blue &&
           alpha == SDL_ALPHA_OPAQUE;
}

static bool surface_channel_is_near(Uint8 actual, Uint8 expected, Uint8 tolerance) {
    unsigned int difference = actual > expected ? actual - expected : expected - actual;
    return difference <= tolerance;
}

static bool surface_pixel_is_near(SDL_Surface *surface,
                                  int x,
                                  int y,
                                  Uint8 expected_red,
                                  Uint8 expected_green,
                                  Uint8 expected_blue,
                                  Uint8 expected_alpha,
                                  Uint8 tolerance) {
    Uint8 red, green, blue, alpha;
    return SDL_ReadSurfacePixel(surface, x, y, &red, &green, &blue, &alpha) &&
           surface_channel_is_near(red, expected_red, tolerance) &&
           surface_channel_is_near(green, expected_green, tolerance) &&
           surface_channel_is_near(blue, expected_blue, tolerance) &&
           surface_channel_is_near(alpha, expected_alpha, tolerance);
}

static Uint8 alpha_blend_channel(Uint8 source, Uint8 destination, Uint8 alpha) {
    unsigned int numerator =
        (unsigned int)source * alpha + (unsigned int)destination * (SDL_ALPHA_OPAQUE - alpha);
    return (Uint8)((numerator + SDL_ALPHA_OPAQUE / 2U) / SDL_ALPHA_OPAQUE);
}

static SDL_Surface *gpu_keyed_surface(SDL_PixelFormat format, int width) {
    SDL_Surface *surface = SDL_CreateSurface(width, 1, format);
    if (surface == NULL) {
        return NULL;
    }
    Uint32 color_key = SDL_MapSurfaceRGB(surface, 0, 0, 0);
    SDL_Rect opaque = {1, 0, 1, 1};
    SDL_Rect second_opaque = {2, 0, 1, 1};
    bool success =
        SDL_FillSurfaceRect(surface, NULL, color_key) &&
        SDL_SetSurfaceColorKey(surface, true, color_key) &&
        SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE) &&
        SDL_FillSurfaceRect(surface, &opaque, SDL_MapSurfaceRGB(surface, 220, 40, 10)) &&
        SDL_FillSurfaceRect(surface, &second_opaque, SDL_MapSurfaceRGB(surface, 10, 200, 80));
    if (!success) {
        SDL_DestroySurface(surface);
        return NULL;
    }
    return surface;
}

static SDL_Surface *gpu_indexed_alpha_surface(int width) {
    SDL_Surface *surface = SDL_CreateSurface(width, 1, SDL_PIXELFORMAT_INDEX8);
    if (surface == NULL) {
        return NULL;
    }
    SDL_Color colors[3] = {
        {0, 0, 0, SDL_ALPHA_TRANSPARENT},
        {220, 40, 10, SDL_ALPHA_OPAQUE},
        {10, 200, 80, 128},
    };
    SDL_Palette *palette = SDL_CreatePalette(256);
    SDL_Rect opaque = {1, 0, 1, 1};
    SDL_Rect partial = {2, 0, 1, 1};
    bool success = palette != NULL;
    if (success) {
        success = SDL_SetPaletteColors(palette, colors, 0, SDL_arraysize(colors)) &&
                  SDL_SetSurfacePalette(surface, palette);
    }
    SDL_DestroyPalette(palette);
    success = success && SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE) &&
              SDL_FillSurfaceRect(surface, NULL, 0) && SDL_FillSurfaceRect(surface, &opaque, 1) &&
              SDL_FillSurfaceRect(surface, &partial, 2);
    if (!success) {
        SDL_DestroySurface(surface);
        return NULL;
    }
    return surface;
}

static SDL_Surface *gpu_opaque_rgb_surface(int width) {
    SDL_Surface *surface = SDL_CreateSurface(width, 1, SDL_PIXELFORMAT_RGB24);
    if (surface == NULL) {
        return NULL;
    }
    SDL_Rect opaque = {1, 0, 1, 1};
    SDL_Rect second_opaque = {2, 0, 1, 1};
    bool success =
        SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE) &&
        SDL_FillSurfaceRect(surface, NULL, SDL_MapSurfaceRGB(surface, 80, 30, 220)) &&
        SDL_FillSurfaceRect(surface, &opaque, SDL_MapSurfaceRGB(surface, 220, 40, 10)) &&
        SDL_FillSurfaceRect(surface, &second_opaque, SDL_MapSurfaceRGB(surface, 10, 200, 80));
    if (!success) {
        SDL_DestroySurface(surface);
        return NULL;
    }
    return surface;
}

static SDL_Surface *gpu_opaque_indexed_surface(int width) {
    SDL_Surface *surface = SDL_CreateSurface(width, 1, SDL_PIXELFORMAT_INDEX8);
    if (surface == NULL) {
        return NULL;
    }
    SDL_Color colors[256];
    for (size_t index = 0; index < SDL_arraysize(colors); index++) {
        colors[index] = (SDL_Color){255, 255, 255, SDL_ALPHA_OPAQUE};
    }
    colors[0] = (SDL_Color){80, 30, 220, SDL_ALPHA_OPAQUE};
    colors[1] = (SDL_Color){220, 40, 10, SDL_ALPHA_OPAQUE};
    colors[2] = (SDL_Color){10, 200, 80, SDL_ALPHA_OPAQUE};
    SDL_Palette *palette = SDL_CreatePalette(SDL_arraysize(colors));
    SDL_Rect opaque = {1, 0, 1, 1};
    SDL_Rect second_opaque = {2, 0, 1, 1};
    bool success = palette != NULL;
    if (success) {
        success = SDL_SetPaletteColors(palette, colors, 0, SDL_arraysize(colors)) &&
                  SDL_SetSurfacePalette(surface, palette);
    }
    SDL_DestroyPalette(palette);
    success = success && SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE) &&
              SDL_FillSurfaceRect(surface, NULL, 0) && SDL_FillSurfaceRect(surface, &opaque, 1) &&
              SDL_FillSurfaceRect(surface, &second_opaque, 2);
    if (!success) {
        SDL_DestroySurface(surface);
        return NULL;
    }
    return surface;
}

static bool gpu_transparency_checkpoint(SDL_Surface *source,
                                        const SDL_Rect *source_rectangle,
                                        const char *label,
                                        bool render_to_canvas,
                                        bool first_pixel_transparent,
                                        bool partial_alpha,
                                        bool prime_shared_blend) {
    const Uint8 background_red = 17;
    const Uint8 background_green = 47;
    const Uint8 background_blue = 83;
    const SDL_FRect destination = {1.0f, 1.0f, 3.0f, 1.0f};
    const SDL_FRect background = {0.0f, 0.0f, 8.0f, 4.0f};
    const SDL_FRect canvas_destination = {0.0f, 0.0f, 8.0f, 4.0f};
    const SDL_FRect prime_destination = {0.0f, 0.0f, 1.0f, 1.0f};
    SDL_Surface *canvas = NULL;
    SDL_Surface *priming = NULL;
    bool success = gpu_renderer_begin_frame();
    if (prime_shared_blend) {
        priming = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
        success = success && priming != NULL &&
                  SDL_FillSurfaceRect(priming, NULL, SDL_MapSurfaceRGBA(priming, 1, 2, 3, 255)) &&
                  SDL_SetSurfaceBlendMode(priming, SDL_BLENDMODE_ADD);
    }
    if (render_to_canvas) {
        canvas = SDL_CreateSurface(8, 4, SDL_PIXELFORMAT_RGBA32);
        success = success && canvas != NULL && gpu_renderer_canvas_register(&canvas) &&
                  gpu_renderer_canvas_fill(canvas,
                                           NULL,
                                           background_red,
                                           background_green,
                                           background_blue,
                                           SDL_ALPHA_OPAQUE) &&
                  (!prime_shared_blend ||
                   gpu_renderer_draw_surface_to(canvas, priming, NULL, &prime_destination)) &&
                  gpu_renderer_draw_surface_to(canvas, source, source_rectangle, &destination) &&
                  gpu_renderer_draw_surface(canvas, NULL, &canvas_destination);
    } else {
        success = success &&
                  gpu_renderer_draw_rect(&background,
                                         background_red,
                                         background_green,
                                         background_blue,
                                         SDL_ALPHA_OPAQUE,
                                         true) &&
                  (!prime_shared_blend ||
                   gpu_renderer_draw_surface_to(NULL, priming, NULL, &prime_destination)) &&
                  gpu_renderer_draw_surface_to(NULL, source, source_rectangle, &destination);
    }
    success = success && gpu_renderer_present();

    SDL_Surface *checkpoint = success ? gpu_renderer_readback(NULL) : NULL;
    Uint8 partial_red = partial_alpha ? alpha_blend_channel(10, background_red, 128) : 10;
    Uint8 partial_green = partial_alpha ? alpha_blend_channel(200, background_green, 128) : 200;
    Uint8 partial_blue = partial_alpha ? alpha_blend_channel(80, background_blue, 128) : 80;
    Uint8 first_red = first_pixel_transparent ? background_red : 80;
    Uint8 first_green = first_pixel_transparent ? background_green : 30;
    Uint8 first_blue = first_pixel_transparent ? background_blue : 220;
    bool valid = checkpoint != NULL &&
                 surface_pixel_is_near(checkpoint,
                                       1,
                                       1,
                                       first_red,
                                       first_green,
                                       first_blue,
                                       SDL_ALPHA_OPAQUE,
                                       1) &&
                 surface_pixel_is_near(checkpoint, 2, 1, 220, 40, 10, SDL_ALPHA_OPAQUE, 1) &&
                 surface_pixel_is_near(checkpoint,
                                       3,
                                       1,
                                       partial_red,
                                       partial_green,
                                       partial_blue,
                                       SDL_ALPHA_OPAQUE,
                                       partial_alpha ? 3 : 1);
    if (!valid) {
        SDL_SetError("GPU transparency checkpoint failed for %s", label);
    }
    SDL_DestroySurface(checkpoint);
    bool idle = gpu_renderer_conformance_wait_idle();
    SDL_DestroySurface(priming);
    SDL_DestroySurface(canvas);
    return success && valid && idle;
}

static bool draw_checkpoint(SDL_Surface *source,
                            Uint8 ui_red,
                            Uint8 ui_green,
                            Uint8 ui_blue,
                            uint16_t light_level) {
    const int map_size = 32;
    SDL_FRect map_destination = {0.0f, 0.0f, (float)map_size, (float)map_size};
    SDL_FRect ui_destination = {40.0f, 40.0f, 16.0f, 16.0f};
    lighting_vertex_t light_quad[4] = {
        {.x = 0,
         .y = 0,
         .scalar = light_level,
         .red = light_level,
         .green = light_level,
         .blue = light_level},
        {.x = map_size,
         .y = 0,
         .scalar = light_level,
         .red = light_level,
         .green = light_level,
         .blue = light_level},
        {.x = map_size,
         .y = map_size,
         .scalar = light_level,
         .red = light_level,
         .green = light_level,
         .blue = light_level},
        {.x = 0,
         .y = map_size,
         .scalar = light_level,
         .red = light_level,
         .green = light_level,
         .blue = light_level},
    };

#define GPU_STEP(_operation)                                                                    \
    do {                                                                                        \
        if (!(_operation)) {                                                                    \
            fprintf(stderr, "GPU conformance failed at %s: %s\n", #_operation, SDL_GetError()); \
            return false;                                                                       \
        }                                                                                       \
    } while (0)
    GPU_STEP(gpu_renderer_begin_frame());
    GPU_STEP(gpu_renderer_map_begin(map_size, map_size));
    gpu_renderer_map_set_owner(0, map_size / 2, false);
    gpu_renderer_map_light_quad(0, light_quad);
    GPU_STEP(gpu_renderer_draw_surface(source, NULL, &map_destination));
    /* Adjacent identical state must retain painter order in one instanced batch. */
    GPU_STEP(gpu_renderer_draw_surface(source, NULL, &map_destination));
    GPU_STEP(gpu_renderer_map_end());
    gpu_map_renderer_probe_t probe;
    GPU_STEP(gpu_map_renderer_probe(16, 16, 0, &probe));
    if (probe.albedo[0] != 255 || probe.albedo[1] != 0 || probe.albedo[2] != 0 ||
        probe.albedo[3] != 255 || probe.lighting_key != UINT32_C(1) ||
        probe.light[0] != light_level || probe.light[1] != light_level ||
        probe.light[2] != light_level || probe.light[3] != light_level ||
        probe.final_color[0] == 0 || probe.final_color[1] != 0 || probe.final_color[2] != 0 ||
        probe.final_color[3] != 255) {
        fprintf(stderr,
                "GPU map-target mismatch: albedo=%u,%u,%u,%u key=%u "
                "light=%u,%u,%u,%u final=%u,%u,%u,%u\n",
                probe.albedo[0],
                probe.albedo[1],
                probe.albedo[2],
                probe.albedo[3],
                probe.lighting_key,
                probe.light[0],
                probe.light[1],
                probe.light[2],
                probe.light[3],
                probe.final_color[0],
                probe.final_color[1],
                probe.final_color[2],
                probe.final_color[3]);
        return false;
    }
    GPU_STEP(gpu_renderer_draw_map(0.0f, 0.0f, (float)map_size, (float)map_size));
    GPU_STEP(
        gpu_renderer_draw_rect(&ui_destination, ui_red, ui_green, ui_blue, SDL_ALPHA_OPAQUE, true));
    GPU_STEP(gpu_renderer_present());

    SDL_Surface *checkpoint = gpu_renderer_readback(NULL);
    bool valid = checkpoint != NULL &&
                 surface_pixel_is(checkpoint,
                                  16,
                                  16,
                                  probe.final_color[0],
                                  probe.final_color[1],
                                  probe.final_color[2]) &&
                 surface_pixel_is(checkpoint, 48, 48, ui_red, ui_green, ui_blue);
    if (!valid) {
        Uint8 map_red = 0, map_green = 0, map_blue = 0, map_alpha = 0;
        Uint8 ui_actual_red = 0, ui_actual_green = 0, ui_actual_blue = 0, ui_alpha = 0;
        if (checkpoint != NULL) {
            SDL_ReadSurfacePixel(checkpoint, 16, 16, &map_red, &map_green, &map_blue, &map_alpha);
            SDL_ReadSurfacePixel(checkpoint,
                                 48,
                                 48,
                                 &ui_actual_red,
                                 &ui_actual_green,
                                 &ui_actual_blue,
                                 &ui_alpha);
        }
        fprintf(stderr,
                "GPU checkpoint mismatch: map=%u,%u,%u,%u ui=%u,%u,%u,%u error=%s\n",
                map_red,
                map_green,
                map_blue,
                map_alpha,
                ui_actual_red,
                ui_actual_green,
                ui_actual_blue,
                ui_alpha,
                SDL_GetError());
    }
    SDL_DestroySurface(checkpoint);
#undef GPU_STEP
    return valid;
}

static bool target_resize_fault_checkpoint(void) {
    if (!gpu_renderer_begin_frame() || gpu_renderer_map_begin(33, 32) ||
        gpu_renderer_frame_valid() || gpu_map_renderer_texture(false) == NULL) {
        SDL_SetError("target allocation fault did not preserve the published map target");
        return false;
    }
    return true;
}

static bool auxiliary_first_map_checkpoint(SDL_Surface *source) {
    SDL_FRect destination = {0.0f, 0.0f, 24.0f, 24.0f};
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin_auxiliary(24, 24) ||
        !gpu_renderer_draw_surface(source, NULL, &destination) || !gpu_renderer_map_end() ||
        gpu_map_renderer_texture(true) == NULL || !gpu_renderer_present()) {
        return false;
    }
    return gpu_renderer_wait_idle();
}

static bool surfaces_match(SDL_Surface *left, SDL_Surface *right) {
    if (left == NULL || right == NULL || left->w != right->w || left->h != right->h ||
        left->pitch < left->w * 4 || right->pitch < right->w * 4) {
        return false;
    }
    for (int y = 0; y < left->h; y++) {
        if (memcmp((const Uint8 *)left->pixels + (size_t)y * left->pitch,
                   (const Uint8 *)right->pixels + (size_t)y * right->pitch,
                   (size_t)left->w * 4U) != 0) {
            return false;
        }
    }
    return true;
}

static SDL_Surface *retained_damage_frame(SDL_Surface *source, float x) {
    SDL_FRect destination = {x, 8.0f, 16.0f, 16.0f};
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(64, 64)) {
        return NULL;
    }
    gpu_renderer_map_set_instance_identity(UINT64_C(0x493), 0);
    if (!gpu_renderer_draw_surface(source, NULL, &destination) || !gpu_renderer_map_end() ||
        !gpu_renderer_draw_map(0.0f, 0.0f, 64.0f, 64.0f) || !gpu_renderer_present()) {
        return NULL;
    }
    return gpu_renderer_readback(NULL);
}

static bool retained_damage_checkpoint(SDL_Surface *source) {
    gpu_map_renderer_invalidate_target(false);
    gpu_renderer_statistics_reset();
    SDL_Surface *first = retained_damage_frame(source, 8.0f);
    if (first == NULL) {
        return false;
    }
    gpu_renderer_statistics_reset();
    SDL_Surface *middle = retained_damage_frame(source, 32.0f);
    gpu_renderer_statistics_t middle_statistics;
    gpu_renderer_statistics_get(&middle_statistics);
    if (middle == NULL || surfaces_match(first, middle) ||
        middle_statistics.map_full_redraws != 0 || middle_statistics.map_damage_frames != 1 ||
        middle_statistics.map_damage_pixels == 0 ||
        middle_statistics.map_damage_pixels >= 64U * 64U) {
        SDL_SetError("A-to-B retained map movement did not use bounded damage");
        SDL_DestroySurface(first);
        SDL_DestroySurface(middle);
        return false;
    }
    gpu_renderer_statistics_reset();
    SDL_Surface *last = retained_damage_frame(source, 8.0f);
    gpu_renderer_statistics_t last_statistics;
    gpu_renderer_statistics_get(&last_statistics);
    bool valid = last != NULL && surfaces_match(first, last) &&
                 last_statistics.map_full_redraws == 0 && last_statistics.map_damage_frames == 1 &&
                 last_statistics.map_damage_pixels > 0 &&
                 last_statistics.map_damage_pixels < 64U * 64U;
    if (!valid) {
        SDL_SetError("A-to-B-to-A retained map movement did not restore its pixels");
    }
    SDL_DestroySurface(first);
    SDL_DestroySurface(middle);
    SDL_DestroySurface(last);
    return valid;
}

static bool recover_and_republish(SDL_Window *window,
                                  SDL_Surface *source,
                                  bool qualified,
                                  uint16_t light_level);

static bool async_map_submission_checkpoint(SDL_Surface *source) {
    SDL_FRect destination = {0.0f, 0.0f, 32.0f, 32.0f};
    if (!gpu_renderer_wait_idle()) {
        return false;
    }
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(32, 32)) {
        return false;
    }
    /* A preceding retained-map checkpoint may resize the target here and
     * legitimately wait for its old fence. Measure only this submission. */
    gpu_renderer_statistics_reset();
    if (!gpu_renderer_draw_surface(source, NULL, &destination) || !gpu_renderer_map_end()) {
        return false;
    }

    gpu_renderer_statistics_t submitted;
    gpu_renderer_statistics_get(&submitted);
    bool queued = submitted.map_submissions == 1 && submitted.map_completions == 0 &&
                  submitted.timings[GPU_RENDERER_TIMING_COMPLETION].calls == 0 &&
                  gpu_map_renderer_pending_submission_count() == 1 && gpu_renderer_map_available();
    if (!queued) {
        SDL_SetError("map submission completed synchronously or was not published atomically");
        return false;
    }
    size_t peak_pending = gpu_map_renderer_pending_submission_count();
    if (!gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) || !gpu_renderer_present()) {
        return false;
    }
    for (unsigned int frame = 1; frame < 4; frame++) {
        /* Retained map rendering correctly skips an identical frame. Move the
         * source by one pixel so this checkpoint still fills the async queue. */
        destination.x = (float)frame;
        if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(32, 32) ||
            !gpu_renderer_draw_surface(source, NULL, &destination) || !gpu_renderer_map_end() ||
            !gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) || !gpu_renderer_present()) {
            return false;
        }
        peak_pending = MAX(peak_pending, gpu_map_renderer_pending_submission_count());
    }

    gpu_renderer_statistics_t queued_frames;
    gpu_renderer_statistics_get(&queued_frames);
    if (queued_frames.map_submissions != 4 || queued_frames.map_in_flight_peak > 3 ||
        peak_pending > 3) {
        SDL_SetError("map submissions exceeded the bounded in-flight frame budget");
        return false;
    }
    if (!gpu_renderer_wait_idle()) {
        return false;
    }

    gpu_renderer_statistics_t completed;
    gpu_renderer_statistics_get(&completed);
    return completed.map_submissions == 4 && completed.map_completions == 4 &&
           completed.map_in_flight_peak <= 3 && gpu_map_renderer_pending_submission_count() == 0;
}

static bool
async_fence_failure_checkpoint(SDL_Window *window, SDL_Surface *source, bool qualified) {
    SDL_FRect destination = {0.0f, 0.0f, 32.0f, 32.0f};
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(32, 32) ||
        !gpu_renderer_draw_surface(source, NULL, &destination) || !gpu_renderer_map_end()) {
        return false;
    }
    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_FENCE);
    gpu_map_renderer_poll();
    gpu_renderer_statistics_t failed;
    gpu_renderer_statistics_get(&failed);
    bool discarded = gpu_map_renderer_pending_submission_count() == 0 &&
                     !gpu_renderer_map_available() && failed.map_dropped_updates == 1 &&
                     gpu_renderer_recreation_take_request();
    if (!discarded) {
        SDL_SetError("failed map fence did not invalidate the published target");
        return false;
    }
    return recover_and_republish(window, source, qualified, 2048);
}

static bool instance_delta_frame(SDL_Surface *source, float second_x) {
    SDL_FRect first = {0.0f, 0.0f, 16.0f, 16.0f};
    SDL_FRect second = {second_x, 16.0f, 16.0f, 16.0f};
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(32, 32)) {
        return false;
    }
    gpu_renderer_map_set_instance_identity(1, 0);
    if (!gpu_renderer_draw_surface(source, NULL, &first)) {
        return false;
    }
    gpu_renderer_map_set_instance_identity(2, 0);
    return gpu_renderer_draw_surface(source, NULL, &second) && gpu_renderer_map_end() &&
           gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) && gpu_renderer_present();
}

static bool instance_delta_upload_checkpoint(SDL_Surface *source) {
    if (!instance_delta_frame(source, 0.0f)) {
        return false;
    }
    if (!gpu_renderer_wait_idle()) {
        return false;
    }
    gpu_renderer_statistics_reset();
    if (!instance_delta_frame(source, 0.0f)) {
        return false;
    }
    gpu_renderer_statistics_t stable;
    gpu_renderer_statistics_get(&stable);
    if (stable.upload_count != 0 || stable.upload_bytes != 0 ||
        stable.slot_uniform_upload_count != 0 || stable.slot_uniform_upload_bytes != 0 ||
        stable.instance_upload_count != 0 || stable.map_skipped_passes != 1 ||
        stable.map_retained_frames != 1) {
        SDL_SetError("unchanged stable map records submitted retained GPU work");
        return false;
    }
    if (!gpu_renderer_wait_idle()) {
        return false;
    }
    gpu_renderer_statistics_reset();
    if (!instance_delta_frame(source, 1.0f)) {
        return false;
    }
    gpu_renderer_statistics_t changed;
    gpu_renderer_statistics_get(&changed);
    if (changed.upload_count != 2 || changed.slot_uniform_upload_count != 1 ||
        changed.slot_uniform_upload_bytes != 16 || changed.instance_upload_count != 1 ||
        changed.instance_upload_bytes == 0 || changed.instance_upload_bytes > 256 ||
        changed.source_upload_count != 0 || changed.light_upload_count != 0) {
        SDL_SetError("one changed stable map record did not produce one bounded instance delta");
        return false;
    }
    return true;
}

static bool light_delta_frame(SDL_Surface *source, unsigned int changed_quad, uint16_t delta) {
    SDL_FRect destination = {0.0f, 0.0f, 32.0f, 32.0f};
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(32, 32)) {
        return false;
    }
    for (unsigned int index = 0; index < 8; index++) {
        int left = (int)index * 4;
        uint16_t radiance = (uint16_t)(512U + index * 128U + (index == changed_quad ? delta : 0U));
        lighting_vertex_t quad[4] = {
            {.x = left,
             .y = 0,
             .scalar = radiance,
             .red = radiance,
             .green = radiance,
             .blue = radiance},
            {.x = left + 4,
             .y = 0,
             .scalar = radiance,
             .red = radiance,
             .green = radiance,
             .blue = radiance},
            {.x = left + 4,
             .y = 32,
             .scalar = radiance,
             .red = radiance,
             .green = radiance,
             .blue = radiance},
            {.x = left,
             .y = 32,
             .scalar = radiance,
             .red = radiance,
             .green = radiance,
             .blue = radiance},
        };
        gpu_renderer_map_light_quad(0, quad);
    }
    gpu_renderer_map_set_owner(0, 16, false);
    gpu_renderer_map_set_instance_identity(0x477, 0);
    return gpu_renderer_draw_surface(source, NULL, &destination) && gpu_renderer_map_end() &&
           gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) && gpu_renderer_present();
}

/** Prove one spatial or timed sample change transfers exactly one compact quad. */
static bool light_delta_upload_checkpoint(SDL_Surface *source) {
    if (!light_delta_frame(source, UINT_MAX, 0)) {
        return false;
    }
    /* Delta accounting is an idle-state contract. The asynchronous renderer
     * must cycle a compact buffer when an earlier submission still uses it;
     * wait here before measuring the retained snapshot rather than turning
     * this focused upload test into an implicit completion wait. */
    if (!gpu_renderer_wait_idle()) {
        return false;
    }
    gpu_renderer_statistics_reset();
    if (!light_delta_frame(source, UINT_MAX, 0)) {
        return false;
    }
    if (!gpu_renderer_wait_idle()) {
        return false;
    }
    gpu_renderer_statistics_t stable;
    gpu_renderer_statistics_get(&stable);
    if (stable.light_upload_count != 0 || stable.light_upload_bytes != 0) {
        SDL_SetError("unchanged compact light records were uploaded");
        return false;
    }

    for (uint16_t delta = 1; delta <= 2; delta++) {
        if (!gpu_renderer_wait_idle()) {
            return false;
        }
        gpu_renderer_statistics_reset();
        if (!light_delta_frame(source, 2, delta)) {
            return false;
        }
        if (!gpu_renderer_wait_idle()) {
            return false;
        }
        gpu_renderer_statistics_t changed;
        gpu_renderer_statistics_get(&changed);
        if (changed.light_upload_count != 1 || changed.light_upload_bytes != 112 ||
            changed.source_upload_count != 0 || changed.instance_upload_count != 0) {
            SDL_SetError("one compact light change did not produce one 112-byte delta");
            return false;
        }
    }
    return true;
}

static bool retained_identity_frame(SDL_Surface *source, unsigned int phase) {
    const size_t count = 2048;
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(64, 64)) {
        return false;
    }
    for (size_t painter = 0; painter < count; painter++) {
        uint64_t identity = painter + 1U;
        if (phase == 1) {
            identity = painter == 0 ? UINT64_C(0x47700000) : painter;
        } else if (phase == 2) {
            identity = painter == 0 ? count : painter == count - 1U ? 1U : painter + 1U;
        }
        SDL_FRect destination = {
            .x = (float)(identity % 48U),
            .y = (float)((identity / 48U) % 48U),
            .w = 8.0f,
            .h = 8.0f,
        };
        gpu_renderer_map_set_instance_identity(identity, 0);
        if (!gpu_renderer_draw_surface(source, NULL, &destination)) {
            return false;
        }
    }
    return gpu_renderer_map_end() && gpu_renderer_draw_map(0.0f, 0.0f, 64.0f, 64.0f) &&
           gpu_renderer_present();
}

static bool retained_identity_delta_checkpoint(SDL_Surface *source) {
    if (!retained_identity_frame(source, 0)) {
        return false;
    }
    if (!gpu_renderer_wait_idle()) {
        return false;
    }
    gpu_renderer_statistics_reset();
    if (!retained_identity_frame(source, 1)) {
        return false;
    }
    gpu_renderer_statistics_t inserted;
    gpu_renderer_statistics_get(&inserted);
    if (inserted.instance_upload_count != 1 || inserted.instance_upload_bytes == 0 ||
        inserted.instance_upload_bytes > 256 || inserted.source_upload_count != 0 ||
        inserted.light_upload_count != 0) {
        SDL_SetError("stable-slot head insertion rewrote the painter suffix");
        return false;
    }
    if (!gpu_renderer_wait_idle()) {
        return false;
    }
    gpu_renderer_statistics_reset();
    if (!retained_identity_frame(source, 2)) {
        return false;
    }
    gpu_renderer_statistics_t reordered;
    gpu_renderer_statistics_get(&reordered);
    if (reordered.instance_upload_count > 2 || reordered.instance_upload_bytes > 512 ||
        reordered.source_upload_count != 0 || reordered.light_upload_count != 0) {
        SDL_SetError("stable-slot painter reorder rewrote unchanged instance records");
        return false;
    }
    return true;
}

static bool atlas_batch_checkpoint(SDL_Surface *first, SDL_Surface *second) {
    SDL_FRect left = {0.0f, 0.0f, 16.0f, 32.0f};
    SDL_FRect right = {16.0f, 0.0f, 16.0f, 32.0f};
    gpu_renderer_statistics_reset();
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(32, 32)) {
        return false;
    }
    gpu_renderer_map_set_instance_identity(100, 0);
    if (!gpu_renderer_draw_surface(first, NULL, &left)) {
        return false;
    }
    gpu_renderer_map_set_instance_identity(101, 0);
    if (!gpu_renderer_draw_surface(second, NULL, &right) || !gpu_renderer_map_end() ||
        !gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) || !gpu_renderer_present()) {
        return false;
    }
    gpu_renderer_statistics_t statistics;
    gpu_renderer_statistics_get(&statistics);
    if (statistics.commands != 3 || statistics.batches != 2 || statistics.draws != 4 ||
        statistics.source_upload_count != 1 || statistics.source_upload_bytes == 0 ||
        statistics.resource_creations != 0) {
        SDL_SetError("atlas batch checkpoint: commands=%llu batches=%llu draws=%llu "
                     "source_uploads=%llu source_bytes=%llu creations=%llu",
                     (unsigned long long)statistics.commands,
                     (unsigned long long)statistics.batches,
                     (unsigned long long)statistics.draws,
                     (unsigned long long)statistics.source_upload_count,
                     (unsigned long long)statistics.source_upload_bytes,
                     (unsigned long long)statistics.resource_creations);
        return false;
    }
    return true;
}

static bool
slot_fragmentation_frame(SDL_Surface *source, unsigned int phase, bool change_one_instance) {
    const size_t command_count = 1024;
    const size_t identity_count = 1280;
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(64, 64)) {
        return false;
    }
    for (size_t painter = 0; painter < command_count; painter++) {
        uint64_t identity = UINT64_C(0x47720000) + (painter * 257U + phase * 67U) % identity_count;
        SDL_FRect destination = {
            .x = (float)(identity % 48U),
            .y = (float)((identity / 48U) % 48U),
            .w = 8.0f,
            .h = 8.0f,
        };
        if (change_one_instance && painter == command_count / 2U) {
            destination.x += 1.0f;
        }
        gpu_renderer_map_set_instance_identity(identity, 0);
        if (!gpu_renderer_draw_surface(source, NULL, &destination)) {
            return false;
        }
    }
    return gpu_renderer_map_end() && gpu_renderer_draw_map(0.0f, 0.0f, 64.0f, 64.0f) &&
           gpu_renderer_present();
}

static bool slot_fragmentation_checkpoint(SDL_Surface *source) {
    const unsigned int phases = 8;
    const uint64_t commands = 1024 + 1; /* World commands plus the final pass. */
    const uint64_t maximum_batches = 1024 / 256 + 1; /* Slot chunks plus final pass. */
    for (unsigned int phase = 0; phase < phases; phase++) {
        gpu_renderer_statistics_reset();
        if (!slot_fragmentation_frame(source, phase, false)) {
            return false;
        }
        gpu_renderer_statistics_t statistics;
        gpu_renderer_statistics_get(&statistics);
        if (statistics.commands != commands || statistics.batches > maximum_batches) {
            SDL_SetError("fragmented stable slots produced %llu batches for %llu commands at "
                         "phase %u",
                         (unsigned long long)statistics.batches,
                         (unsigned long long)statistics.commands,
                         phase);
            return false;
        }
    }

    if (!gpu_renderer_wait_idle()) {
        return false;
    }
    gpu_renderer_statistics_reset();
    if (!slot_fragmentation_frame(source, phases - 1U, false)) {
        return false;
    }
    gpu_renderer_statistics_t stable;
    gpu_renderer_statistics_get(&stable);
    if (stable.commands != 0 || stable.batches != 0 || stable.slot_uniform_upload_count != 0 ||
        stable.slot_uniform_upload_bytes != 0 || stable.instance_upload_count != 0 ||
        stable.instance_upload_bytes != 0 || stable.source_upload_count != 0 ||
        stable.light_upload_count != 0 || stable.map_skipped_passes != 1 ||
        stable.map_retained_frames != 1) {
        SDL_SetError("unchanged fragmented slots submitted retained GPU work");
        return false;
    }

    if (!gpu_renderer_wait_idle()) {
        return false;
    }
    gpu_renderer_statistics_reset();
    if (!slot_fragmentation_frame(source, phases - 1U, true)) {
        return false;
    }
    gpu_renderer_statistics_t changed;
    gpu_renderer_statistics_get(&changed);
    if (changed.commands >= commands || changed.batches > commands ||
        changed.slot_uniform_upload_count != changed.batches - 1U ||
        changed.slot_uniform_upload_bytes > 4096 || changed.instance_upload_count != 1 ||
        changed.instance_upload_bytes == 0 || changed.instance_upload_bytes > 256 ||
        changed.source_upload_count != 0 || changed.light_upload_count != 0 ||
        changed.map_damage_frames != 1 || changed.map_damage_pixels == 0 ||
        changed.map_damage_pixels >= 64U * 64U) {
        SDL_SetError("one fragmented-slot instance change exceeded sparse upload/damage bounds");
        return false;
    }
    return true;
}

static bool queued_asset_invalidation_checkpoint(void) {
    SDL_Surface *temporary = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA32);
    SDL_FRect destination = {0.0f, 0.0f, 32.0f, 32.0f};
    bool success =
        temporary != NULL &&
        SDL_FillSurfaceRect(temporary,
                            NULL,
                            SDL_MapSurfaceRGBA(temporary, 32, 192, 64, SDL_ALPHA_OPAQUE)) &&
        gpu_renderer_begin_frame() && gpu_renderer_map_begin(32, 32);
    if (success) {
        gpu_renderer_map_set_instance_identity(477, 0);
        success = gpu_renderer_draw_surface(temporary, NULL, &destination);
    }
    if (success) {
        /* Simulate cache eviction while the command is queued. The command's
         * retained asset reference must remain valid through submission. */
        gpu_renderer_invalidate_surface(temporary);
        success = gpu_renderer_map_end() && gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) &&
                  gpu_renderer_present();
    }
    SDL_DestroySurface(temporary);
    return success;
}

static bool atlas_churn_cycle(unsigned int phase, size_t *peak_pages) {
    const size_t surfaces_num = 160;
    SDL_Surface **surfaces = xcalloc(surfaces_num, sizeof(*surfaces));
    bool success = true;
    for (size_t i = 0; i < surfaces_num; i++) {
        bool landscape = ((unsigned int)i + phase) % 2U == 0;
        int width = landscape ? 256 : 192;
        int height = landscape ? 192 : 256;
        surfaces[i] = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
        if (surfaces[i] == NULL ||
            !SDL_FillSurfaceRect(surfaces[i],
                                 NULL,
                                 SDL_MapSurfaceRGBA(surfaces[i],
                                                    (Uint8)(32U + (i % 192U)),
                                                    (Uint8)(64U + (i % 128U)),
                                                    (Uint8)(96U + (i % 96U)),
                                                    SDL_ALPHA_OPAQUE))) {
            success = false;
            break;
        }
    }
    SDL_FRect destination = {0.0f, 0.0f, 8.0f, 8.0f};
    success = success && gpu_renderer_begin_frame() && gpu_renderer_map_begin(32, 32);
    for (size_t i = 0; success && i < surfaces_num; i++) {
        destination.x = (float)(i % 4U) * 8.0f;
        destination.y = (float)((i / 4U) % 4U) * 8.0f;
        gpu_renderer_map_set_instance_identity(UINT64_C(0x47710000) + i, phase);
        success = gpu_renderer_draw_surface(surfaces[i], NULL, &destination);
    }
    success = success && gpu_renderer_map_end() &&
              gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) && gpu_renderer_present();
    if (success && peak_pages != NULL) {
        *peak_pages = gpu_map_renderer_atlas_page_count();
    }
    for (size_t i = 0; i < surfaces_num; i++) {
        SDL_DestroySurface(surfaces[i]);
    }
    free(surfaces);
    if (!success) {
        return false;
    }

    /* Replacing the world command set releases its retained asset references. */
    success = gpu_renderer_begin_frame() && gpu_renderer_map_begin(32, 32) &&
              gpu_renderer_map_end() && gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) &&
              gpu_renderer_present();
    /* Resource-retirement assertions must observe the fence, not merely the
     * client-side surface destruction that queued the release. */
    return success && gpu_renderer_wait_idle();
}

static bool atlas_churn_plateau_checkpoint(void) {
    /* Release assets retained by earlier checkpoints before taking the
     * plateau baseline; the churn contract is about its own live set. */
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(32, 32) || !gpu_renderer_map_end() ||
        !gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) || !gpu_renderer_present()) {
        return false;
    }
    size_t baseline_pages = gpu_map_renderer_atlas_page_count();
    size_t baseline_allocations = gpu_map_renderer_atlas_allocation_count();
    size_t peak_pages = 0;
    if (!atlas_churn_cycle(0, &peak_pages)) {
        return false;
    }
    size_t plateau_pages = gpu_map_renderer_atlas_page_count();
    size_t plateau_allocations = gpu_map_renderer_atlas_allocation_count();
    gpu_renderer_statistics_t plateau_statistics;
    gpu_renderer_statistics_get(&plateau_statistics);
    if (peak_pages <= baseline_pages || plateau_pages > baseline_pages + 1U ||
        plateau_allocations != baseline_allocations) {
        SDL_SetError("atlas churn did not reclaim empty pages: baseline=%" PRIu64 " peak=%" PRIu64
                     " retained=%" PRIu64 " allocations=%" PRIu64 "/%" PRIu64,
                     (uint64_t)baseline_pages,
                     (uint64_t)peak_pages,
                     (uint64_t)plateau_pages,
                     (uint64_t)plateau_allocations,
                     (uint64_t)baseline_allocations);
        return false;
    }
    for (unsigned int phase = 1; phase < 4; phase++) {
        if (!atlas_churn_cycle(phase, NULL)) {
            return false;
        }
        gpu_renderer_statistics_t statistics;
        gpu_renderer_statistics_get(&statistics);
        if (gpu_map_renderer_atlas_page_count() != plateau_pages ||
            gpu_map_renderer_atlas_allocation_count() != baseline_allocations ||
            statistics.retained_bytes != plateau_statistics.retained_bytes) {
            SDL_SetError("atlas alternating-size churn did not plateau at phase %u", phase);
            return false;
        }
    }
    return true;
}

static bool atlas_allocation_fault_checkpoint(void) {
    SDL_Surface *temporary = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_FRect destination = {0.0f, 0.0f, 32.0f, 32.0f};
    size_t pages = gpu_map_renderer_atlas_page_count();
    size_t allocations = gpu_map_renderer_atlas_allocation_count();
    bool success =
        temporary != NULL &&
        SDL_FillSurfaceRect(temporary,
                            NULL,
                            SDL_MapSurfaceRGBA(temporary, 192, 32, 96, SDL_ALPHA_OPAQUE)) &&
        gpu_renderer_begin_frame() && gpu_renderer_map_begin(32, 32);
    if (success) {
        gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_ALLOCATION);
        success = !gpu_renderer_draw_surface(temporary, NULL, &destination) &&
                  !gpu_renderer_frame_valid() && gpu_renderer_map_end() &&
                  gpu_map_renderer_atlas_page_count() == pages &&
                  gpu_map_renderer_atlas_allocation_count() == allocations;
    }
    SDL_DestroySurface(temporary);
    if (!success) {
        SDL_SetError("failed atlas allocation retained an asset or page");
    }
    return success;
}

static bool ui_atlas_fault_checkpoint(void) {
    SDL_Surface *surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
    SDL_FRect destination = {0.0f, 0.0f, 8.0f, 8.0f};
    size_t pages = gpu_renderer_atlas_page_count();
    size_t allocations = gpu_renderer_atlas_allocation_count();
    bool success =
        surface != NULL &&
        SDL_FillSurfaceRect(surface,
                            NULL,
                            SDL_MapSurfaceRGBA(surface, 192, 32, 96, SDL_ALPHA_OPAQUE)) &&
        gpu_renderer_begin_frame();
    if (success) {
        gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_UI_ATLAS_UPLOAD);
        success = !gpu_renderer_draw_surface(surface, NULL, &destination) &&
                  !gpu_renderer_frame_valid() && gpu_renderer_atlas_page_count() <= pages + 1U &&
                  gpu_renderer_atlas_allocation_count() == allocations;
    }
    SDL_DestroySurface(surface);
    if (!success) {
        SDL_SetError("failed UI atlas upload retained an allocation");
    }
    return success;
}

static bool ui_atlas_churn_cycle(unsigned int phase, size_t *peak_pages) {
    const size_t surfaces_num = 40;
    SDL_Surface **surfaces = xcalloc(surfaces_num, sizeof(*surfaces));
    bool success = true;
    for (size_t index = 0; index < surfaces_num; index++) {
        bool landscape = ((unsigned int)index + phase) % 2U == 0;
        int width = landscape ? 512 : 384;
        int height = landscape ? 384 : 512;
        surfaces[index] = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
        if (surfaces[index] == NULL || !SDL_FillSurfaceRect(surfaces[index],
                                                            NULL,
                                                            SDL_MapSurfaceRGBA(surfaces[index],
                                                                               (Uint8)(32U + index),
                                                                               (Uint8)(64U + index),
                                                                               (Uint8)(96U + index),
                                                                               SDL_ALPHA_OPAQUE))) {
            success = false;
            break;
        }
    }
    SDL_FRect destination = {0.0f, 0.0f, 8.0f, 8.0f};
    success = success && gpu_renderer_begin_frame();
    for (size_t index = 0; success && index < surfaces_num; index++) {
        destination.x = (float)(index % 8U);
        destination.y = (float)((index / 8U) % 8U);
        success = gpu_renderer_draw_surface(surfaces[index], NULL, &destination);
    }
    success = success && gpu_renderer_present();
    if (success && peak_pages != NULL) {
        *peak_pages = gpu_renderer_atlas_page_count();
    }
    for (size_t index = 0; index < surfaces_num; index++) {
        SDL_DestroySurface(surfaces[index]);
    }
    free(surfaces);
    return success && gpu_renderer_wait_idle();
}

static bool ui_atlas_churn_plateau_checkpoint(void) {
    size_t baseline_pages = gpu_renderer_atlas_page_count();
    size_t baseline_allocations = gpu_renderer_atlas_allocation_count();
    size_t peak_pages = 0;
    if (!ui_atlas_churn_cycle(0, &peak_pages)) {
        return false;
    }
    size_t plateau_pages = gpu_renderer_atlas_page_count();
    gpu_renderer_statistics_t plateau_statistics;
    gpu_renderer_statistics_get(&plateau_statistics);
    if (peak_pages <= baseline_pages || plateau_pages > baseline_pages + 1U ||
        gpu_renderer_atlas_allocation_count() != baseline_allocations) {
        SDL_SetError("UI atlas churn did not reclaim empty pages");
        return false;
    }
    for (unsigned int phase = 1; phase < 4; phase++) {
        if (!ui_atlas_churn_cycle(phase, NULL)) {
            return false;
        }
        gpu_renderer_statistics_t statistics;
        gpu_renderer_statistics_get(&statistics);
        if (gpu_renderer_atlas_page_count() != plateau_pages ||
            gpu_renderer_atlas_allocation_count() != baseline_allocations ||
            statistics.retained_bytes != plateau_statistics.retained_bytes) {
            SDL_SetError("UI atlas alternating-size churn did not plateau at phase %u", phase);
            return false;
        }
    }
    return true;
}

/** Render primary then auxiliary targets and prove the primary remains selected. */
static bool retained_primary_auxiliary_checkpoint(SDL_Surface *primary,
                                                  SDL_Surface *auxiliary,
                                                  bool verify_churn) {
    SDL_FRect primary_destination = {0.0f, 0.0f, 64.0f, 64.0f};
    SDL_FRect auxiliary_destination = {0.0f, 0.0f, 24.0f, 24.0f};
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(64, 64) ||
        !gpu_renderer_draw_surface(primary, NULL, &primary_destination) ||
        !gpu_renderer_map_end() || !gpu_renderer_map_begin_auxiliary(24, 24) ||
        !gpu_renderer_draw_surface(auxiliary, NULL, &auxiliary_destination) ||
        !gpu_renderer_map_end() || !gpu_renderer_draw_map(0.0f, 0.0f, 64.0f, 64.0f) ||
        !gpu_renderer_present()) {
        return false;
    }
    if (verify_churn) {
        gpu_renderer_statistics_t statistics;
        gpu_renderer_statistics_get(&statistics);
        if (statistics.resource_creations != 0 || statistics.resource_destructions != 0 ||
            statistics.upload_count != statistics.slot_uniform_upload_count ||
            statistics.upload_bytes != statistics.slot_uniform_upload_bytes ||
            statistics.slot_uniform_upload_count != 2 ||
            statistics.slot_uniform_upload_bytes != 32) {
            SDL_SetError("retained primary/auxiliary map targets churned after warmup");
            return false;
        }
    }
    SDL_Surface *checkpoint = gpu_renderer_readback(NULL);
    bool valid = checkpoint != NULL && surface_pixel_is(checkpoint, 32, 32, 255, 0, 0);
    SDL_DestroySurface(checkpoint);
    if (!valid) {
        SDL_SetError("auxiliary rendering replaced the retained primary map target");
    }
    return valid;
}

static bool lighting_lookup_checkpoint(SDL_Surface *source) {
    SDL_FRect full = {0.0f, 0.0f, 32.0f, 32.0f};
    SDL_FRect inset = {8.0f, 8.0f, 16.0f, 16.0f};
    SDL_FRect extrapolation_pixel = {12.0f, 14.0f, 1.0f, 1.0f};
    lighting_vertex_t low[4] = {
        {.x = 0, .y = 0, .scalar = 512, .red = 512, .green = 512, .blue = 512},
        {.x = 32, .y = 0, .scalar = 512, .red = 512, .green = 512, .blue = 512},
        {.x = 32, .y = 32, .scalar = 512, .red = 512, .green = 512, .blue = 512},
        {.x = 0, .y = 32, .scalar = 512, .red = 512, .green = 512, .blue = 512},
    };
    lighting_vertex_t high[4] = {
        {.x = 0, .y = 0, .scalar = 2048, .red = 2048, .green = 2048, .blue = 2048},
        {.x = 32, .y = 0, .scalar = 2048, .red = 2048, .green = 2048, .blue = 2048},
        {.x = 32, .y = 32, .scalar = 2048, .red = 2048, .green = 2048, .blue = 2048},
        {.x = 0, .y = 32, .scalar = 2048, .red = 2048, .green = 2048, .blue = 2048},
    };
    lighting_vertex_t edge[4] = {
        {.x = 8, .y = 8, .scalar = 1024, .red = 1024, .green = 1024, .blue = 1024},
        {.x = 24, .y = 8, .scalar = 1024, .red = 1024, .green = 1024, .blue = 1024},
        {.x = 24, .y = 24, .scalar = 1024, .red = 1024, .green = 1024, .blue = 1024},
        {.x = 8, .y = 24, .scalar = 1024, .red = 1024, .green = 1024, .blue = 1024},
    };
    lighting_vertex_t gap_top_left[4] = {
        {.x = 2, .y = 4, .scalar = 200, .red = 300, .green = 2000, .blue = 1700},
        {.x = 6, .y = 4, .scalar = 200, .red = 300, .green = 2000, .blue = 1700},
        {.x = 6, .y = 8, .scalar = 200, .red = 300, .green = 2000, .blue = 1700},
        {.x = 2, .y = 8, .scalar = 200, .red = 300, .green = 2000, .blue = 1700},
    };
    lighting_vertex_t gap_top_right[4] = {
        {.x = 18, .y = 4, .scalar = 1000, .red = 1100, .green = 1000, .blue = 900},
        {.x = 22, .y = 4, .scalar = 1000, .red = 1100, .green = 1000, .blue = 900},
        {.x = 22, .y = 8, .scalar = 1000, .red = 1100, .green = 1000, .blue = 900},
        {.x = 18, .y = 8, .scalar = 1000, .red = 1100, .green = 1000, .blue = 900},
    };
    lighting_vertex_t gap_bottom_left[4] = {
        {.x = 4, .y = 20, .scalar = 1400, .red = 1500, .green = 600, .blue = 800},
        {.x = 8, .y = 20, .scalar = 1400, .red = 1500, .green = 600, .blue = 800},
        {.x = 8, .y = 24, .scalar = 1400, .red = 1500, .green = 600, .blue = 800},
        {.x = 4, .y = 24, .scalar = 1400, .red = 1500, .green = 600, .blue = 800},
    };
    lighting_vertex_t gap_bottom_right[4] = {
        {.x = 20, .y = 20, .scalar = 2000, .red = 1900, .green = 1600, .blue = 200},
        {.x = 24, .y = 20, .scalar = 2000, .red = 1900, .green = 1600, .blue = 200},
        {.x = 24, .y = 24, .scalar = 2000, .red = 1900, .green = 1600, .blue = 200},
        {.x = 20, .y = 24, .scalar = 2000, .red = 1900, .green = 1600, .blue = 200},
    };
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(32, 32)) {
        return false;
    }
    gpu_renderer_map_light_quad(0, low);
    gpu_renderer_map_light_quad(0, high);
    gpu_renderer_map_light_quad(1, edge);
    gpu_renderer_map_light_quad(2, gap_top_left);
    gpu_renderer_map_light_quad(2, gap_top_right);
    gpu_renderer_map_light_quad(2, gap_bottom_left);
    gpu_renderer_map_light_quad(2, gap_bottom_right);
    gpu_renderer_map_set_owner(0, 16, false);
    if (!gpu_renderer_draw_surface(source, NULL, &full)) {
        return false;
    }
    gpu_renderer_map_set_owner(1, -200, false);
    if (!gpu_renderer_draw_surface(source, NULL, &inset)) {
        return false;
    }
    gpu_renderer_map_set_owner(2, 14, false);
    if (!gpu_renderer_draw_surface(source, NULL, &extrapolation_pixel) || !gpu_renderer_map_end()) {
        return false;
    }
    gpu_map_renderer_probe_t overlap;
    gpu_map_renderer_probe_t extrapolated;
    gpu_map_renderer_probe_t horizontal_then_vertical;
    const uint16_t expected_radiance[3] = {1227, 1205, 894};
    uint16_t expected_linear[3];
    lighting_tone_map_linear(1168, expected_radiance, expected_linear);
    uint8_t expected_red = lighting_multiply_channel(255, expected_linear[0]);
    if (!gpu_map_renderer_probe(4, 4, 0, &overlap) ||
        !gpu_map_renderer_probe(16, 16, 1, &extrapolated) ||
        !gpu_map_renderer_probe(12, 14, 2, &horizontal_then_vertical) ||
        overlap.lighting_key != UINT32_C(1) || overlap.light[0] != 2048 ||
        overlap.light[1] != 2048 || overlap.light[2] != 2048 || overlap.light[3] != 2048 ||
        extrapolated.lighting_key != UINT32_C(2) || extrapolated.light[0] != 1024 ||
        extrapolated.light[1] != 1024 || extrapolated.light[2] != 1024 ||
        extrapolated.light[3] != 1024 || horizontal_then_vertical.lighting_key != UINT32_C(3) ||
        horizontal_then_vertical.light[0] != 1168 || horizontal_then_vertical.light[1] != 1227 ||
        horizontal_then_vertical.light[2] != 1205 || horizontal_then_vertical.light[3] != 894 ||
        horizontal_then_vertical.final_color[0] != expected_red ||
        horizontal_then_vertical.final_color[1] != 0 ||
        horizontal_then_vertical.final_color[2] != 0 ||
        horizontal_then_vertical.final_color[3] != SDL_ALPHA_OPAQUE) {
        SDL_SetError("compact GPU light precedence/extrapolation checkpoint failed");
        return false;
    }
    return gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) && gpu_renderer_present();
}

/** Preserve projected per-pixel lighting separately from fixed structural rows. */
static bool projected_lighting_checkpoint(SDL_Surface *source, float height) {
    SDL_FRect full = {0.0f, 0.0f, 32.0f, height};
    lighting_vertex_t gradient[4] = {
        {.x = 0, .y = 0, .scalar = 256, .red = 256, .green = 256, .blue = 256},
        {.x = 32, .y = 0, .scalar = 256, .red = 256, .green = 256, .blue = 256},
        {.x = 32, .y = 32, .scalar = 2048, .red = 2048, .green = 2048, .blue = 2048},
        {.x = 0, .y = 32, .scalar = 2048, .red = 2048, .green = 2048, .blue = 2048},
    };
    if (!gpu_renderer_begin_frame() || !gpu_renderer_map_begin(32, 32)) {
        return false;
    }
    gpu_renderer_map_light_quad(0, gradient);
    gpu_renderer_map_set_owner(0, 0, true);
    if (!gpu_renderer_draw_surface(source, NULL, &full) || !gpu_renderer_map_end()) {
        return false;
    }
    gpu_map_renderer_probe_t upper;
    gpu_map_renderer_probe_t lower;
    if (!gpu_map_renderer_probe(16, 4, 0, &upper) || !gpu_map_renderer_probe(16, 28, 0, &lower) ||
        upper.lighting_key == 0 || lower.lighting_key == 0 ||
        upper.lighting_key == lower.lighting_key || upper.light[0] >= lower.light[0] ||
        upper.final_color[0] >= lower.final_color[0]) {
        SDL_SetError("projected GPU lighting collapsed to one structural sample row");
        return false;
    }
    return gpu_renderer_draw_map(0.0f, 0.0f, 32.0f, 32.0f) && gpu_renderer_present();
}

static bool surface_sha256(SDL_Surface *surface, char digest[65]) {
    SDL_Surface *canonical = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (canonical == NULL) {
        return false;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    bool succeeded = context != NULL && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    uint8_t dimensions[8];
    uint32_t width = (uint32_t)canonical->w;
    uint32_t height = (uint32_t)canonical->h;
    for (size_t i = 0; i < 4; i++) {
        dimensions[i] = (uint8_t)(width >> (24U - i * 8U));
        dimensions[4U + i] = (uint8_t)(height >> (24U - i * 8U));
    }
    succeeded = succeeded && EVP_DigestUpdate(context, dimensions, sizeof(dimensions)) == 1;
    bool locked = !SDL_MUSTLOCK(canonical) || SDL_LockSurface(canonical);
    succeeded = succeeded && locked;
    for (int y = 0; succeeded && y < canonical->h; y++) {
        const uint8_t *row = (const uint8_t *)canonical->pixels + (size_t)y * canonical->pitch;
        succeeded = EVP_DigestUpdate(context, row, (size_t)canonical->w * 4U) == 1;
    }
    if (locked && SDL_MUSTLOCK(canonical)) {
        SDL_UnlockSurface(canonical);
    }
    uint8_t raw[EVP_MAX_MD_SIZE];
    unsigned int raw_size = 0;
    succeeded = succeeded && EVP_DigestFinal_ex(context, raw, &raw_size) == 1 && raw_size == 32;
    EVP_MD_CTX_free(context);
    SDL_DestroySurface(canonical);
    if (!succeeded) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        snprintf(&digest[i * 2U], 3, "%02x", raw[i]);
    }
    return true;
}

static bool qualification_requested(void) {
    const char *value = SDL_GetEnvironmentVariable(SDL_GetEnvironment(),
                                                   "ATRINIK_GPU_CONFORMANCE_QUALIFIED_HARDWARE");
    return value != NULL && strcmp(value, "1") == 0;
}

typedef struct recovery_scene {
    SDL_Surface *source;
    uint16_t light_level;
} recovery_scene_t;

static bool recovery_window_apply(void *userdata) {
    (void)userdata;
    return true;
}

static bool recovery_scene_republish(void *userdata) {
    recovery_scene_t *scene = userdata;
    return draw_checkpoint(scene->source, 0, 255, 0, scene->light_level);
}

static bool recovery_scene_republish_failure(void *userdata) {
    (void)userdata;
    SDL_SetError("injected complete-scene republish callback failure");
    return false;
}

static bool recover_and_republish(SDL_Window *window,
                                  SDL_Surface *source,
                                  bool qualified,
                                  uint16_t light_level) {
    gpu_renderer_statistics_t before;
    gpu_renderer_statistics_get(&before);
    unsigned int attempts = 0;
    recovery_scene_t scene = {.source = source, .light_level = light_level};
    if (!gpu_renderer_recover_and_republish(window,
                                            &attempts,
                                            !qualified,
                                            recovery_window_apply,
                                            recovery_scene_republish,
                                            &scene) ||
        attempts != 1U) {
        return false;
    }
    gpu_renderer_statistics_t after;
    gpu_renderer_statistics_get(&after);
    return after.device_recoveries == before.device_recoveries + 1U &&
           after.upload_count > before.upload_count &&
           after.resource_creations > before.resource_creations;
}

static bool canvas_registration_fault_checkpoint(void) {
    SDL_Surface *canvas = SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_RGBA32);
    bool success = canvas != NULL && gpu_renderer_begin_frame();
    if (success) {
        gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_CANVAS_REGISTRATION);
        success = !gpu_renderer_canvas_register(&canvas) && canvas == NULL &&
                  !gpu_renderer_frame_valid() && !gpu_renderer_present();
    }
    SDL_DestroySurface(canvas);
    if (!success) {
        SDL_SetError(
            "failed canvas registration did not invalidate the frame and release ownership");
    }
    return success;
}

static bool canvas_registration_retry_checkpoint(void) {
    SDL_Surface *canvas = SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_RGBA32);
    SDL_FRect destination = {8.0f, 8.0f, 16.0f, 16.0f};
    bool success = canvas != NULL && gpu_renderer_begin_frame() &&
                   gpu_renderer_canvas_register(&canvas) &&
                   gpu_renderer_canvas_registered(canvas) &&
                   gpu_renderer_canvas_fill(canvas, NULL, 32, 96, 192, SDL_ALPHA_OPAQUE) &&
                   gpu_renderer_draw_surface(canvas, NULL, &destination) &&
                   gpu_renderer_present() && gpu_renderer_conformance_wait_idle();
    SDL_DestroySurface(canvas);
    if (!success) {
        SDL_SetError("canvas registration did not recover on the next recreated canvas");
    }
    return success;
}

int main(void) {
#define GPU_REQUIRE(_condition)                                     \
    do {                                                            \
        if (!(_condition)) {                                        \
            fprintf(stderr,                                         \
                    "GPU conformance requirement failed: %s: %s\n", \
                    #_condition,                                    \
                    SDL_GetError());                                \
            return EXIT_FAILURE;                                    \
        }                                                           \
    } while (0)
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return conformance_unavailable("SDL video initialization");
    }

    GPU_REQUIRE(gpu_map_renderer_target_payload_bytes(1920, 1080, 5) ==
                UINT64_C(1920) * 1080U * 12U);
    GPU_REQUIRE(gpu_map_renderer_target_payload_bytes(2560, 1440, 7) ==
                UINT64_C(2560) * 1440U * 12U);
    GPU_REQUIRE(gpu_map_renderer_target_retained_bytes(1920, 1080, 5) >
                gpu_map_renderer_target_payload_bytes(1920, 1080, 5));
    GPU_REQUIRE(gpu_map_renderer_target_retained_bytes(2560, 1440, 7) >
                gpu_map_renderer_target_payload_bytes(2560, 1440, 7));
    GPU_REQUIRE(gpu_map_renderer_target_retained_bytes(3840, 2160, 1) ==
                gpu_map_renderer_target_retained_bytes(3840, 2160, MAP2_LEVELS));
    GPU_REQUIRE(gpu_map_renderer_target_retained_bytes(3840, 2160, MAP2_LEVELS) % (64U * 1024U) ==
                0);

    SDL_Window *window = SDL_CreateWindow("Atrinik GPU conformance",
                                          64,
                                          64,
                                          SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    if (window == NULL) {
        int result = conformance_unavailable("hidden window creation");
        SDL_Quit();
        return result;
    }
    bool qualified = qualification_requested();
    const char *requested_driver =
        SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "ATRINIK_GPU_CONFORMANCE_DRIVER");
    if (qualified && requested_driver != NULL && *requested_driver != '\0' &&
        !SDL_SetHint(SDL_HINT_GPU_DRIVER, requested_driver)) {
        fprintf(stderr, "GPU qualification could not select requested production backend\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    if (!qualified && !gpu_renderer_conformance_available()) {
        int result = conformance_unavailable("supported GPU device preflight");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return result;
    }
    bool renderer_created =
        qualified ? gpu_renderer_create(window) : gpu_renderer_create_conformance(window);
    if (!renderer_created) {
        if (!qualified) {
            int result = conformance_unavailable("production renderer creation");
            SDL_DestroyWindow(window);
            SDL_Quit();
            return result;
        }
        fprintf(stderr,
                "GPU conformance failed at production renderer creation: %s\n",
                SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    GPU_REQUIRE(gpu_renderer_ready());
    GPU_REQUIRE(!qualified || gpu_renderer_hardware_verified());
    GPU_REQUIRE(strcmp(gpu_renderer_backend(), "vulkan") == 0 ||
                strcmp(gpu_renderer_backend(), "direct3d12") == 0 ||
                strcmp(gpu_renderer_backend(), "metal") == 0);
    GPU_REQUIRE(gpu_renderer_device_name()[0] != '\0');
    GPU_REQUIRE(!qualified || strcmp(gpu_renderer_device_name(), "unavailable") != 0);
    GPU_REQUIRE(!qualified || strcmp(gpu_renderer_driver_name(), "unavailable") != 0);
    GPU_REQUIRE(!qualified || strcmp(gpu_renderer_driver_version(), "unavailable") != 0);
    if (qualified && strcmp(gpu_renderer_backend(), "direct3d12") == 0) {
        const char *identity = gpu_renderer_adapter_identity();
        GPU_REQUIRE(strlen(identity) == 27);
        GPU_REQUIRE(strncmp(identity, "dxgi-luid:", 10) == 0);
        GPU_REQUIRE(identity[18] == ':');
        for (size_t index = 10; index < 27; index++) {
            if (index == 18) {
                continue;
            }
            GPU_REQUIRE((identity[index] >= '0' && identity[index] <= '9') ||
                        (identity[index] >= 'a' && identity[index] <= 'f'));
        }
    }

    SDL_Surface *sources[4] = {
        SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32),
        SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_RGBA32),
        SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA32),
        SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_RGBA32),
    };
    for (size_t i = 0; i < SDL_arraysize(sources); i++) {
        GPU_REQUIRE(sources[i] != NULL);
    }
    SDL_Surface *source = sources[0];
    GPU_REQUIRE(
        SDL_FillSurfaceRect(source, NULL, SDL_MapSurfaceRGBA(source, 255, 0, 0, SDL_ALPHA_OPAQUE)));
    GPU_REQUIRE(SDL_FillSurfaceRect(sources[1],
                                    NULL,
                                    SDL_MapSurfaceRGBA(sources[1], 0, 96, 255, SDL_ALPHA_OPAQUE)));
    GPU_REQUIRE(SDL_SetSurfaceAlphaMod(sources[1], 160));
    GPU_REQUIRE(SDL_SetSurfaceBlendMode(sources[1], SDL_BLENDMODE_BLEND));
    GPU_REQUIRE(SDL_FillSurfaceRect(sources[2],
                                    NULL,
                                    SDL_MapSurfaceRGBA(sources[2], 255, 196, 0, SDL_ALPHA_OPAQUE)));
    GPU_REQUIRE(SDL_SetSurfaceColorMod(sources[2], 192, 255, 192));
    GPU_REQUIRE(SDL_FillSurfaceRect(sources[3],
                                    NULL,
                                    SDL_MapSurfaceRGBA(sources[3], 32, 192, 64, SDL_ALPHA_OPAQUE)));

    /* The production minimap can be the first GPU map target. Keep that path
     * covered before the primary map allocates the shared projected-light
     * binding. */
    gpu_renderer_statistics_reset();
    GPU_REQUIRE(auxiliary_first_map_checkpoint(source));

    SDL_Rect transparency_source = {0, 0, 3, 1};
    SDL_Surface *keyed_rgb = gpu_keyed_surface(SDL_PIXELFORMAT_RGB24, 3);
    SDL_Surface *keyed_xrgb = gpu_keyed_surface(SDL_PIXELFORMAT_XRGB8888, 513);
    SDL_Surface *indexed_alpha = gpu_indexed_alpha_surface(3);
    SDL_Surface *indexed_alpha_standalone = gpu_indexed_alpha_surface(513);
    SDL_Surface *opaque_rgb = gpu_opaque_rgb_surface(513);
    SDL_Surface *opaque_indexed = gpu_opaque_indexed_surface(3);
    GPU_REQUIRE(keyed_rgb != NULL && keyed_xrgb != NULL && indexed_alpha != NULL &&
                indexed_alpha_standalone != NULL && opaque_rgb != NULL && opaque_indexed != NULL);
    /* The 3-pixel sources exercise an atlas upload; width 513 exceeds the
     * renderer's 512-pixel UI-atlas entry limit and exercises standalone
     * textures while the source rectangle keeps the readback compact. */
    GPU_REQUIRE(gpu_transparency_checkpoint(keyed_rgb,
                                            &transparency_source,
                                            "keyed RGB atlas via canvas",
                                            true,
                                            true,
                                            false,
                                            true));
    GPU_REQUIRE(gpu_transparency_checkpoint(keyed_xrgb,
                                            &transparency_source,
                                            "keyed XRGB standalone",
                                            false,
                                            true,
                                            false,
                                            false));
    GPU_REQUIRE(gpu_transparency_checkpoint(indexed_alpha,
                                            &transparency_source,
                                            "indexed alpha atlas",
                                            false,
                                            true,
                                            true,
                                            false));
    GPU_REQUIRE(gpu_transparency_checkpoint(indexed_alpha_standalone,
                                            &transparency_source,
                                            "indexed alpha standalone",
                                            false,
                                            true,
                                            true,
                                            false));
    GPU_REQUIRE(gpu_transparency_checkpoint(opaque_rgb,
                                            &transparency_source,
                                            "opaque RGB standalone",
                                            false,
                                            false,
                                            false,
                                            false));
    GPU_REQUIRE(gpu_transparency_checkpoint(opaque_indexed,
                                            &transparency_source,
                                            "opaque indexed atlas",
                                            true,
                                            false,
                                            false,
                                            false));
    SDL_DestroySurface(keyed_rgb);
    SDL_DestroySurface(keyed_xrgb);
    SDL_DestroySurface(indexed_alpha);
    SDL_DestroySurface(indexed_alpha_standalone);
    SDL_DestroySurface(opaque_rgb);
    SDL_DestroySurface(opaque_indexed);

    gpu_map_renderer_invalidate_target(false);
    gpu_renderer_statistics_reset();
    GPU_REQUIRE(draw_checkpoint(source, 0, 255, 0, 2048));
    gpu_renderer_statistics_t warmup;
    gpu_renderer_statistics_get(&warmup);
    GPU_REQUIRE(warmup.upload_count > 0);
    GPU_REQUIRE(warmup.resource_creations > 0);
    GPU_REQUIRE(warmup.batches > 0);
    GPU_REQUIRE(warmup.commands > warmup.batches);
    GPU_REQUIRE(warmup.draws >= warmup.batches);
    GPU_REQUIRE(warmup.timings[GPU_RENDERER_TIMING_COMPLETION].calls == 1);
    GPU_REQUIRE(warmup.map_full_redraws == 1);
    GPU_REQUIRE(warmup.map_last_invalidation_reason ==
                    GPU_RENDERER_MAP_INVALIDATION_MAP_PUBLICATION ||
                warmup.map_last_invalidation_reason == GPU_RENDERER_MAP_INVALIDATION_RESIZE);
    GPU_REQUIRE(strcmp(gpu_renderer_map_invalidation_reason_name(
                           warmup.map_last_invalidation_reason),
                       warmup.map_last_invalidation_reason ==
                               GPU_RENDERER_MAP_INVALIDATION_MAP_PUBLICATION
                           ? "map_publication"
                           : "resize") == 0);

    GPU_REQUIRE(draw_checkpoint(source, 0, 255, 0, 2048));
    gpu_renderer_statistics_t retained;
    gpu_renderer_statistics_get(&retained);
    GPU_REQUIRE(retained.upload_count == warmup.upload_count);
    GPU_REQUIRE(retained.upload_bytes == warmup.upload_bytes);
    GPU_REQUIRE(retained.slot_uniform_upload_count == warmup.slot_uniform_upload_count);
    GPU_REQUIRE(retained.slot_uniform_upload_bytes == warmup.slot_uniform_upload_bytes);
    GPU_REQUIRE(retained.source_upload_count == warmup.source_upload_count);
    GPU_REQUIRE(retained.instance_upload_count == warmup.instance_upload_count);
    GPU_REQUIRE(retained.light_upload_count == warmup.light_upload_count);
    GPU_REQUIRE(retained.resource_creations == warmup.resource_creations);
    GPU_REQUIRE(retained.retained_bytes == warmup.retained_bytes);
    GPU_REQUIRE(retained.timings[GPU_RENDERER_TIMING_COMPLETION].calls ==
                warmup.timings[GPU_RENDERER_TIMING_COMPLETION].calls + 1U);
    GPU_REQUIRE(retained.map_full_redraws == warmup.map_full_redraws);
    GPU_REQUIRE(retained.map_damage_frames == warmup.map_damage_frames);
    GPU_REQUIRE(retained.map_damage_pixels == warmup.map_damage_pixels);
    GPU_REQUIRE(retained.map_damage_bytes == warmup.map_damage_bytes);
    GPU_REQUIRE(retained.map_retained_frames == warmup.map_retained_frames + 1U);
    GPU_REQUIRE(retained.map_skipped_passes == warmup.map_skipped_passes + 1U);
    GPU_REQUIRE(retained.map_published_generation == warmup.map_published_generation);
    GPU_REQUIRE(retained.map_source_generation == warmup.map_source_generation);
    GPU_REQUIRE(retained.map_camera_generation == warmup.map_camera_generation);
    GPU_REQUIRE(retained.map_lighting_generation == warmup.map_lighting_generation);
    GPU_REQUIRE(retained.map_effect_generation == warmup.map_effect_generation);
    GPU_REQUIRE(retained.map_last_invalidation_reason == GPU_RENDERER_MAP_INVALIDATION_UNCHANGED);
    GPU_REQUIRE(retained.map_invalidation_counts[GPU_RENDERER_MAP_INVALIDATION_UNCHANGED] == 1);
    GPU_REQUIRE(retained_damage_checkpoint(source));
    GPU_REQUIRE(async_map_submission_checkpoint(source));
    GPU_REQUIRE(async_fence_failure_checkpoint(window, source, qualified));
    GPU_REQUIRE(instance_delta_upload_checkpoint(source));
    GPU_REQUIRE(light_delta_upload_checkpoint(source));
    GPU_REQUIRE(atlas_batch_checkpoint(source, sources[3]));
    GPU_REQUIRE(slot_fragmentation_checkpoint(source));
    GPU_REQUIRE(retained_identity_delta_checkpoint(source));
    GPU_REQUIRE(queued_asset_invalidation_checkpoint());
    GPU_REQUIRE(atlas_churn_plateau_checkpoint());
    GPU_REQUIRE(atlas_allocation_fault_checkpoint());
    GPU_REQUIRE(ui_atlas_fault_checkpoint());
    GPU_REQUIRE(ui_atlas_churn_plateau_checkpoint());
    GPU_REQUIRE(draw_checkpoint(source, 0, 255, 0, 2048));
    GPU_REQUIRE(lighting_lookup_checkpoint(source));
    GPU_REQUIRE(projected_lighting_checkpoint(source, 32.0f));
    gpu_renderer_statistics_reset();
    GPU_REQUIRE(projected_lighting_checkpoint(source, 32.0f));
    gpu_renderer_statistics_t projected_stable;
    gpu_renderer_statistics_get(&projected_stable);
    GPU_REQUIRE(projected_stable.projected_light_upload_count == 0);
    GPU_REQUIRE(projected_stable.projected_light_upload_bytes == 0);
    gpu_renderer_statistics_reset();
    GPU_REQUIRE(projected_lighting_checkpoint(source, 31.0f));
    gpu_renderer_statistics_t projected_changed;
    gpu_renderer_statistics_get(&projected_changed);
    GPU_REQUIRE(projected_changed.projected_light_upload_count == 1);
    GPU_REQUIRE(projected_changed.projected_light_upload_bytes == sizeof(uint32_t));
    GPU_REQUIRE(retained_primary_auxiliary_checkpoint(source, sources[3], false));
    GPU_REQUIRE(gpu_map_renderer_texture(false) != NULL);
    GPU_REQUIRE(gpu_map_renderer_texture(true) != NULL);
    gpu_map_renderer_invalidate_target(false);
    gpu_map_renderer_invalidate_target(true);
    GPU_REQUIRE(gpu_map_renderer_texture(false) == NULL);
    GPU_REQUIRE(gpu_map_renderer_texture(true) == NULL);
    gpu_renderer_statistics_reset();
    GPU_REQUIRE(retained_primary_auxiliary_checkpoint(source, sources[3], true));

    GPU_REQUIRE(canvas_registration_fault_checkpoint());
    GPU_REQUIRE(recover_and_republish(window, source, qualified, 2048));
    GPU_REQUIRE(canvas_registration_retry_checkpoint());

    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_ALLOCATION);
    GPU_REQUIRE(!draw_checkpoint(source, 255, 0, 255, 2048));
    GPU_REQUIRE(!gpu_renderer_frame_valid());
    GPU_REQUIRE(recover_and_republish(window, source, qualified, 2048));

    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_TARGET);
    GPU_REQUIRE(target_resize_fault_checkpoint());
    GPU_REQUIRE(recover_and_republish(window, source, qualified, 2048));

    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_UPLOAD);
    GPU_REQUIRE(!draw_checkpoint(source, 255, 0, 255, 1024));
    GPU_REQUIRE(!gpu_renderer_frame_valid());
    GPU_REQUIRE(recover_and_republish(window, source, qualified, 1024));

    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_SUBMISSION);
    GPU_REQUIRE(!draw_checkpoint(source, 255, 0, 255, 1536));
    GPU_REQUIRE(!gpu_renderer_frame_valid());
    GPU_REQUIRE(recover_and_republish(window, source, qualified, 1536));

    SDL_Rect invalid_readback = {-1, 0, 1, 1};
    GPU_REQUIRE(gpu_renderer_readback(&invalid_readback) == NULL);
    SDL_ClearError();

    SDL_Rect map_screenshot = {8, 8, 32, 24};
    SDL_Surface *map_screenshot_surface = gpu_renderer_readback(&map_screenshot);
    GPU_REQUIRE(map_screenshot_surface != NULL);
    GPU_REQUIRE(map_screenshot_surface->w == map_screenshot.w);
    GPU_REQUIRE(map_screenshot_surface->h == map_screenshot.h);
    SDL_DestroySurface(map_screenshot_surface);

    async_readback_result_t asynchronous = {0};
    GPU_REQUIRE(gpu_renderer_readback_async(&map_screenshot,
                                            async_readback_complete,
                                            async_readback_cancel,
                                            &asynchronous));
    GPU_REQUIRE(!asynchronous.called);
    GPU_REQUIRE(gpu_renderer_conformance_wait_idle());
    gpu_renderer_readback_poll();
    GPU_REQUIRE(asynchronous.called && !asynchronous.canceled && asynchronous.surface != NULL);
    GPU_REQUIRE(asynchronous.surface->w == map_screenshot.w);
    GPU_REQUIRE(asynchronous.surface->h == map_screenshot.h);
    SDL_DestroySurface(asynchronous.surface);

    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_READBACK);
    GPU_REQUIRE(!draw_checkpoint(source, 255, 0, 255, 2048));
    GPU_REQUIRE(recover_and_republish(window, source, qualified, 2048));

    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_SWAPCHAIN);
    GPU_REQUIRE(!draw_checkpoint(source, 255, 0, 255, 2048));
    GPU_REQUIRE(!gpu_renderer_frame_valid());
    GPU_REQUIRE(recover_and_republish(window, source, qualified, 2048));

    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_DEVICE_LOSS);
    GPU_REQUIRE(!draw_checkpoint(source, 255, 0, 255, 2048));
    GPU_REQUIRE(!gpu_renderer_frame_valid());
    GPU_REQUIRE(recover_and_republish(window, source, qualified, 2048));

    GPU_REQUIRE(SDL_SetWindowSize(window, 80, 72));
    GPU_REQUIRE(SDL_SyncWindow(window));
    int logical_width, logical_height, output_width, output_height;
    GPU_REQUIRE(SDL_GetWindowSize(window, &logical_width, &logical_height));
    GPU_REQUIRE(logical_width == 80 && logical_height == 72);
    gpu_renderer_recreation_request();
    GPU_REQUIRE(gpu_renderer_recreation_take_request());
    GPU_REQUIRE(recover_and_republish(window, source, qualified, 2048));
    GPU_REQUIRE(gpu_renderer_output_size(&output_width, &output_height));
    GPU_REQUIRE(output_width == 80 && output_height == 72);
    GPU_REQUIRE(SDL_SetWindowSize(window, 64, 64));
    GPU_REQUIRE(SDL_SyncWindow(window));
    GPU_REQUIRE(SDL_GetWindowSize(window, &logical_width, &logical_height));
    GPU_REQUIRE(logical_width == 64 && logical_height == 64);
    gpu_renderer_recreation_request();
    GPU_REQUIRE(gpu_renderer_recreation_take_request());
    GPU_REQUIRE(recover_and_republish(window, source, qualified, 2048));
    GPU_REQUIRE(gpu_renderer_output_size(&output_width, &output_height));
    GPU_REQUIRE(output_width == 64 && output_height == 64);

    gpu_renderer_statistics_t before_callback_failure;
    gpu_renderer_statistics_get(&before_callback_failure);
    unsigned int callback_attempts = 0;
    GPU_REQUIRE(!gpu_renderer_recover_and_republish(window,
                                                    &callback_attempts,
                                                    !qualified,
                                                    recovery_window_apply,
                                                    recovery_scene_republish_failure,
                                                    NULL));
    GPU_REQUIRE(callback_attempts == 1U);
    GPU_REQUIRE(!gpu_renderer_ready());
    gpu_renderer_statistics_t after_callback_failure;
    gpu_renderer_statistics_get(&after_callback_failure);
    GPU_REQUIRE(after_callback_failure.recovery_failures ==
                before_callback_failure.recovery_failures + 1U);
    callback_attempts = 0;
    recovery_scene_t callback_recovery_scene = {.source = source, .light_level = 2048};
    GPU_REQUIRE(gpu_renderer_recover_and_republish(window,
                                                   &callback_attempts,
                                                   !qualified,
                                                   recovery_window_apply,
                                                   recovery_scene_republish,
                                                   &callback_recovery_scene));

    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_SHADER);
    unsigned int attempts = 0;
    recovery_scene_t recovery_scene = {.source = source, .light_level = 2048};
    GPU_REQUIRE(!gpu_renderer_recover_and_republish(window,
                                                    &attempts,
                                                    !qualified,
                                                    recovery_window_apply,
                                                    recovery_scene_republish,
                                                    &recovery_scene));
    GPU_REQUIRE(attempts == 1U);
    GPU_REQUIRE(!gpu_renderer_recover_and_republish(window,
                                                    &attempts,
                                                    !qualified,
                                                    recovery_window_apply,
                                                    recovery_scene_republish,
                                                    &recovery_scene));
    GPU_REQUIRE(!gpu_renderer_ready());
    attempts = 0;
    GPU_REQUIRE(gpu_renderer_recover_and_republish(window,
                                                   &attempts,
                                                   !qualified,
                                                   recovery_window_apply,
                                                   recovery_scene_republish,
                                                   &recovery_scene));
    gpu_renderer_statistics_t recovered;
    gpu_renderer_statistics_get(&recovered);
    GPU_REQUIRE(recovered.device_recoveries >= 12);
    GPU_REQUIRE(recovered.recovery_failures >= 2);
    GPU_REQUIRE(draw_checkpoint(source, 0, 0, 255, 2048));
    SDL_Surface *final_checkpoint_surface = gpu_renderer_readback(NULL);
    char final_checkpoint[65];
    GPU_REQUIRE(final_checkpoint_surface != NULL &&
                surface_sha256(final_checkpoint_surface, final_checkpoint));
    SDL_DestroySurface(final_checkpoint_surface);
    GPU_REQUIRE(final_checkpoint[0] != '\0');

    async_readback_result_t canceled = {0};
    GPU_REQUIRE(gpu_renderer_readback_async(&map_screenshot,
                                            async_readback_complete,
                                            async_readback_cancel,
                                            &canceled));
    GPU_REQUIRE(!canceled.called && !canceled.canceled);
    gpu_renderer_destroy();
    GPU_REQUIRE(!canceled.called && canceled.canceled && canceled.surface == NULL);
    GPU_REQUIRE(!gpu_renderer_ready());
    GPU_REQUIRE(strcmp(gpu_renderer_backend(), "") == 0);
    for (size_t i = 0; i < SDL_arraysize(sources); i++) {
        SDL_DestroySurface(sources[i]);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
#undef GPU_REQUIRE
    return EXIT_SUCCESS;
}
