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
typedef enum SDL_ScaleMode SDL_ScaleMode;
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_Rect SDL_Rect;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Surface SDL_Surface;
typedef struct SDL_Window SDL_Window;
typedef struct lighting_vertex lighting_vertex_t;

#define GPU_RENDERER_STATISTICS_VERSION UINT8_C(3)
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

/** Named causes used to classify a retained map frame. */
typedef enum gpu_renderer_map_invalidation_reason {
    GPU_RENDERER_MAP_INVALIDATION_UNCHANGED,
    GPU_RENDERER_MAP_INVALIDATION_ANIMATION,
    GPU_RENDERER_MAP_INVALIDATION_ACTOR_EFFECT,
    GPU_RENDERER_MAP_INVALIDATION_CAMERA_SCROLL,
    GPU_RENDERER_MAP_INVALIDATION_MAP_PUBLICATION,
    GPU_RENDERER_MAP_INVALIDATION_LIGHTING,
    GPU_RENDERER_MAP_INVALIDATION_RESIZE,
    GPU_RENDERER_MAP_INVALIDATION_RESOURCE_REPLACEMENT,
    GPU_RENDERER_MAP_INVALIDATION_RESET,
    GPU_RENDERER_MAP_INVALIDATION_DEVICE_RECOVERY,

    GPU_RENDERER_MAP_INVALIDATION_REASON_NUM
} gpu_renderer_map_invalidation_reason_t;

/** Per-frame retained-map contract recorded with renderer statistics. */
typedef struct gpu_renderer_map_frame_diagnostics {
    gpu_renderer_map_invalidation_reason_t invalidation_reason;
    uint64_t published_generation;
    uint64_t source_generation;
    uint64_t camera_generation;
    uint64_t lighting_generation;
    uint64_t effect_generation;
    uint64_t dirty_commands;
    int32_t dirty_x;
    int32_t dirty_y;
    int32_t dirty_width;
    int32_t dirty_height;
} gpu_renderer_map_frame_diagnostics_t;

typedef struct gpu_renderer_statistics {
    gpu_renderer_timing_t timings[GPU_RENDERER_TIMING_NUM];
    uint64_t commands;
    uint64_t batches;
    uint64_t draws;
    uint64_t upload_count;
    uint64_t upload_bytes;
    uint64_t source_upload_count;
    uint64_t source_upload_bytes;
    uint64_t instance_upload_count;
    uint64_t instance_upload_bytes;
    uint64_t light_upload_count;
    uint64_t light_upload_bytes;
    uint64_t projected_light_upload_count;
    uint64_t projected_light_upload_bytes;
    uint64_t slot_uniform_upload_count;
    uint64_t slot_uniform_upload_bytes;
    uint64_t resource_creations;
    uint64_t resource_destructions;
    uint64_t retained_bytes;
    uint64_t peak_retained_bytes;
    uint64_t device_recoveries;
    uint64_t recovery_failures;
    uint64_t readbacks;
    uint64_t fallbacks;
    uint64_t map_full_redraws;
    uint64_t map_damage_frames;
    uint64_t map_damage_pixels;
    uint64_t map_damage_bytes;
    uint64_t map_retained_frames;
    uint64_t map_skipped_passes;
    uint64_t map_dirty_commands;
    uint64_t map_dirty_pixels;
    uint64_t map_dirty_bytes;
    uint64_t map_published_generation;
    uint64_t map_source_generation;
    uint64_t map_camera_generation;
    uint64_t map_lighting_generation;
    uint64_t map_effect_generation;
    uint64_t map_last_dirty_commands;
    uint64_t map_last_dirty_pixels;
    uint64_t map_last_dirty_bytes;
    int32_t map_last_dirty_x;
    int32_t map_last_dirty_y;
    int32_t map_last_dirty_width;
    int32_t map_last_dirty_height;
    gpu_renderer_map_invalidation_reason_t map_last_invalidation_reason;
    uint64_t map_invalidation_counts[GPU_RENDERER_MAP_INVALIDATION_REASON_NUM];
} gpu_renderer_statistics_t;

bool gpu_renderer_create(SDL_Window *window);
bool gpu_renderer_recover(SDL_Window *window);
/** Perform at most one complete device reconstruction for the current failure. */
bool gpu_renderer_recover_bounded(SDL_Window *window,
                                  unsigned int *attempts,
                                  bool conformance_device);
