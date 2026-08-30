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
#define LIGHTING_TRANSFER_VERSION UINT8_C(2)
#define LIGHTING_SPRITE_CACHE_MAX_BYTES (8U * 1024U * 1024U)
#define LIGHTING_SPRITE_CACHE_ENTRY_OVERHEAD 512U
/* Retain two translated dense-view working sets while the byte ceiling remains
 * the authoritative bound for large transformed sprites. */
#define LIGHTING_SPRITE_CACHE_MAX_ENTRIES 64U

/** Conservatively charge pixels plus retained entry/surface/allocator metadata.
 */
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

/** Increment when the statistics-only benchmark API changes. */
#define LIGHTING_BENCHMARK_STATISTICS_VERSION UINT8_C(8)

typedef enum lighting_benchmark_reconstruction {
    LIGHTING_BENCHMARK_RECONSTRUCTION_TRANSLATED,
    LIGHTING_BENCHMARK_RECONSTRUCTION_FULL,
} lighting_benchmark_reconstruction_t;

typedef struct lighting_benchmark_timing {
    uint64_t calls;
    uint64_t elapsed_ns;
} lighting_benchmark_timing_t;

/** Non-overlapping pipeline timings. Clock reads are benchmark-only. */
typedef struct lighting_benchmark_timings {
    lighting_benchmark_timing_t translation;
    lighting_benchmark_timing_t dirty_clear;
    lighting_benchmark_timing_t rasterization;
    lighting_benchmark_timing_t extrapolation;
    lighting_benchmark_timing_t tone_map_multiply;
    lighting_benchmark_timing_t sprite_lookup;
    lighting_benchmark_timing_t sprite_construction;
    lighting_benchmark_timing_t sprite_invalidation;
} lighting_benchmark_timings_t;

/** Event counters accumulated for one logical lighting level. */
typedef struct lighting_benchmark_counters {
    uint64_t field_begins;
    uint64_t field_dirty_marks;
    /** Lighting-field pixels invalidated for a subsequent full rebuild. */
    uint64_t field_dirty_pixels;
    /** Quads accepted by the lighting rasterizer. */
    uint64_t field_rasterized_quads;
    uint64_t field_translations;
    /** Logical field pixels remapped during scroll; the ring avoids copying them. */
    uint64_t field_translation_logical_pixels;
    /** Logical byte footprint of remapped pixels, not bytes physically copied. */
    uint64_t field_translation_logical_bytes;
    /** Bytes physically read from retained lighting samples. */
    uint64_t field_physical_read_bytes;
    /** Bytes physically written to retained lighting samples. */
    uint64_t field_physical_written_bytes;
    /** Lighting-sample bytes physically copied while moving a retained field. */
    uint64_t field_physical_copied_bytes;
    /** Lighting-sample bytes physically cleared while invalidating field storage. */
    uint64_t field_physical_cleared_bytes;
    /** Lighting-field bytes uploaded to graphics storage. */
    uint64_t field_physical_uploaded_bytes;
    uint64_t field_scroll_x_pixels;
    uint64_t field_scroll_y_pixels;
    uint64_t field_translation_fallback_active;
    uint64_t field_translation_fallback_bounds;
    uint64_t field_translation_fallback_control;
    uint64_t field_partial_rebuilds;
    uint64_t field_full_rebuilds;
    uint64_t field_full_rebuild_cache;
    uint64_t field_full_rebuild_active;
    uint64_t field_full_rebuild_bounds;
    uint64_t field_full_rebuild_control;
    uint64_t field_full_rebuild_other;
    uint64_t field_rebuilds;
    uint64_t field_reuses;
    uint64_t render_calls;
    uint64_t render_failures;
    /** One final scene-linear composition for a complete primary-map draw. */
    uint64_t whole_field_compositions;
    /** Marked scene pixels visited by the final composition pass. */
    uint64_t whole_field_processed_pixels;
    uint64_t whole_field_pixels;
    uint64_t lit_sprite_draws;
    uint64_t lit_sprite_lookups;
    uint64_t lit_sprite_hits;
    uint64_t lit_sprite_misses;
    uint64_t lit_sprite_insertions;
    uint64_t lit_sprite_evictions;
    uint64_t lit_sprite_fallbacks;
    uint64_t lit_sprite_clears;
    uint64_t lit_sprite_cleared_entries;
    uint64_t lit_sprite_structure_lookups;
    uint64_t lit_sprite_structure_hits;
    uint64_t lit_sprite_structure_misses;
    uint64_t lit_sprite_structure_constructions;
    uint64_t lit_sprite_structure_insertions;
    uint64_t lit_sprite_structure_evictions;
    uint64_t lit_sprite_structure_invalidations;
    uint64_t lit_sprite_projected_lookups;
    uint64_t lit_sprite_projected_hits;
    uint64_t lit_sprite_projected_misses;
    uint64_t lit_sprite_projected_constructions;
    uint64_t lit_sprite_projected_insertions;
    uint64_t lit_sprite_projected_evictions;
    uint64_t lit_sprite_projected_invalidations;
    uint64_t lit_sprite_invalidation_field;
    uint64_t lit_sprite_invalidation_scroll;
    uint64_t lit_sprite_invalidation_source;
    uint64_t lit_sprite_invalidation_reset;
    uint64_t lit_sprite_invalidation_eviction;
} lighting_benchmark_counters_t;

