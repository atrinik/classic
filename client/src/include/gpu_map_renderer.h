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

#ifndef GPU_MAP_RENDERER_H
#define GPU_MAP_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lighting.h>

typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_FRect SDL_FRect;
typedef struct SDL_Rect SDL_Rect;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Surface SDL_Surface;
typedef struct SDL_Texture SDL_Texture;
typedef enum gpu_renderer_map_invalidation_reason gpu_renderer_map_invalidation_reason_t;
bool gpu_map_renderer_create(SDL_GPUDevice *device, SDL_Renderer *renderer);
void gpu_map_renderer_destroy(void);
bool gpu_map_renderer_begin(int width, int height, bool auxiliary);
/** Reuse a complete published target without opening a command buffer. */
bool gpu_map_renderer_retain(int width, int height, bool auxiliary);
bool gpu_map_renderer_active(void);
void gpu_map_renderer_set_invalidation_hint(gpu_renderer_map_invalidation_reason_t reason);
void gpu_map_renderer_set_owner(uint8_t owner, int sample_y, bool projected);
/** Bind the stable semantic record identity for the next world draw. */
void gpu_map_renderer_set_instance_identity(uint64_t record_identity, uint32_t draw_variant);
void gpu_map_renderer_light_quad(uint8_t owner, const lighting_vertex_t vertices[4]);
bool gpu_map_renderer_draw_surface(SDL_Surface *surface,
                                   const SDL_Rect *source,
                                   const SDL_FRect *destination);
bool gpu_map_renderer_draw_rect(const SDL_FRect *destination,
                                uint8_t red,
                                uint8_t green,
                                uint8_t blue,
                                uint8_t alpha,
                                bool filled);
bool gpu_map_renderer_set_clip(const SDL_Rect *rectangle);
bool gpu_map_renderer_end(void);
SDL_Texture *gpu_map_renderer_texture(bool auxiliary);
/** Mark a retained target unavailable until its next successful publication. */
void gpu_map_renderer_invalidate_target(bool auxiliary);
void gpu_map_renderer_invalidate_surface(SDL_Surface *surface);
/** Poll submitted map work without blocking and retire signaled resources. */
void gpu_map_renderer_poll(void);
/** Wait for all submitted map work during an explicit lifecycle transition. */
bool gpu_map_renderer_wait_idle(void);
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
typedef struct gpu_map_renderer_probe {
    uint8_t albedo[4];
    uint32_t lighting_key;
    uint16_t light[4];
    uint8_t final_color[4];
} gpu_map_renderer_probe_t;

bool gpu_map_renderer_probe(int x, int y, uint8_t light_owner, gpu_map_renderer_probe_t *probe);
size_t gpu_map_renderer_target_payload_bytes(int width, int height, uint8_t active_depths);
size_t gpu_map_renderer_target_retained_bytes(int width, int height, uint8_t active_depths);
size_t gpu_map_renderer_atlas_page_count(void);
size_t gpu_map_renderer_atlas_allocation_count(void);
/** Count active retained primary or auxiliary instances with compact-light owners. */
size_t gpu_map_renderer_lit_instance_count(bool auxiliary);
/** Return the number of submitted map command buffers not yet retired. */
size_t gpu_map_renderer_pending_submission_count(void);
#endif

#endif