typedef bool (*gpu_renderer_recovery_step_fn)(void *userdata);
/**
 * Recreate the device, reapply window state, and publish one complete frame
 * as a single bounded recovery transaction.
 */
bool gpu_renderer_recover_and_republish(SDL_Window *window,
                                        unsigned int *attempts,
                                        bool conformance_device,
                                        gpu_renderer_recovery_step_fn apply_window_state,
                                        gpu_renderer_recovery_step_fn republish_complete_frame,
                                        void *userdata);
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
typedef enum gpu_renderer_conformance_fault {
    GPU_RENDERER_CONFORMANCE_FAULT_NONE,
    GPU_RENDERER_CONFORMANCE_FAULT_ALLOCATION,
    GPU_RENDERER_CONFORMANCE_FAULT_SHADER,
    GPU_RENDERER_CONFORMANCE_FAULT_TARGET,
    GPU_RENDERER_CONFORMANCE_FAULT_UPLOAD,
    GPU_RENDERER_CONFORMANCE_FAULT_SUBMISSION,
    GPU_RENDERER_CONFORMANCE_FAULT_SWAPCHAIN,
    GPU_RENDERER_CONFORMANCE_FAULT_DEVICE_LOSS,
    GPU_RENDERER_CONFORMANCE_FAULT_READBACK,
    GPU_RENDERER_CONFORMANCE_FAULT_UI_ATLAS_UPLOAD,
    GPU_RENDERER_CONFORMANCE_FAULT_CANVAS_REGISTRATION,
} gpu_renderer_conformance_fault_t;

/** Test-only CPU-emulated GPU entry point; production always requires hardware. */
bool gpu_renderer_conformance_available(void);
bool gpu_renderer_create_conformance(SDL_Window *window);
bool gpu_renderer_recover_conformance(SDL_Window *window);
bool gpu_renderer_conformance_wait_idle(void);
void gpu_renderer_conformance_fault_set(gpu_renderer_conformance_fault_t fault);
bool gpu_renderer_conformance_fault_take(gpu_renderer_conformance_fault_t fault);
size_t gpu_renderer_atlas_page_count(void);
size_t gpu_renderer_atlas_allocation_count(void);
#endif
void gpu_renderer_recreation_request(void);
bool gpu_renderer_recreation_take_request(void);
void gpu_renderer_destroy(void);
bool gpu_renderer_ready(void);
/** True only when production creation authoritatively rejected software adapters. */
bool gpu_renderer_hardware_verified(void);
/** Flush all submitted 2D work and wait for the shared GPU device to become idle. */
bool gpu_renderer_wait_idle(void);
SDL_Renderer *gpu_renderer_sdl(void);
SDL_GPUDevice *gpu_renderer_device(void);
const char *gpu_renderer_backend(void);
const char *gpu_renderer_device_name(void);
const char *gpu_renderer_driver_name(void);
const char *gpu_renderer_driver_version(void);
bool gpu_renderer_output_size(int *width, int *height);
bool gpu_renderer_begin_frame(void);
bool gpu_renderer_present(void);
bool gpu_renderer_frame_valid(void);
bool gpu_renderer_map_begin(int width, int height);
/** Begin the independently retained auxiliary/minimap map target. */
bool gpu_renderer_map_begin_auxiliary(int width, int height);
/** Reuse a complete published map target without issuing map GPU work. */
bool gpu_renderer_map_retain(int width, int height);
/** Supply the production map invalidation reason for the next map target. */
void gpu_renderer_map_set_invalidation_hint(gpu_renderer_map_invalidation_reason_t reason);
void gpu_renderer_map_set_owner(uint8_t owner, int sample_y, bool projected);
/** Bind the stable semantic map-record identity for the next painter draw. */
void gpu_renderer_map_set_instance_identity(uint64_t record_identity, uint32_t draw_variant);
void gpu_renderer_map_light_quad(uint8_t owner, const lighting_vertex_t vertices[4]);
bool gpu_renderer_map_end(void);
bool gpu_renderer_draw_map(float x, float y, float width, float height);
/** Return whether a primary retained map generation is safe to display. */
bool gpu_renderer_map_available(void);
bool gpu_renderer_draw_map_to(SDL_Surface *target,
                              const SDL_FRect *source,
                              const SDL_FRect *destination,
                              SDL_ScaleMode scale_mode);
