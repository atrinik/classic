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

#ifndef GPU_RENDERER_H
#define GPU_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SDL_FRect SDL_FRect;
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_Rect SDL_Rect;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Surface SDL_Surface;
typedef struct SDL_Window SDL_Window;

#define GPU_RENDERER_STATISTICS_VERSION UINT8_C(1)
#define GPU_RENDERER_OWNER_UNLIT (UINT8_MAX - UINT8_C(1))

typedef enum gpu_renderer_timing_stage {
    GPU_RENDERER_TIMING_COMMAND_BUILD,
    GPU_RENDERER_TIMING_ALBEDO_OWNER,
    GPU_RENDERER_TIMING_LIGHT_TONE,
    GPU_RENDERER_TIMING_UI,
    GPU_RENDERER_TIMING_SUBMISSION,
    GPU_RENDERER_TIMING_COMPLETION,
    GPU_RENDERER_TIMING_PRESENT_WAIT,

    GPU_RENDERER_TIMING_NUM
} gpu_renderer_timing_stage_t;

typedef struct gpu_renderer_timing {
    uint64_t calls;
    uint64_t elapsed_ns;
} gpu_renderer_timing_t;

typedef struct gpu_renderer_statistics {
    gpu_renderer_timing_t timings[GPU_RENDERER_TIMING_NUM];
    uint64_t commands;
    uint64_t batches;
    uint64_t draws;
    uint64_t upload_count;
    uint64_t upload_bytes;
    uint64_t resource_creations;
    uint64_t resource_destructions;
    uint64_t retained_bytes;
    uint64_t peak_retained_bytes;
    uint64_t device_recoveries;
    uint64_t recovery_failures;
    uint64_t fallbacks;
} gpu_renderer_statistics_t;

bool gpu_renderer_create(SDL_Window *window);
bool gpu_renderer_recover(SDL_Window *window);
void gpu_renderer_destroy(void);
bool gpu_renderer_ready(void);
SDL_Renderer *gpu_renderer_sdl(void);
SDL_GPUDevice *gpu_renderer_device(void);
const char *gpu_renderer_backend(void);
const char *gpu_renderer_device_name(void);
const char *gpu_renderer_driver_name(void);
const char *gpu_renderer_driver_version(void);
bool gpu_renderer_output_size(int *width, int *height);
bool gpu_renderer_begin_frame(void);
bool gpu_renderer_present(void);
bool gpu_renderer_map_begin(int width, int height);
void gpu_renderer_map_set_owner(uint8_t owner);
bool gpu_renderer_map_end(void);
bool gpu_renderer_draw_map(float x, float y, float width, float height);
bool gpu_renderer_draw_surface(SDL_Surface *surface,
                               const SDL_Rect *source,
                               const SDL_FRect *destination);
bool gpu_renderer_draw_rect(const SDL_FRect *rectangle,
                            Uint8 red,
                            Uint8 green,
                            Uint8 blue,
                            Uint8 alpha,
                            bool filled);
bool gpu_renderer_draw_line(float x1,
                            float y1,
                            float x2,
                            float y2,
                            Uint8 red,
                            Uint8 green,
                            Uint8 blue,
                            Uint8 alpha);
bool gpu_renderer_set_clip(const SDL_Rect *rectangle);
void gpu_renderer_invalidate_surface(SDL_Surface *surface);
void gpu_renderer_surface_changed(SDL_Surface *surface);
SDL_Surface *gpu_renderer_readback(const SDL_Rect *rect);

void gpu_renderer_statistics_reset(void);
void gpu_renderer_statistics_get(gpu_renderer_statistics_t *statistics);
uint64_t gpu_renderer_timing_begin(void);
void gpu_renderer_timing_end(gpu_renderer_timing_stage_t stage, uint64_t started_ns);
void gpu_renderer_statistics_commands(uint64_t commands, uint64_t batches, uint64_t draws);
void gpu_renderer_statistics_recovery(bool succeeded);

#endif
