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
#include <stdint.h>

typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_FRect SDL_FRect;
typedef struct SDL_Rect SDL_Rect;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Surface SDL_Surface;
typedef struct SDL_Texture SDL_Texture;
typedef struct lighting_vertex lighting_vertex_t;

bool gpu_map_renderer_create(SDL_GPUDevice *device, SDL_Renderer *renderer);
void gpu_map_renderer_destroy(void);
bool gpu_map_renderer_begin(int width, int height);
bool gpu_map_renderer_active(void);
void gpu_map_renderer_set_owner(uint8_t owner, int sample_y);
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
SDL_Texture *gpu_map_renderer_texture(void);
void gpu_map_renderer_invalidate_surface(SDL_Surface *surface);

#endif
