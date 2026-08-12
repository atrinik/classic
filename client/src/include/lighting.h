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

#ifndef LIGHTING_H
#define LIGHTING_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SDL_Rect SDL_Rect;
typedef struct SDL_Surface SDL_Surface;

/** Increment when cached lighting transfer semantics change. */
#define LIGHTING_TRANSFER_VERSION UINT8_C(1)
#define LIGHTING_SPRITE_CACHE_MAX_BYTES (8U * 1024U * 1024U)
#define LIGHTING_SPRITE_CACHE_ENTRY_OVERHEAD 512U
#define LIGHTING_SPRITE_CACHE_MAX_ENTRIES 8192U

/** Conservatively charge pixels plus retained entry/surface/allocator metadata. */
static inline size_t lighting_sprite_cache_charge(size_t pitch, size_t height) {
    if (height != 0 && pitch > (SIZE_MAX - LIGHTING_SPRITE_CACHE_ENTRY_OVERHEAD) / height) {
        return SIZE_MAX;
    }
    return pitch * height + LIGHTING_SPRITE_CACHE_ENTRY_OVERHEAD;
}

typedef enum lighting_surface_mode {
    LIGHTING_SURFACE_STRUCTURE,
    LIGHTING_SURFACE_PROJECTED,
} lighting_surface_mode_t;

/** One light sample projected into map-widget coordinates. */
typedef struct lighting_vertex {
    int x;
    int y;
    uint16_t scalar;
    uint16_t red;
    uint16_t green;
    uint16_t blue;
} lighting_vertex_t;

uint16_t lighting_srgb8_to_linear(uint8_t value);
uint8_t lighting_linear_to_srgb8(uint16_t value);
uint8_t lighting_radiance_to_level(uint16_t radiance);
void lighting_tone_map_linear(uint16_t scalar,
                              const uint16_t radiance[3],
                              uint16_t linear[3]);
uint8_t lighting_multiply_channel(uint8_t source, uint16_t illumination_linear);

/** Bilinearly interpolate one Q5.11 channel without overflowing its bounds. */
static inline uint16_t lighting_bilinear_channel(uint16_t upper_left,
                                                 uint16_t upper_right,
                                                 uint16_t lower_right,
                                                 uint16_t lower_left,
                                                 uint64_t u,
                                                 uint64_t v,
                                                 uint64_t scale) {
    assert(scale != 0);
    assert(u <= scale);
    assert(v <= scale);
    assert(scale <= UINT64_MAX / (UINT64_C(1) + UINT16_MAX));

    uint64_t inverse_u = scale - u;
    uint64_t inverse_v = scale - v;
    uint64_t upper = (upper_left * inverse_u + upper_right * u + scale / 2) / scale;
    uint64_t lower = (lower_left * inverse_u + lower_right * u + scale / 2) / scale;
    return (uint16_t)((upper * inverse_v + lower * v + scale / 2) / scale);
}

bool lighting_begin(int width, int height, uint64_t cache_key);
bool lighting_select_level(int depth);
void lighting_set_level_mask(uint16_t mask);
void lighting_level_scroll(int dz);
bool lighting_needs_update(void);
void lighting_draw_quad(const lighting_vertex_t vertices[4]);
void lighting_render(SDL_Surface *destination);
void lighting_show_surface(SDL_Surface *destination,
                           int x,
                           int y,
                           SDL_Rect *srcrect,
                           SDL_Surface *source,
                           int sample_y,
                           lighting_surface_mode_t mode);
void lighting_clear_sprite_cache(void);
void lighting_deinit(void);

#endif