/** Current and peak cache state for one protocol depth. */
typedef struct lighting_benchmark_level_statistics {
    lighting_benchmark_counters_t counters;
    lighting_benchmark_timings_t timings;
    int depth;
    bool allocated;
    bool active;
    bool cache_valid;
    bool update_needed;
    int width;
    int height;
    uint64_t cache_key;
    uint64_t pending_cache_key;
    size_t lit_sprite_entries;
    size_t lit_sprite_bytes;
    size_t retained_field_bytes;
    size_t peak_lit_sprite_entries;
    size_t peak_lit_sprite_bytes;
    size_t peak_retained_field_bytes;
    uint64_t state_digest;
} lighting_benchmark_level_statistics_t;

/** Observable software-lighting cache state for deterministic benchmarks. */
typedef struct lighting_benchmark_statistics {
    lighting_benchmark_counters_t counters;
    lighting_benchmark_timings_t timings;
    /* Version-one aliases retained for baseline overlay consumers. */
    uint64_t field_rebuilds;
    uint64_t field_reuses;
    uint64_t lit_sprite_lookups;
    uint64_t lit_sprite_hits;
    uint64_t lit_sprite_misses;
    uint64_t lit_sprite_evictions;
    size_t lit_sprite_entries;
    size_t lit_sprite_bytes;
    size_t retained_field_bytes;
    /** Occupancy gauges captured immediately before the phase. */
    size_t start_allocated_levels;
    size_t start_active_levels;
    size_t start_cache_valid_levels;
    size_t start_dirty_levels;
    size_t start_lit_sprite_entries;
    size_t start_lit_sprite_bytes;
    size_t start_retained_field_bytes;
    uint64_t start_state_digest;
    size_t allocated_levels;
    size_t active_levels;
    size_t cache_valid_levels;
    size_t dirty_levels;
    size_t peak_allocated_levels;
    size_t peak_active_levels;
    size_t peak_cache_valid_levels;
    size_t peak_dirty_levels;
    size_t peak_lit_sprite_entries;
    size_t peak_lit_sprite_bytes;
    size_t peak_retained_field_bytes;
    uint64_t state_digest;
} lighting_benchmark_statistics_t;

uint16_t lighting_srgb8_to_linear(uint8_t value);
uint8_t lighting_linear_to_srgb8(uint16_t value);
uint8_t lighting_radiance_to_level(uint16_t radiance);
void lighting_tone_map_linear(uint16_t scalar, const uint16_t radiance[3], uint16_t linear[3]);
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
/** Return the primary lighting viewport dimensions when its cache exists. */
bool lighting_viewport_size_get(int *width, int *height);
/** Dirty a viewport-relative rectangle in one physical map depth. */
void lighting_dirty_screen_rect(int depth, int x0, int y0, int x1, int y1);
/** Translate cached lighting and dirty newly exposed screen pixels. */
void lighting_scroll(int screen_dx, int screen_dy);
void lighting_draw_quad(const lighting_vertex_t vertices[4]);
void lighting_render(SDL_Surface *destination);

/**
 * Begin a painter-owned scene composition.
 *
 * Map commands record the physical lighting depth that owns each visible
 * pixel while the established painter order is still being emitted. The
 * scene is multiplied once, after all world sprites have been painted.
 */
bool lighting_scene_begin(int width, int height);
void lighting_scene_mark_surface(SDL_Surface *source,
                                 int x,
                                 int y,
                                 SDL_Rect *srcrect,
                                 int depth,
                                 int sample_y);
void lighting_scene_render(SDL_Surface *destination);
void lighting_scene_cancel(void);
void lighting_show_surface(SDL_Surface *destination,
                           int x,
                           int y,
                           SDL_Rect *srcrect,
                           SDL_Surface *source,
                           int sample_y,
                           lighting_surface_mode_t mode);
/** Main-thread only: remove lit results referring to a source before destruction. */
void lighting_invalidate_surface(SDL_Surface *source);
void lighting_clear_sprite_cache(void);
void lighting_deinit(void);

void lighting_benchmark_statistics_reset(void);
void lighting_benchmark_statistics_get(lighting_benchmark_statistics_t *statistics);
/** Copy cumulative operation timings without traversing benchmark state. */
void lighting_benchmark_timings_get(lighting_benchmark_timings_t *timings);
void lighting_benchmark_configure(bool timing_enabled,
                                  lighting_benchmark_reconstruction_t reconstruction);
#ifdef ATRINIK_WIDGET_TESTS
/** Configure and query the movement-only construction fallback seam. */
void lighting_benchmark_fault_configure(unsigned int fault);
bool lighting_benchmark_fault_complete(void);
/** Report whether a source address token is retained by any lit-sprite cache key. */
bool lighting_benchmark_source_address_retained(uintptr_t source_address);
#endif
/** Copy the benchmark state for one protocol depth. */
bool lighting_benchmark_level_statistics_get(int depth,
                                             lighting_benchmark_level_statistics_t *statistics);

#endif