bool gpu_renderer_draw_surface(SDL_Surface *surface,
                               const SDL_Rect *source,
                               const SDL_FRect *destination);
bool gpu_renderer_draw_surface_to(SDL_Surface *target,
                                  SDL_Surface *surface,
                                  const SDL_Rect *source,
                                  const SDL_FRect *destination);
bool gpu_renderer_draw_surface_scaled_to(SDL_Surface *target,
                                         SDL_Surface *surface,
                                         const SDL_Rect *source,
                                         const SDL_FRect *destination,
                                         SDL_ScaleMode scale_mode);
/** Register an owned canvas, destroying and nulling it on any failure. */
bool gpu_renderer_canvas_register(SDL_Surface **surface);
bool gpu_renderer_canvas_registered(SDL_Surface *surface);
bool gpu_renderer_canvas_fill(SDL_Surface *surface,
                              const SDL_Rect *rectangle,
                              Uint8 red,
                              Uint8 green,
                              Uint8 blue,
                              Uint8 alpha);
bool gpu_renderer_canvas_draw_rect(SDL_Surface *surface,
                                   const SDL_FRect *rectangle,
                                   Uint8 red,
                                   Uint8 green,
                                   Uint8 blue,
                                   Uint8 alpha,
                                   bool filled);
bool gpu_renderer_canvas_draw_line(SDL_Surface *surface,
                                   float x1,
                                   float y1,
                                   float x2,
                                   float y2,
                                   Uint8 red,
                                   Uint8 green,
                                   Uint8 blue,
                                   Uint8 alpha);
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
/** Callback for a completed asynchronous frame readback; owns surface. */
typedef void (*gpu_renderer_readback_callback_t)(SDL_Surface *surface, void *userdata);
/** Callback for an asynchronous frame readback canceled by renderer teardown. */
typedef void (*gpu_renderer_readback_cancel_callback_t)(void *userdata);
/** Enqueue a completed-frame readback without waiting for GPU completion. */
bool gpu_renderer_readback_async(const SDL_Rect *rect,
                                 gpu_renderer_readback_callback_t callback,
                                 gpu_renderer_readback_cancel_callback_t cancel_callback,
                                 void *userdata);
/** Dispatch completed asynchronous readbacks from the client thread. */
void gpu_renderer_readback_poll(void);
/** Explicit synchronous readback reserved for checkpoints and conformance. */
SDL_Surface *gpu_renderer_readback(const SDL_Rect *rect);

/** Return the retained upload generation associated with a source surface. */
Uint64 gpu_renderer_surface_generation(SDL_Surface *surface);

void gpu_renderer_statistics_reset(void);
void gpu_renderer_statistics_get(gpu_renderer_statistics_t *statistics);
uint64_t gpu_renderer_timing_begin(void);
void gpu_renderer_timing_end(gpu_renderer_timing_stage_t stage, uint64_t started_ns);
void gpu_renderer_statistics_commands(uint64_t commands, uint64_t batches, uint64_t draws);
void gpu_renderer_statistics_source_upload(size_t bytes);
void gpu_renderer_statistics_instance_upload(size_t bytes);
void gpu_renderer_statistics_light_upload(size_t bytes);
/** Record the projected-row subset of compact light uploads. */
void gpu_renderer_statistics_projected_light_upload(size_t bytes);
void gpu_renderer_statistics_slot_uniform_upload(size_t bytes);
void gpu_renderer_statistics_resource_create(size_t retained_bytes);
void gpu_renderer_statistics_resource_destroy(size_t retained_bytes);
void gpu_renderer_statistics_map_frame(bool full_redraw,
                                       bool damage,
                                       size_t damage_pixels,
                                       size_t damage_bytes,
                                       bool skipped_pass,
                                       const gpu_renderer_map_frame_diagnostics_t *diagnostics);
const char *gpu_renderer_map_invalidation_reason_name(
    gpu_renderer_map_invalidation_reason_t reason);
void gpu_renderer_statistics_recovery(bool succeeded);

#endif
